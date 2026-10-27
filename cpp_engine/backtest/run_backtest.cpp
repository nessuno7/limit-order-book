//
// Runs QuoteAtTouch over a synthetic feed or a LOBSTER message file and
// prints the result.
//
//   lob_backtest [--seed N] [--ops N] [--profile tight|deep|volatile]
//                [--lobster MESSAGE_FILE --lobster-book ORDERBOOK_FILE [--price-divisor D]
//                 [--resync cache|memory|online]]
//                [--latency-us N] [--md-latency-us N] [--size N] [--max-position N] [--horizon-ms N]
//                [--maker-fee F] [--taker-fee F] [--repeat N]
//                [--out DIR [--sample-ms N] [--trace-from NS --trace-to NS]]
//
// --repeat N replays N times and reports the fastest (the results are the
// same every time); a 10-level LOBSTER day replays in tens of milliseconds,
// too short for one run to time reliably.
// --resync picks how the LOBSTER book is kept in line with the orderbook
// file (all three give the same result):
//   cache   corrections precomputed once and cached next to the message file
//           (backtest/DayCache.h); the default
//   memory  precomputed in memory from the CSVs, nothing written
//   online  re-synced to the orderbook file after every message
// --out DIR writes summary.json, series.csv and fills.csv there (see
// backtest/Trace.h), from one more run after the timed ones, so tracing never
// slows the timing. --sample-ms sets the series' step; by default the day is
// cut into about 10,000 samples. --trace-from/--trace-to (feed time, ns
// since midnight) sample only that window, by default in about 10,000 steps:
// the viewer re-runs a zoomed window this way. The localhost viewer
// (ui/server.py) reads these files.
//

#include "backtest/Backtester.h"
#include "backtest/DayCache.h"
#include "backtest/Feeds.h"
#include "backtest/Strategies.h"
#include "backtest/Trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
    [[noreturn]] void usage(const char *why) {
        std::cerr << why << "\nsee the header of backtest/run_backtest.cpp for the options\n";
        std::exit(2);
    }

    double since(const std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    std::string jsonString(const std::string &s) {
        std::string r = "\"";
        for (const char c: s) {
            if (c == '"' || c == '\\') r += '\\';
            r += c;
        }
        return r + "\"";
    }
}

int main(const int argc, char **argv) {
    std::uint64_t seed = 1;
    std::size_t ops = 1'000'000;
    int profile = 0;
    std::string lobster, lobsterBook, resync = "cache";
    Price priceDivisor = 1;
    bt::Config cfg;
    cfg.minPrice = 0;
    cfg.maxPrice = 0;
    Quantity size = 10;
    Quantity maxPosition = 200;
    int repeat = 1;
    std::string outDir;
    bt::Nanos sampleStep = 0; // 0: about 10,000 samples over the feed
    bt::Nanos traceFrom = 0, traceTo = 0; // a window of feed time to trace; 0, 0: all of it

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) usage(("missing value for " + a).c_str());
        const std::string v = argv[++i];
        if (a == "--seed") seed = std::stoull(v);
        else if (a == "--ops") ops = std::stoull(v);
        else if (a == "--profile") profile = v == "deep" ? 1 : v == "volatile" ? 2 : 0;
        else if (a == "--lobster") lobster = v;
        else if (a == "--lobster-book") lobsterBook = v;
        else if (a == "--price-divisor") priceDivisor = std::stoll(v);
        else if (a == "--resync") resync = v;
        else if (a == "--latency-us") cfg.orderLatency = std::stoll(v) * 1'000;
        else if (a == "--md-latency-us") cfg.mdLatency = std::stoll(v) * 1'000;
        else if (a == "--horizon-ms") cfg.markoutHorizon = std::stoll(v) * 1'000'000;
        else if (a == "--size") size = std::stoll(v);
        else if (a == "--max-position") maxPosition = std::stoll(v);
        else if (a == "--maker-fee") cfg.makerFee = std::stod(v);
        else if (a == "--taker-fee") cfg.takerFee = std::stod(v);
        else if (a == "--repeat") repeat = std::max(1, std::stoi(v));
        else if (a == "--out") outDir = v;
        else if (a == "--sample-ms") sampleStep = std::stoll(v) * 1'000'000;
        else if (a == "--trace-from") traceFrom = std::stoll(v);
        else if (a == "--trace-to") traceTo = std::stoll(v);
        else usage(("unknown option " + a).c_str());
    }
    if (resync != "cache" && resync != "memory" && resync != "online") usage("--resync takes cache, memory or online");

    std::vector<bt::MarketEvent> events;
    bt::LobsterDay day;          // --resync online
    bt::CompiledDay compiled;    // --resync cache / memory
    const auto tLoad = std::chrono::steady_clock::now();
    if (!lobster.empty()) {
        // The orderbook file seeds the book and re-syncs it after every
        // message: LOBSTER only reports the top N levels.
        if (lobsterBook.empty()) usage("--lobster needs --lobster-book, its orderbook file");
        std::size_t rows = 0, depth = 0;
        Price lo = 0, hi = 0;
        std::string how;
        if (resync == "online") {
            std::ifstream in(lobster), books(lobsterBook);
            if (!in || !books) usage("--lobster needs a readable message file and --lobster-book its orderbook file");
            day = bt::loadLobster(in, books, priceDivisor);
            rows = day.messages.size(), depth = day.depth, lo = day.minPrice, hi = day.maxPrice;
            how = "parsed both CSVs";
        } else {
            bool fromCache = false;
            compiled = bt::loadLobsterCompiled(lobster, lobsterBook, priceDivisor, fromCache, resync == "cache");
            rows = compiled.rows, depth = compiled.depth, lo = compiled.minPrice, hi = compiled.maxPrice;
            how = fromCache ? "from cache " + bt::dayCachePath(lobster, priceDivisor)
                  : resync == "cache" ? "compiled from the CSVs, cached to " + bt::dayCachePath(lobster, priceDivisor)
                                      : "compiled from the CSVs";
        }
        cfg.minPrice = lo - 1;
        cfg.maxPrice = hi + 1;
        cfg.snapshotDepth = depth;
        std::cout << "feed: " << lobster << ", " << rows << " messages\n";
        std::printf("load:     %.3f s, %s\n", since(tLoad), how.c_str());
    } else {
        const FlowParams params = flowProfile(profile);
        cfg.minPrice = params.minPrice;
        cfg.maxPrice = params.maxPrice;
        events = bt::syntheticFeed(seed, ops, params);
        std::cout << "feed: synthetic " << flowName(profile) << ", seed " << seed << ", " << events.size()
                  << " messages from " << ops << " requests\n";
    }

    const auto replay = [&](bt::Backtester &backtest) {
        if (!lobster.empty() && resync == "online") {
            backtest.seed(day.initial);
            for (std::size_t i = 0; i < day.messages.size(); ++i) {
                if (day.messages[i]) backtest.step(*day.messages[i], &day.books[i]);
            }
            backtest.finish();
        } else if (!lobster.empty()) {
            backtest.seed(compiled.seedFixes());
            for (std::size_t i = 0; i < compiled.events.size(); ++i) {
                backtest.step(compiled.events[i], compiled.fixesAfter(i));
            }
            backtest.finish();
        } else {
            backtest.run(events);
        }
    };

    std::optional<bt::QuoteAtTouch> strategy;
    std::optional<bt::Backtester> run;
    double secs = 0;
    for (int r = 0; r < repeat; ++r) {
        strategy.emplace(size, maxPosition);
        run.emplace(cfg, *strategy);
        const auto t0 = std::chrono::steady_clock::now();
        replay(*run);
        secs = r == 0 ? since(t0) : std::min(secs, since(t0));
    }
    const bt::Backtester &backtest = *run;

    const bt::Stats &s = backtest.stats();
    std::printf("strategy: QuoteAtTouch size %lld, max position %lld, latency %lld us order entry + %lld us market data\n",
                static_cast<long long>(size), static_cast<long long>(maxPosition),
                static_cast<long long>(cfg.orderLatency / 1'000), static_cast<long long>(cfg.mdLatency / 1'000));
    std::printf("replay:   %.3f s, %.2f M messages/s, %llu messages on orders the feed never added\n", secs,
                static_cast<double>(s.events) / secs / 1e6, static_cast<unsigned long long>(s.unknownOrders));
    std::printf("fills:    %llu (maker %lld, taker %lld shares; bought %lld, sold %lld)\n",
                static_cast<unsigned long long>(s.fills), static_cast<long long>(s.makerVolume),
                static_cast<long long>(s.takerVolume), static_cast<long long>(s.boughtVolume),
                static_cast<long long>(s.soldVolume));
    std::printf("position: %lld at the end, %lld at most\n", static_cast<long long>(s.position),
                static_cast<long long>(s.maxAbsPosition));
    std::printf("pnl:      %.1f ticks marked to mid %.1f (fees %.1f)\n", s.pnl(backtest.mid()), backtest.mid(), s.fees);
    std::printf("markout:  %.3f ticks/share after %lld ms\n", s.markoutPerShare(),
                static_cast<long long>(cfg.markoutHorizon / 1'000'000));
    if (outDir.empty()) return 0;

    // The traced run.
    bt::Nanos first = 0, last = 0;
    if (!lobster.empty() && resync == "online") {
        for (const auto &m: day.messages) {
            if (!m) continue;
            if (first == 0) first = m->time;
            last = m->time;
        }
    } else if (!lobster.empty() && !compiled.events.empty()) {
        first = compiled.events.front().time, last = compiled.events.back().time;
    } else if (!events.empty()) {
        first = events.front().time, last = events.back().time;
    }
    if (traceTo <= traceFrom) traceFrom = first, traceTo = last;
    if (sampleStep <= 0) sampleStep = std::max<bt::Nanos>(1'000, (traceTo - traceFrom) / 10'000);
    bt::QuoteAtTouch inner(size, maxPosition);
    bt::Trace trace(inner, sampleStep, traceFrom, traceTo);
    bt::Backtester traced(cfg, trace);
    replay(traced);
    trace.finish(traced);
    std::filesystem::create_directories(outDir);
    trace.write(outDir);

    const std::string feed = lobster.empty() ? "synthetic " + std::string(flowName(profile)) : lobster;
    std::FILE *json = std::fopen((outDir + "/summary.json").c_str(), "w");
    if (json == nullptr) usage(("cannot write " + outDir + "/summary.json").c_str());
    std::fprintf(json, "{\n  \"feed\": %s,\n  \"messages\": %llu,\n", jsonString(feed).c_str(),
                 static_cast<unsigned long long>(s.events));
    // LOBSTER prices are dollars x 10,000 before the divisor; synthetic prices are bare ticks.
    std::fprintf(json, "  \"tick_value\": %.6g,\n  \"tick_unit\": \"%s\",\n",
                 lobster.empty() ? 1.0 : static_cast<double>(priceDivisor) / 10'000.0, lobster.empty() ? "tick" : "$");
    std::fprintf(json, "  \"time_start\": %lld,\n  \"time_end\": %lld,\n  \"sample_ns\": %lld,\n",
                 static_cast<long long>(first), static_cast<long long>(last), static_cast<long long>(sampleStep));
    std::fprintf(json, "  \"replay_seconds\": %.6f,\n  \"messages_per_second\": %.0f,\n", secs,
                 static_cast<double>(s.events) / secs);
    std::fprintf(json,
                 "  \"config\": {\"strategy\": \"QuoteAtTouch\", \"size\": %lld, \"max_position\": %lld, "
                 "\"order_latency_us\": %lld, \"md_latency_us\": %lld, \"markout_horizon_ms\": %lld, "
                 "\"maker_fee\": %.6g, \"taker_fee\": %.6g},\n",
                 static_cast<long long>(size), static_cast<long long>(maxPosition),
                 static_cast<long long>(cfg.orderLatency / 1'000), static_cast<long long>(cfg.mdLatency / 1'000),
                 static_cast<long long>(cfg.markoutHorizon / 1'000'000), cfg.makerFee, cfg.takerFee);
    std::fprintf(json,
                 "  \"stats\": {\"fills\": %llu, \"maker_volume\": %lld, \"taker_volume\": %lld, "
                 "\"bought\": %lld, \"sold\": %lld, \"position\": %lld, \"max_abs_position\": %lld, "
                 "\"cash\": %lld, \"fees\": %.6g, \"pnl\": %.6g, \"mid\": %.6g, "
                 "\"markout_per_share\": %.6g, \"unknown_orders\": %llu}\n}\n",
                 static_cast<unsigned long long>(s.fills), static_cast<long long>(s.makerVolume),
                 static_cast<long long>(s.takerVolume), static_cast<long long>(s.boughtVolume),
                 static_cast<long long>(s.soldVolume), static_cast<long long>(s.position),
                 static_cast<long long>(s.maxAbsPosition), static_cast<long long>(s.cash), s.fees,
                 s.pnl(backtest.exchangeMid()), backtest.exchangeMid(), s.markoutPerShare(),
                 static_cast<unsigned long long>(s.unknownOrders));
    std::fclose(json);
    std::printf("trace:    %zu samples, %zu fills in %s\n", trace.samples().size(), trace.fills().size(),
                outDir.c_str());
    return 0;
}
