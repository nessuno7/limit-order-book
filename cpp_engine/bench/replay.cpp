//
// C++ twin of benchmark/replay.py: replays a workload file (see
// benchmark/workload.py) and prints one JSON line with the same keys, so
// benchmark/compare.py can check that both engines produced identical fills
// and compare their time and memory.
//
//   lob_replay <workload.csv> [reps] [trades_dump.csv]
//
// Memory: a counting global operator new/delete gives the peak heap growth
// during one replay -- the analogue of tracemalloc's peak in the Python run.
// Its bookkeeping is two additions per allocation, so the timed runs keep it.
// On MinGW libstdc++ is linked statically, so that allocations made inside it
// also go through the counter (a DLL would keep its own operator new).
//
// What is timed: constructing each Order, submit(), cancel(), run() -- not
// file parsing.
//

#include "call_auction/CallAuction.h"
#include "call_auction/rules/Discriminatory.h"
#include "call_auction/rules/Uniform.h"
#include "continuous_auction/ContinuousDoubleAuction.h"
#include "orderbook/OrderArena.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

// ── counting allocator ───────────────────────────────────────────────────────

namespace {
    std::size_t gLive = 0;
    std::size_t gPeak = 0;
    constexpr std::size_t HEADER = 16; // keeps the returned block max-aligned

    void *countedAlloc(std::size_t n) {
        void *base = std::malloc(n + HEADER);
        if (base == nullptr) throw std::bad_alloc();
        *static_cast<std::size_t *>(base) = n;
        gLive += n;
        gPeak = std::max(gPeak, gLive);
        return static_cast<char *>(base) + HEADER;
    }

    void countedFree(void *p) noexcept {
        if (p == nullptr) return;
        void *base = static_cast<char *>(p) - HEADER;
        gLive -= *static_cast<std::size_t *>(base);
        std::free(base);
    }
}

void *operator new(std::size_t n) { return countedAlloc(n); }
void *operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void *p) noexcept { countedFree(p); }
void operator delete[](void *p) noexcept { countedFree(p); }
void operator delete(void *p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void *p, std::size_t) noexcept { countedFree(p); }

namespace {
    // (current, peak) resident set size of this process.
    std::pair<std::size_t, std::size_t> rssBytes() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc{};
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        return {pmc.WorkingSetSize, pmc.PeakWorkingSetSize};
#else
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
        const auto peak = static_cast<std::size_t>(ru.ru_maxrss);
#else
        const auto peak = static_cast<std::size_t>(ru.ru_maxrss) * 1024;
#endif
        std::size_t current = 0;
        if (std::FILE *f = std::fopen("/proc/self/statm", "r")) {
            unsigned long size = 0, resident = 0;
            if (std::fscanf(f, "%lu %lu", &size, &resident) == 2) {
                current = resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
            }
            std::fclose(f);
        }
        return {current, peak};
#endif
    }

    // ── workload ─────────────────────────────────────────────────────────────

    // datetime(2026, 1, 1, 9, 0, 0), as in replay.py; only differences matter.
    const Datetime T0 = Datetime{} + std::chrono::hours(24 * 20454 + 9);
    constexpr TraderId TRADER = 1;

    struct Event {
        char op;
        std::size_t idx;
        Side side;
        std::optional<Price> price;
        Quantity qty;
        TimeInForce tif;
        Datetime ts;
    };

    struct Workload {
        std::string kind;
        std::string rule;
        std::vector<Event> events;
    };

    std::vector<std::string> split(const std::string &line) {
        std::vector<std::string> out;
        std::string field;
        std::istringstream in(line);
        while (std::getline(in, field, ',')) out.push_back(field);
        while (out.size() < 7) out.emplace_back();
        return out;
    }

    Workload load(const std::string &path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open " + path);
        Workload w;
        std::string line;
        std::getline(in, line); // "# kind=... rule=..."
        std::istringstream meta(line.substr(2));
        std::string kv;
        while (meta >> kv) {
            const auto eq = kv.find('=');
            (kv.substr(0, eq) == "kind" ? w.kind : w.rule) = kv.substr(eq + 1);
        }
        std::getline(in, line); // column header
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto f = split(line);
            Event e{f[0][0], 0, Side::BUY, std::nullopt, 0, TimeInForce::GTC, T0};
            if (e.op == 'N' || e.op == 'C') e.idx = std::stoul(f[1]);
            if (e.op == 'N') {
                e.side = f[2] == "B" ? Side::BUY : Side::SELL;
                if (!f[3].empty()) e.price = std::stoll(f[3]);
                e.qty = std::stoll(f[4]);
                e.tif = f[5] == "G" ? TimeInForce::GTC : f[5] == "I" ? TimeInForce::IOC : TimeInForce::FOK;
                e.ts = T0 + std::chrono::microseconds(std::stoll(f[6]));
            }
            w.events.push_back(e);
        }
        return w;
    }

    // Everything one replay creates; destroyed together after it is measured.
    struct Run {
        std::unique_ptr<PricingRule> rule;
        std::unique_ptr<MatchingEngine> engine;
        OrderArena arena;
        std::vector<Order *> orders;
    };

    std::unique_ptr<Run> replay(const Workload &w) {
        auto run = std::make_unique<Run>();
        ContinuousDoubleAuction *continuous = nullptr;
        CallAuction *call = nullptr;
        if (w.kind == "continuous") {
            auto e = std::make_unique<ContinuousDoubleAuction>();
            continuous = e.get();
            run->engine = std::move(e);
        } else {
            if (w.rule == "uniform") run->rule = std::make_unique<Uniform>();
            else run->rule = std::make_unique<Discriminatory>();
            auto e = std::make_unique<CallAuction>(*run->rule);
            call = e.get();
            run->engine = std::move(e);
        }
        MatchingEngine &engine = *run->engine;

        for (const Event &e: w.events) {
            if (e.op == 'N') {
                Order *o = run->arena.create(e.side, e.price, e.qty, TRADER, e.tif, e.ts);
                run->orders.push_back(o);
                engine.submit(o);
            } else if (e.op == 'C') {
                const OrderId id = run->orders[e.idx]->orderId;
                if (continuous->book.index.count(id) != 0) continuous->book.cancel(id);
            } else {
                call->run();
            }
        }
        return run;
    }

    // ── fingerprints: identical across languages iff the outcomes are ────────

    struct Fnv1a {
        std::uint64_t h = 0xcbf29ce484222325ULL;

        void add(const std::string &s) {
            for (const char c: s) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
        }

        [[nodiscard]] std::string hex() const {
            char buf[17];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
            return buf;
        }
    };

    struct Fingerprint {
        std::size_t trades = 0;
        Quantity volume = 0;
        Quantity notional = 0;
        std::string tradeDigest, stateDigest;
        std::string tradeLog;

        bool operator==(const Fingerprint &o) const {
            return trades == o.trades && volume == o.volume && notional == o.notional
                   && tradeDigest == o.tradeDigest && stateDigest == o.stateDigest;
        }
    };

    Fingerprint fingerprint(const Workload &w, const Run &run) {
        std::unordered_map<OrderId, std::size_t> idx;
        for (std::size_t i = 0; i < run.orders.size(); ++i) idx[run.orders[i]->orderId] = i;

        Fingerprint fp;
        Fnv1a trades;
        for (const Trade &t: run.engine->trades) {
            const std::string line = std::to_string(idx.at(t.buyOrderId)) + "," + std::to_string(idx.at(t.sellOrderId))
                                     + "," + std::to_string(t.price) + "," + std::to_string(t.quantity) + "\n";
            trades.add(line);
            fp.tradeLog += line;
            fp.volume += t.quantity;
            fp.notional += t.quantity * t.price;
        }
        fp.trades = run.engine->trades.size();
        fp.tradeDigest = trades.hex();

        Fnv1a state;
        if (w.kind == "continuous") {
            const OrderBook &book = run.engine->book;
            const std::pair<const char *, const std::map<Price, std::deque<OrderId>> *> sides[] = {
                {"B", &book.bids}, {"S", &book.asks}};
            for (const auto &[tag, levels]: sides) {
                for (const auto &[price, queue]: *levels) {
                    for (const OrderId id: queue) {
                        state.add(std::string(tag) + "," + std::to_string(price) + "," + std::to_string(idx.at(id))
                                  + "," + std::to_string(book.index.at(id)->quantity) + "\n");
                    }
                }
            }
        } else {
            std::vector<std::pair<std::size_t, Quantity>> pending;
            for (const Order *o: dynamic_cast<const CallAuction &>(*run.engine).getPending()) {
                pending.emplace_back(idx.at(o->orderId), o->quantity);
            }
            std::sort(pending.begin(), pending.end());
            for (const auto &[i, q]: pending) state.add(std::to_string(i) + "," + std::to_string(q) + "\n");
        }
        fp.stateDigest = state.hex();
        return fp;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: lob_replay <workload.csv> [reps] [trades_dump.csv]\n";
        return 2;
    }
    const std::string path = argv[1];
    const int reps = argc > 2 ? std::stoi(argv[2]) : 10;

    const Workload w = load(path);
    std::string scenario = path.substr(path.find_last_of("/\\") + 1);
    scenario = scenario.substr(0, scenario.find_last_of('.'));

    const auto [rssBefore, rssPeakBefore] = rssBytes();
    (void) rssPeakBefore;

    std::vector<double> times;
    std::size_t heapPeak = 0;
    std::size_t orders = 0;
    std::optional<Fingerprint> first;
    for (int r = 0; r < reps; ++r) {
        const std::size_t liveBefore = gLive;
        gPeak = gLive;
        const auto t0 = std::chrono::steady_clock::now();
        const auto run = replay(w);
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double>(t1 - t0).count());
        heapPeak = std::max(heapPeak, gPeak - liveBefore);
        orders = run->orders.size();

        Fingerprint fp = fingerprint(w, *run);
        if (first.has_value() && !(*first == fp)) {
            std::cerr << "replay is not deterministic\n";
            return 1;
        }
        if (!first.has_value()) first = std::move(fp);
    }
    const auto [rssNow, rssPeak] = rssBytes();
    (void) rssNow;

    if (argc > 3) std::ofstream(argv[3], std::ios::binary) << first->tradeLog;

    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.size() % 2 == 1
                              ? sorted[sorted.size() / 2]
                              : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
    const double nEvents = static_cast<double>(w.events.size());

    std::cout.precision(9);
    std::cout << "{\"language\": \"cpp\", \"scenario\": \"" << scenario << "\""
              << ", \"events\": " << w.events.size()
              << ", \"orders\": " << orders
              << ", \"trades\": " << first->trades
              << ", \"volume\": " << first->volume
              << ", \"notional\": " << first->notional
              << ", \"trade_digest\": \"" << first->tradeDigest << "\""
              << ", \"state_digest\": \"" << first->stateDigest << "\""
              << ", \"reps\": " << reps
              << ", \"time_s_median\": " << median
              << ", \"time_s_min\": " << sorted.front()
              << ", \"events_per_sec\": " << nEvents / median
              << ", \"ns_per_event\": " << median / nEvents * 1e9
              << ", \"heap_peak_bytes\": " << heapPeak
              << ", \"rss_baseline_bytes\": " << rssBefore
              << ", \"rss_peak_bytes\": " << rssPeak
              << "}\n";
    return 0;
}
