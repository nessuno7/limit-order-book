//
// Per-operation latency distributions for every continuous-book
// implementation, on the same seeded order flow the fuzzer uses.
//
// Each op is timed on its own with a fenced rdtsc, calibrated against the
// wall clock; the timer's own cost (median of an empty measurement) is
// subtracted. Samples are bucketed by what the op did, and each bucket
// reports p50 / p90 / p99 / p99.9 / p99.99 / max, not a mean: a matching
// engine is judged by its tail. Every figure is the median over R passes.
//
// Allocations are counted inside the timed regions only, by a counting
// global operator new, to check the "zero allocations in the hot path" claim.
//
//   lob_latency [results.json] [--ops N] [--reps R] [--capacity N]
//                                              benchmark (defaults: 2,000,000 ops per scenario,
//                                              5 passes, pool/id-table capacity 65,536)
//   lob_latency --profile engine|map|fast [samples.txt]
//                                              replay one book in a loop, untimed, for perf; on
//                                              Windows, sample it with bench/Sampler.h
//
// Run it on an idle machine; it pins itself to one core at high priority.
//

#include "harness/Adapters.h"
#include "harness/OpGenerator.h"
#include "harness/ReferenceBook.h"
#include "Sampler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <x86intrin.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

// ── counting allocator ───────────────────────────────────────────────────────

namespace {
    std::uint64_t gAllocations = 0;

    void *countedAlloc(const std::size_t n) {
        ++gAllocations;
        if (void *p = std::malloc(n == 0 ? 1 : n)) return p;
        throw std::bad_alloc();
    }

    void release(void *p) noexcept { std::free(p); }
}

void *operator new(const std::size_t n) { return countedAlloc(n); }
void *operator new[](const std::size_t n) { return countedAlloc(n); }
void operator delete(void *p) noexcept { release(p); }
void operator delete[](void *p) noexcept { release(p); }
void operator delete(void *p, std::size_t) noexcept { release(p); }
void operator delete[](void *p, std::size_t) noexcept { release(p); }

namespace {
    // ── timing ───────────────────────────────────────────────────────────────

    inline std::uint64_t tsc() {
        _mm_lfence(); // nothing before may still be in flight ...
        const std::uint64_t t = __rdtsc();
        _mm_lfence(); // ... and nothing after may start early
        return t;
    }

    double calibrateNsPerTick() {
        const auto w0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = tsc();
        while (std::chrono::steady_clock::now() - w0 < std::chrono::milliseconds(300)) {}
        const auto w1 = std::chrono::steady_clock::now();
        const std::uint64_t c1 = tsc();
        return std::chrono::duration<double, std::nano>(w1 - w0).count() / static_cast<double>(c1 - c0);
    }

    std::uint64_t timerOverheadTicks() {
        std::vector<std::uint64_t> samples(200'000);
        for (auto &s: samples) {
            const std::uint64_t t0 = tsc();
            s = tsc() - t0;
        }
        std::nth_element(samples.begin(), samples.begin() + 100'000, samples.end());
        return samples[100'000];
    }

    void pinToOneCore() {
#ifdef _WIN32
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << 2);
#else
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(2, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#endif
    }

    // ── workload ─────────────────────────────────────────────────────────────

    constexpr Price MIN_PRICE = 1;
    constexpr Price MAX_PRICE = 20'000;
    // Capacity of FastBook/MapBook's pool and id table. Sized to the workload
    // (13x the deep book's peak of ~5,000 orders), as a real book would be:
    // oversizing spreads the id table over more cache lines than the live
    // orders need. The engine's unordered_map sizes itself.
    std::size_t MAX_ORDERS = 1 << 16;

    struct Scenario {
        std::string name;
        std::string description;
        std::vector<Op> ops;
        std::size_t maxDepth = 0;
    };

    // The fuzzer's flow, minus deliberate rejections (exceptions are not what
    // we are timing). The reference book tracks which ids are live.
    Scenario makeScenario(const int profile, const std::size_t n) {
        FlowParams params = flowProfile(profile);
        params.badPct = 0;
        Scenario s{flowName(profile), profile == 1 ? "~5,000 resting orders over hundreds of levels"
                                                   : "shallow book, ~50-100 resting orders", {}, 0};
        OpGenerator gen(42, params);
        ReferenceBook ref;
        s.ops.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            s.ops.push_back(gen.next(ref));
            ref.apply(s.ops.back());
            s.maxDepth = std::max(s.maxDepth, ref.restingOrders());
        }
        return s;
    }

    // ── measurement ──────────────────────────────────────────────────────────

    enum Bucket { ADD_REST, ADD_TRADE, CANCEL, MODIFY, ALL, BUCKETS };
    const char *const BUCKET_NAMES[BUCKETS] = {"add, no fill", "add, trades", "cancel", "modify", "all ops"};

    struct Distribution {
        std::size_t count = 0;
        double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    };

    Distribution summarise(std::vector<std::uint32_t> &ticks, const double nsPerTick) {
        Distribution d;
        d.count = ticks.size();
        if (ticks.empty()) return d;
        std::sort(ticks.begin(), ticks.end());
        const auto at = [&](const double q) {
            const auto i = static_cast<std::size_t>(q * static_cast<double>(ticks.size() - 1));
            return static_cast<double>(ticks[i]) * nsPerTick;
        };
        double sum = 0;
        for (const auto t: ticks) sum += t;
        d.mean = sum / static_cast<double>(ticks.size()) * nsPerTick;
        d.p50 = at(0.50);
        d.p90 = at(0.90);
        d.p99 = at(0.99);
        d.p999 = at(0.999);
        d.p9999 = at(0.9999);
        d.max = static_cast<double>(ticks.back()) * nsPerTick;
        return d;
    }

    struct Result {
        std::string impl;
        Distribution buckets[BUCKETS];
        std::uint64_t allocations = 0; // inside the timed regions of accepted ops
        std::uint64_t rejected = 0;    // e.g. cancelling an order that just filled; kept out of the figures
        double opsPerSec = 0;          // untimed replay, prepare() included
    };

    // One pass with a timer around every op. Warm-up is the previous passes
    // (and a first throwaway one), so code, branch predictors and allocator
    // are hot; the book itself starts empty each time.
    template<class Impl>
    Result timedPass(const Scenario &s, const double nsPerTick, const std::uint64_t overhead) {
        Result r;
        r.impl = Impl::NAME;
        std::vector<std::uint32_t> samples[BUCKETS];
        for (auto &v: samples) v.reserve(s.ops.size());

        auto book = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
        for (const Op &op: s.ops) {
            book->prepare(op);
            const std::uint64_t allocBefore = gAllocations;
            std::size_t fills = NOT_RESTING;
            const std::uint64_t t0 = tsc();
            try {
                fills = book->run(op);
            } catch (const std::exception &) {}
            const std::uint64_t t1 = tsc();
            if (fills == NOT_RESTING) {
                ++r.rejected; // a cancel or modify of an order that already filled
                continue;
            }
            r.allocations += gAllocations - allocBefore;
            const std::uint64_t dt = t1 - t0 > overhead ? t1 - t0 - overhead : 0;
            const auto ticks = static_cast<std::uint32_t>(std::min<std::uint64_t>(dt, UINT32_MAX));
            const Bucket b = op.kind == OpKind::Cancel ? CANCEL
                             : op.kind == OpKind::Modify ? MODIFY
                             : fills > 0 ? ADD_TRADE : ADD_REST;
            samples[b].push_back(ticks);
            samples[ALL].push_back(ticks);
        }
        for (int b = 0; b < BUCKETS; ++b) r.buckets[b] = summarise(samples[b], nsPerTick);
        return r;
    }

    // The same replay with no timer around each op.
    template<class Impl>
    double throughputPass(const Scenario &s) {
        auto book = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
        const auto w0 = std::chrono::steady_clock::now();
        for (const Op &op: s.ops) {
            book->prepare(op);
            try { book->run(op); } catch (const std::exception &) {}
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        return static_cast<double>(s.ops.size()) / secs;
    }

    double median(std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    // Each statistic is the median of that statistic over the passes, which
    // discards a pass that an interrupt or a frequency change landed on.
    Result combine(const std::vector<Result> &passes, const std::vector<double> &throughputs) {
        Result r = passes.front();
        const auto med = [&](auto field) {
            std::vector<double> v;
            for (const Result &p: passes) v.push_back(field(p));
            return median(v);
        };
        for (int b = 0; b < BUCKETS; ++b) {
            Distribution &d = r.buckets[b];
            d.mean = med([b](const Result &p) { return p.buckets[b].mean; });
            d.p50 = med([b](const Result &p) { return p.buckets[b].p50; });
            d.p90 = med([b](const Result &p) { return p.buckets[b].p90; });
            d.p99 = med([b](const Result &p) { return p.buckets[b].p99; });
            d.p999 = med([b](const Result &p) { return p.buckets[b].p999; });
            d.p9999 = med([b](const Result &p) { return p.buckets[b].p9999; });
            d.max = med([b](const Result &p) { return p.buckets[b].max; });
        }
        r.allocations = 0;
        for (const Result &p: passes) r.allocations = std::max(r.allocations, p.allocations); // worst pass
        r.opsPerSec = median(throughputs);
        return r;
    }

    // ── output ───────────────────────────────────────────────────────────────

    std::string fmt(const double ns) {
        std::ostringstream out;
        if (ns >= 10'000) out << std::fixed << std::setprecision(1) << ns / 1000 << " us";
        else out << std::fixed << std::setprecision(0) << ns << " ns";
        return out.str();
    }

    void printTable(const Scenario &s, const std::vector<Result> &results) {
        std::cout << "\n### " << s.name << ": " << s.description << " (" << s.ops.size() << " ops, max depth "
                  << s.maxDepth << ")\n\n"
                  << "| book | operation | count | p50 | p90 | p99 | p99.9 | p99.99 | max | mean |\n"
                  << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const Result &r: results) {
            for (int b = 0; b < BUCKETS; ++b) {
                const Distribution &d = r.buckets[b];
                std::cout << "| " << (b == 0 ? r.impl : "") << " | " << BUCKET_NAMES[b] << " | " << d.count
                          << " | " << fmt(d.p50) << " | " << fmt(d.p90) << " | " << fmt(d.p99) << " | "
                          << fmt(d.p999) << " | " << fmt(d.p9999) << " | " << fmt(d.max) << " | " << fmt(d.mean)
                          << " |\n";
            }
        }
        std::cout << "\n| book | throughput (untimed replay) | heap allocations in timed ops | per 1,000 ops | rejected ops |\n"
                  << "|---|---:|---:|---:|---:|\n";
        for (const Result &r: results) {
            std::cout << "| " << r.impl << " | " << std::fixed << std::setprecision(1) << r.opsPerSec / 1e6
                      << " M ops/s | " << r.allocations << " | " << std::setprecision(1)
                      << 1000.0 * static_cast<double>(r.allocations) / static_cast<double>(s.ops.size()) << " | "
                      << r.rejected << " |\n";
        }
    }

    void writeJson(const std::string &path, const double nsPerTick, const std::uint64_t overhead,
                   const std::vector<std::pair<Scenario *, std::vector<Result>>> &all) {
        std::ofstream out(path);
        out << std::fixed << std::setprecision(1);
        out << "{\n  \"ns_per_tick\": " << std::setprecision(6) << nsPerTick << ",\n  \"timer_overhead_ns\": "
            << std::setprecision(1) << static_cast<double>(overhead) * nsPerTick << ",\n  \"scenarios\": [\n";
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto &[s, results] = all[i];
            out << "    {\"name\": \"" << s->name << "\", \"ops\": " << s->ops.size() << ", \"max_depth\": "
                << s->maxDepth << ", \"results\": [\n";
            for (std::size_t j = 0; j < results.size(); ++j) {
                const Result &r = results[j];
                out << "      {\"impl\": \"" << r.impl << "\", \"ops_per_sec\": " << r.opsPerSec
                    << ", \"allocations_in_timed_ops\": " << r.allocations << ", \"rejected\": " << r.rejected
                    << ", \"latency_ns\": {";
                for (int b = 0; b < BUCKETS; ++b) {
                    const Distribution &d = r.buckets[b];
                    out << (b ? ", " : "") << "\"" << BUCKET_NAMES[b] << "\": {\"count\": " << d.count
                        << ", \"p50\": " << d.p50 << ", \"p90\": " << d.p90 << ", \"p99\": " << d.p99
                        << ", \"p99.9\": " << d.p999 << ", \"p99.99\": " << d.p9999 << ", \"max\": " << d.max
                        << ", \"mean\": " << d.mean << "}";
                }
                out << "}}" << (j + 1 < results.size() ? "," : "") << "\n";
            }
            out << "    ]}" << (i + 1 < all.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
    }

    template<class Impl>
    void profileLoop(const Scenario &s) {
        for (int rep = 0; rep < 10; ++rep) {
            auto book = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
            for (const Op &op: s.ops) {
                book->prepare(op);
                try { book->run(op); } catch (const std::exception &) {}
            }
        }
    }
}

int main(const int argc, char **argv) {
    std::string jsonPath;
    std::string profileImpl;
    std::size_t n = 2'000'000;
    int reps = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--ops" && i + 1 < argc) n = std::stoull(argv[++i]);
        else if (arg == "--reps" && i + 1 < argc) reps = std::stoi(argv[++i]);
        else if (arg == "--capacity" && i + 1 < argc) MAX_ORDERS = std::stoull(argv[++i]);
        else if (arg == "--profile" && i + 1 < argc) profileImpl = argv[++i];
        else jsonPath = arg;
    }

    if (!profileImpl.empty()) {
        const Scenario s = makeScenario(1, n);
        std::cout << "profiling " << profileImpl << " on " << s.ops.size() << " deep-book ops x 10\n";
#ifdef _WIN32
        Sampler sampler; // perf's job on Linux; see bench/Sampler.h
#endif
        if (profileImpl == "engine") profileLoop<EngineAdapter>(s);
        else if (profileImpl == "map") profileLoop<MapBookAdapter>(s);
        else profileLoop<FastBookAdapter>(s);
#ifdef _WIN32
        sampler.stop();
        const std::string out = jsonPath.empty() ? "samples.txt" : jsonPath;
        sampler.write(out);
        std::cout << sampler.size() << " samples written to " << out << "\n";
#endif
        return 0;
    }

    pinToOneCore();
    const double nsPerTick = calibrateNsPerTick();
    const std::uint64_t overhead = timerOverheadTicks();
    std::cout << "TSC: " << std::fixed << std::setprecision(3) << 1.0 / nsPerTick << " GHz, timer overhead "
              << std::setprecision(1) << static_cast<double>(overhead) * nsPerTick << " ns (subtracted)\n";

    std::vector<std::unique_ptr<Scenario>> scenarios;
    std::vector<std::pair<Scenario *, std::vector<Result>>> all;
    for (const int profile: {0, 1}) {
        scenarios.push_back(std::make_unique<Scenario>(makeScenario(profile, n)));
        Scenario &s = *scenarios.back();
        // Passes are interleaved across the books, so a noisy stretch of
        // time hurts one pass of each rather than every pass of one book.
        std::vector<Result> passes[3];
        std::vector<double> throughputs[3];
        for (int rep = -1; rep < reps; ++rep) { // rep -1 is the warm-up, discarded
            Result e = timedPass<EngineAdapter>(s, nsPerTick, overhead);
            Result m = timedPass<MapBookAdapter>(s, nsPerTick, overhead);
            Result f = timedPass<FastBookAdapter>(s, nsPerTick, overhead);
            const double te = throughputPass<EngineAdapter>(s);
            const double tm = throughputPass<MapBookAdapter>(s);
            const double tf = throughputPass<FastBookAdapter>(s);
            if (rep < 0) continue;
            passes[0].push_back(std::move(e));
            passes[1].push_back(std::move(m));
            passes[2].push_back(std::move(f));
            throughputs[0].push_back(te);
            throughputs[1].push_back(tm);
            throughputs[2].push_back(tf);
        }
        std::vector<Result> results;
        for (int i = 0; i < 3; ++i) results.push_back(combine(passes[i], throughputs[i]));
        printTable(s, results);
        all.emplace_back(&s, std::move(results));
    }
    if (!jsonPath.empty()) {
        writeJson(jsonPath, nsPerTick, overhead, all);
        std::cout << "\nwritten to " << jsonPath << "\n";
    }
    return 0;
}
