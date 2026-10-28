//
// Google Benchmark suite for the three continuous books. Complements
// latency.cpp: this measures steady-state throughput of isolated operations
// with Google Benchmark's statistics (repetitions, CV), where latency.cpp
// measures the per-op distribution on a realistic mixed flow.
//
// Built only through CMake, with -DLOB_GBENCH=ON (fetches google/benchmark):
//   cmake -S . -B build-gbench -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOB_GBENCH=ON
//   cmake --build build-gbench --target lob_gbench
//   build-gbench/lob_gbench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
//
// The engine's figures include building its Order object (EngineAdapter::
// prepare); the fast books take plain arguments and have no such step.
// Iteration counts are fixed: the engine's OrderArena never frees, so a
// free-running benchmark would grow its memory without bound.
//

#include "harness/Adapters.h"
#include "harness/OpGenerator.h"
#include "harness/ReferenceBook.h"

#include <benchmark/benchmark.h>

#include <memory>
#include <random>
#include <vector>

namespace {
    constexpr Price MIN_PRICE = 1;
    constexpr Price MAX_PRICE = 20'000;
    constexpr std::size_t MAX_ORDERS = 1 << 16;
    constexpr Price MID = 10'000;

    Op add(const OrderId id, const Side side, const std::optional<Price> price, const Quantity qty,
           const TimeInForce tif = TimeInForce::GTC) {
        Op op = Op::of(OpKind::Add, id);
        op.side = side;
        op.price = price;
        op.quantity = qty;
        op.tif = tif;
        return op;
    }

    template<class Impl>
    std::size_t apply(Impl &book, const Op &op) {
        book.prepare(op);
        return book.run(op);
    }

    // A two-sided book with `depth` resting orders of `lot` per side, spread
    // over `levels` price levels, one tick either side of MID.
    template<class Impl>
    std::unique_ptr<Impl> prefilled(const int depth, const int levels, OrderId &nextId, const Quantity lot = 10) {
        auto book = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
        for (int i = 0; i < depth; ++i) {
            const Price offset = 1 + i % levels;
            apply(*book, add(nextId++, Side::BUY, MID - offset, lot));
            apply(*book, add(nextId++, Side::SELL, MID + offset, lot));
        }
        return book;
    }

    // A passive order joins a random level behind the touch, then is cancelled.
    template<class Impl>
    void BM_AddCancel(benchmark::State &state) {
        OrderId id = 1;
        auto book = prefilled<Impl>(static_cast<int>(state.range(0)), 200, id);
        std::mt19937 rng(1);
        for (auto _: state) {
            const OrderId mine = id++;
            const Side side = rng() % 2 ? Side::BUY : Side::SELL;
            const Price offset = 1 + static_cast<Price>(rng() % 200);
            benchmark::DoNotOptimize(apply(*book, add(mine, side, side == Side::BUY ? MID - offset : MID + offset, 5)));
            Op cancel = Op::of(OpKind::Cancel, mine);
            benchmark::DoNotOptimize(apply(*book, cancel));
        }
        state.SetItemsProcessed(2 * state.iterations());
    }

    // An IOC buy takes the front 1-lot order at the touch; a new 1-lot ask
    // joins the back, so the book's shape is constant.
    template<class Impl>
    void BM_MatchAndReplenish(benchmark::State &state) {
        OrderId id = 1;
        auto book = prefilled<Impl>(static_cast<int>(state.range(0)), 200, id, 1);
        for (auto _: state) {
            benchmark::DoNotOptimize(apply(*book, add(id++, Side::BUY, MID + 1, 1, TimeInForce::IOC)));
            benchmark::DoNotOptimize(apply(*book, add(id++, Side::SELL, MID + 1, 1)));
        }
        state.SetItemsProcessed(2 * state.iterations());
    }

    // A market order sweeps 10 levels; the levels are rebuilt.
    template<class Impl>
    void BM_Sweep10Levels(benchmark::State &state) {
        OrderId id = 1;
        auto book = prefilled<Impl>(0, 1, id);
        for (auto _: state) {
            for (Price p = MID + 1; p <= MID + 10; ++p) apply(*book, add(id++, Side::SELL, p, 1));
            benchmark::DoNotOptimize(apply(*book, add(id++, Side::BUY, std::nullopt, 10)));
        }
        state.SetItemsProcessed(11 * state.iterations());
    }

    template<class Impl>
    void BM_BestBidAsk(benchmark::State &state) {
        OrderId id = 1;
        const auto book = prefilled<Impl>(static_cast<int>(state.range(0)), 200, id);
        for (auto _: state) {
            benchmark::DoNotOptimize(book->bestBid());
            benchmark::DoNotOptimize(book->bestAsk());
        }
    }

    // The latency benchmark's deep-book flow, replayed end to end.
    const std::vector<Op> &deepFlow() {
        static const std::vector<Op> ops = [] {
            FlowParams params = flowProfile(1);
            params.badPct = 0;
            OpGenerator gen(42, params);
            ReferenceBook ref;
            std::vector<Op> v;
            v.reserve(200'000);
            for (int i = 0; i < 200'000; ++i) {
                v.push_back(gen.next(ref));
                ref.apply(v.back());
            }
            return v;
        }();
        return ops;
    }

    template<class Impl>
    void BM_ReplayDeepFlow(benchmark::State &state) {
        const auto &ops = deepFlow();
        for (auto _: state) {
            auto book = std::make_unique<Impl>(MIN_PRICE, MAX_PRICE, MAX_ORDERS);
            for (const Op &op: ops) {
                try {
                    benchmark::DoNotOptimize(apply(*book, op));
                } catch (const std::exception &) {}
            }
        }
        state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(ops.size()));
    }
}

#define LOB_BENCH_ALL(fn, ...)                                    \
    BENCHMARK_TEMPLATE(fn, EngineAdapter) __VA_ARGS__;            \
    BENCHMARK_TEMPLATE(fn, MapBookAdapter) __VA_ARGS__;           \
    BENCHMARK_TEMPLATE(fn, FastBookAdapter) __VA_ARGS__

LOB_BENCH_ALL(BM_AddCancel, ->Arg(100)->Arg(5000)->Iterations(1'000'000));
LOB_BENCH_ALL(BM_MatchAndReplenish, ->Arg(100)->Arg(5000)->Iterations(1'000'000));
LOB_BENCH_ALL(BM_Sweep10Levels, ->Iterations(200'000));
LOB_BENCH_ALL(BM_BestBidAsk, ->Arg(5000));
LOB_BENCH_ALL(BM_ReplayDeepFlow, ->Unit(benchmark::kMillisecond)->Iterations(10));

BENCHMARK_MAIN();
