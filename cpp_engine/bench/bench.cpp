//
// Port of benchmark/bench.py: the same five workloads, parameters and JSON
// schema, so the two results files can be diffed key by key.
//
// Differences from the Python, by necessity:
//   - std::mt19937(42) does not reproduce CPython's random stream, so the
//     random prices/quantities differ (same ranges, same distribution).
//   - there is no tracemalloc, so "peak_memory_bytes" is omitted (it is an
//     optional key in the schema).
//
// Usage: lob_bench [output.json]   (default ../results/cpp_microbench.json)
//

#include "continuous_auction/ContinuousDoubleAuction.h"
#include "orderbook/OrderArena.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // Keep these identical to PARAMS in benchmark/bench.py.
    struct Params {
        unsigned seed = 42;
        int nOrders = 10'000;
        int buyLo = 80, buyHi = 99;
        int sellLo = 101, sellHi = 120;
        Price matchPrice = 100;
        int qtyLo = 1, qtyHi = 20;
        int nWarmup = 200;
    } const PARAMS;

    constexpr TraderId TRADER = 1;
    std::mt19937 RNG(PARAMS.seed);

    int randInt(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(RNG); }

    Order *lmt(OrderArena &arena, Side side, Price price, Quantity qty) {
        return arena.create(side, price, qty, TRADER);
    }

    std::int64_t timeNs(const std::function<void()> &fn) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    struct Summary {
        std::string workload;
        std::size_t n;
        double meanUs, p50Us, p95Us, p99Us, maxUs, throughputKops;
    };

    double round(double x, int places) {
        const double scale = std::pow(10.0, places);
        return std::round(x * scale) / scale;
    }

    Summary summarise(const std::string &name, const std::vector<std::int64_t> &latenciesNs) {
        std::vector<double> us;
        us.reserve(latenciesNs.size());
        for (const auto ns: latenciesNs) us.push_back(static_cast<double>(ns) / 1'000.0);
        std::vector<double> sorted = us;
        std::sort(sorted.begin(), sorted.end());
        // Same as bench.py's percentile(): sorted(data)[int(p * len(data))]
        const auto pct = [&sorted](double p) {
            return sorted[static_cast<std::size_t>(p * static_cast<double>(sorted.size()))];
        };
        const double mean = std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
        return Summary{name, us.size(), round(mean, 3), round(pct(0.50), 3), round(pct(0.95), 3),
                       round(pct(0.99), 3), round(sorted.back(), 3), round(1'000.0 / mean, 1)};
    }

    // ── Workloads ───────────────────────────────────────────────────────────

    Summary benchInsertNoMatch(int n) {
        OrderArena arena;
        std::vector<Order *> buys, sells, orders;
        for (int i = 0; i < n; ++i) buys.push_back(lmt(arena, Side::BUY, randInt(PARAMS.buyLo, PARAMS.buyHi), randInt(PARAMS.qtyLo, PARAMS.qtyHi)));
        for (int i = 0; i < n; ++i) sells.push_back(lmt(arena, Side::SELL, randInt(PARAMS.sellLo, PARAMS.sellHi), randInt(PARAMS.qtyLo, PARAMS.qtyHi)));
        for (int i = 0; i < n; ++i) {
            orders.push_back(buys[static_cast<std::size_t>(i)]);
            orders.push_back(sells[static_cast<std::size_t>(i)]);
        }

        { // warmup: copies, so the timed run starts from the same fresh orders
            OrderArena warm;
            ContinuousDoubleAuction e;
            for (int i = 0; i < PARAMS.nWarmup; ++i) e.submit(warm.copy(*orders[static_cast<std::size_t>(i)]));
        }

        ContinuousDoubleAuction e;
        std::vector<std::int64_t> latencies;
        for (Order *o: orders) latencies.push_back(timeNs([&] { e.submit(o); }));
        return summarise("insert_no_match", latencies);
    }

    Summary benchSubmitMatch1v1(int n) {
        OrderArena arena;
        for (int i = 0; i < PARAMS.nWarmup; ++i) {
            ContinuousDoubleAuction e;
            e.submit(lmt(arena, Side::SELL, PARAMS.matchPrice, 1));
            e.submit(lmt(arena, Side::BUY, PARAMS.matchPrice, 1));
        }

        ContinuousDoubleAuction e;
        for (int i = 0; i < n; ++i) e.submit(lmt(arena, Side::SELL, PARAMS.matchPrice, 1));
        std::vector<std::int64_t> latencies;
        for (int i = 0; i < n; ++i) {
            // Order construction is inside the timed region, as in bench.py.
            latencies.push_back(timeNs([&] { e.submit(lmt(arena, Side::BUY, PARAMS.matchPrice, 1)); }));
        }
        return summarise("submit_match_1v1", latencies);
    }

    Summary benchSubmitSweep(int n) {
        const Quantity sweepQty = n; // one unit per level, the aggressor buys all of them
        OrderArena arena;
        for (int i = 0; i < PARAMS.nWarmup; ++i) {
            ContinuousDoubleAuction e;
            for (Price p = 101; p < 101 + n; ++p) e.submit(lmt(arena, Side::SELL, p, 1));
            e.submit(lmt(arena, Side::BUY, 100 + n, sweepQty));
        }

        std::vector<std::int64_t> latencies;
        for (int rep = 0; rep < std::max(1, PARAMS.nWarmup / 10); ++rep) { // fewer reps: each is expensive
            ContinuousDoubleAuction e;
            for (Price p = 101; p < 101 + n; ++p) e.submit(lmt(arena, Side::SELL, p, 1));
            latencies.push_back(timeNs([&] { e.submit(lmt(arena, Side::BUY, 100 + n, sweepQty)); }));
        }
        return summarise("submit_sweep", latencies);
    }

    Summary benchCancel(int n) {
        OrderArena arena;
        std::vector<Order *> orders;
        for (Price p = 1; p <= n; ++p) orders.push_back(lmt(arena, Side::BUY, p, randInt(PARAMS.qtyLo, PARAMS.qtyHi)));

        {
            OrderArena warm;
            ContinuousDoubleAuction e;
            std::vector<Order *> copies;
            for (int i = 0; i < PARAMS.nWarmup; ++i) copies.push_back(warm.copy(*orders[static_cast<std::size_t>(i)]));
            for (Order *o: copies) e.submit(o);
            for (Order *o: copies) e.book.cancel(o->orderId);
        }

        ContinuousDoubleAuction e;
        for (Order *o: orders) e.submit(o);
        std::vector<std::int64_t> latencies;
        for (Order *o: orders) latencies.push_back(timeNs([&] { e.book.cancel(o->orderId); }));
        return summarise("cancel", latencies);
    }

    Summary benchBestPrice(int n) {
        OrderArena arena;
        ContinuousDoubleAuction e;
        for (Price p = 1; p <= n; ++p) e.submit(lmt(arena, Side::BUY, p, 1));

        volatile Price sink = 0; // keeps the calls from being optimised away
        for (int i = 0; i < PARAMS.nWarmup; ++i) sink = e.book.bestBid()->first;

        std::vector<std::int64_t> latencies;
        for (int i = 0; i < n; ++i) latencies.push_back(timeNs([&] { sink = e.book.bestBid()->first; }));
        (void) sink;
        return summarise("best_price", latencies);
    }

    void writeResults(const std::vector<Summary> &results, const std::string &path) {
        std::ofstream out(path);
        out << std::fixed;
        out << "{\n  \"params\": {\n"
            << "    \"seed\": " << PARAMS.seed << ",\n"
            << "    \"n_orders\": " << PARAMS.nOrders << ",\n"
            << "    \"buy_price_range\": [" << PARAMS.buyLo << ", " << PARAMS.buyHi << "],\n"
            << "    \"sell_price_range\": [" << PARAMS.sellLo << ", " << PARAMS.sellHi << "],\n"
            << "    \"match_price\": " << PARAMS.matchPrice << ",\n"
            << "    \"qty_range\": [" << PARAMS.qtyLo << ", " << PARAMS.qtyHi << "],\n"
            << "    \"n_warmup\": " << PARAMS.nWarmup << "\n  },\n  \"results\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const Summary &r = results[i];
            out << "    {\n"
                << "      \"workload\": \"" << r.workload << "\",\n"
                << "      \"n\": " << r.n << ",\n"
                << std::setprecision(3)
                << "      \"mean_us\": " << r.meanUs << ",\n"
                << "      \"p50_us\": " << r.p50Us << ",\n"
                << "      \"p95_us\": " << r.p95Us << ",\n"
                << "      \"p99_us\": " << r.p99Us << ",\n"
                << "      \"max_us\": " << r.maxUs << ",\n"
                << std::setprecision(1)
                << "      \"throughput_kops_per_sec\": " << r.throughputKops << "\n"
                << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
        std::cout << "\nResults written to " << path << "\n";
    }
}

int main(int argc, char **argv) {
    const std::string path = argc > 1 ? argv[1] : "../results/cpp_microbench.json";
    const int n = PARAMS.nOrders;
    std::cout << "Running benchmarks  n=" << n << "  seed=" << PARAMS.seed << "\n\n";

    const std::vector<std::pair<std::string, std::function<Summary()>>> workloads = {
        {"insert_no_match", [n] { return benchInsertNoMatch(n); }},
        {"submit_match_1v1", [n] { return benchSubmitMatch1v1(n); }},
        {"submit_sweep", [] { return benchSubmitSweep(50); }}, // 50 levels per sweep
        {"cancel", [n] { return benchCancel(n); }},
        {"best_price", [n] { return benchBestPrice(n); }},
    };

    std::vector<Summary> results;
    for (const auto &[label, fn]: workloads) {
        std::cout << "  " << label << " ..." << std::flush;
        results.push_back(fn());
        const Summary &r = results.back();
        std::cout << std::fixed << std::setprecision(3) << "  mean=" << r.meanUs << "us  p99=" << r.p99Us
                  << "us  " << std::setprecision(0) << r.throughputKops << "k ops/s\n";
    }

    writeResults(results, path);
    return 0;
}
