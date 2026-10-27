//
// Differential fuzzer: every continuous-book implementation against the naive
// ReferenceBook, on millions of seeded random add / cancel / modify ops.
//
// After every op, each implementation must report exactly the reference's
// outcome (status and the full fill list, in order) and the same best bid,
// best ask and resting count. Every --check-every ops, and at the end, the
// full book (every resting order, in priority order) must match as well.
//
// On the first divergence it prints the seed, the op number, the op, both
// outcomes and the ops leading up to it, and exits 1. Replay a failure with
//   lob_fuzz --seed <seed> --ops <op number + 1>
//
//   lob_fuzz [--ops N] [--seeds K] [--seed S] [--check-every N] [--impl engine|map|fast|all]
//

#include "harness/adapters.h"
#include "harness/op_generator.h"
#include "harness/reference_book.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
    struct Config {
        std::uint64_t ops = 1'000'000; // per seed
        std::uint64_t seeds = 5;
        std::uint64_t firstSeed = 1;
        std::uint64_t checkEvery = 1'000;
        std::string impl = "all";
    };

    Config parse(const int argc, char **argv) {
        Config c;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    std::cerr << arg << " needs a value\n";
                    std::exit(2);
                }
                return argv[++i];
            };
            if (arg == "--ops") c.ops = std::stoull(value());
            else if (arg == "--seeds") c.seeds = std::stoull(value());
            else if (arg == "--seed") { c.firstSeed = std::stoull(value()); c.seeds = 1; }
            else if (arg == "--check-every") c.checkEvery = std::stoull(value());
            else if (arg == "--impl") c.impl = value();
            else {
                std::cerr << "usage: lob_fuzz [--ops N] [--seeds K] [--seed S] [--check-every N] "
                             "[--impl engine|map|fast|all]\n";
                std::exit(2);
            }
        }
        return c;
    }

    // The fuzzer keeps orders well inside the ladder, so FastBook's price
    // range never rejects anything the reference would accept.
    constexpr Price MIN_PRICE = 1;
    constexpr Price MAX_PRICE = 20'000;
    constexpr std::size_t MAX_ORDERS = 1 << 20;

    struct Stats {
        std::uint64_t adds = 0, cancels = 0, modifies = 0, fills = 0, rejected = 0, killed = 0;
        std::uint64_t fullChecks = 0;
        std::size_t maxDepth = 0;
    };

    struct Divergence {
        std::string what;
    };

    template<class Impl>
    void compareTop(const Impl &impl, const ReferenceBook &ref) {
        if (impl.bestBid() != ref.bestBid() || impl.bestAsk() != ref.bestAsk()) {
            throw Divergence{"best bid/ask differ"};
        }
        if (impl.restingOrders() != ref.restingOrders()) {
            throw Divergence{"resting count " + std::to_string(impl.restingOrders()) + " vs reference "
                             + std::to_string(ref.restingOrders())};
        }
    }

    template<class Impl>
    void compareFull(const Impl &impl, const ReferenceBook &ref) {
        const Snapshot got = impl.snapshot();
        const Snapshot want = ref.snapshot();
        if (got == want) return;
        std::ostringstream out;
        out << "book differs (" << got.size() << " vs " << want.size() << " resting orders)";
        for (std::size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
            if (i >= got.size() || i >= want.size() || !(got[i] == want[i])) {
                out << "\n    first difference at position " << i << ": ";
                if (i < got.size()) out << got[i];
                else out << "(none)";
                out << "  vs reference  ";
                if (i < want.size()) out << want[i];
                else out << "(none)";
                break;
            }
        }
        throw Divergence{out.str()};
    }

    // One seed against one implementation. Returns false on divergence.
    template<class Impl>
    bool fuzzOne(const std::uint64_t seed, const Config &cfg, Stats &stats) {
        ReferenceBook ref;
        auto impl = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
        OpGenerator gen(seed, flowProfile(static_cast<int>(seed % 3)));
        std::deque<std::string> history; // the last few ops, for the report

        for (std::uint64_t n = 0; n < cfg.ops; ++n) {
            const Op op = gen.next(ref);
            std::ostringstream line;
            line << "#" << n << "  " << op;
            history.push_back(line.str());
            if (history.size() > 12) history.pop_front();

            const Outcome want = ref.apply(op);
            const Outcome got = impl->apply(op);
            try {
                if (!(got == want)) {
                    std::ostringstream out;
                    out << "outcome differs\n    impl:      " << got << "\n    reference: " << want;
                    throw Divergence{out.str()};
                }
                compareTop(*impl, ref);
                if (cfg.checkEvery != 0 && (n + 1) % cfg.checkEvery == 0) {
                    compareFull(*impl, ref);
                    ++stats.fullChecks;
                }
            } catch (const Divergence &d) {
                std::cout << "\nDIVERGENCE  impl=" << Impl::NAME << "  seed=" << seed << "  op=" << n << "\n  "
                          << d.what << "\n  recent ops:\n";
                for (const auto &h: history) std::cout << "    " << h << "\n";
                std::cout << "  replay: lob_fuzz --seed " << seed << " --ops " << n + 1 << " --check-every 1\n";
                return false;
            }

            switch (op.kind) {
                case OpKind::Add: ++stats.adds; break;
                case OpKind::Cancel: ++stats.cancels; break;
                case OpKind::Modify: ++stats.modifies; break;
            }
            if (want.status != Status::Ok) ++stats.rejected;
            if (op.kind == OpKind::Add && op.tif == TimeInForce::FOK && want.fills.empty()) ++stats.killed;
            stats.fills += want.fills.size();
            stats.maxDepth = std::max(stats.maxDepth, ref.restingOrders());
        }

        try {
            compareFull(*impl, ref);
            ++stats.fullChecks;
        } catch (const Divergence &d) {
            std::cout << "\nDIVERGENCE at end  impl=" << Impl::NAME << "  seed=" << seed << "\n  " << d.what << "\n";
            return false;
        }
        return true;
    }

    template<class Impl>
    bool fuzzAll(const Config &cfg) {
        Stats stats;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::uint64_t s = cfg.firstSeed; s < cfg.firstSeed + cfg.seeds; ++s) {
            if (!fuzzOne<Impl>(s, cfg, stats)) return false;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::uint64_t total = cfg.ops * cfg.seeds;
        std::cout << std::left << std::setw(28) << Impl::NAME << std::right
                  << "  ok  " << total << " ops (" << stats.adds << " adds, " << stats.cancels << " cancels, "
                  << stats.modifies << " modifies), " << stats.fills << " fills identical, "
                  << stats.rejected << " rejections, " << stats.killed << " FOKs killed, "
                  << stats.fullChecks << " full-book checks, max depth " << stats.maxDepth << ", "
                  << std::fixed << std::setprecision(1) << secs << " s\n";
        return true;
    }
}

int main(const int argc, char **argv) {
    const Config cfg = parse(argc, argv);
    std::cout << "fuzzing against ReferenceBook: " << cfg.seeds << " seed(s) from " << cfg.firstSeed << " x "
              << cfg.ops << " ops, full-book check every " << cfg.checkEvery << " ops\n";

    bool ok = true;
    if (cfg.impl == "all" || cfg.impl == "engine") ok = ok && fuzzAll<EngineAdapter>(cfg);
    if (cfg.impl == "all" || cfg.impl == "map") ok = ok && fuzzAll<MapBookAdapter>(cfg);
    if (cfg.impl == "all" || cfg.impl == "fast") ok = ok && fuzzAll<FastBookAdapter>(cfg);
    return ok ? 0 : 1;
}
