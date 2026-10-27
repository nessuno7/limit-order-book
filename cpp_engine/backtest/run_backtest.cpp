//
// Runs QuoteAtTouch over a synthetic feed or a LOBSTER message file and
// prints the result.
//
//   lob_backtest [--seed N] [--ops N] [--profile tight|deep|volatile]
//                [--lobster MESSAGE_FILE --lobster-book ORDERBOOK_FILE [--price-divisor D]]
//                [--latency-us N] [--size N] [--max-position N] [--horizon-ms N]
//                [--maker-fee F] [--taker-fee F]
//

#include "backtest/Backtester.h"
#include "backtest/Feeds.h"
#include "backtest/Strategies.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
    [[noreturn]] void usage(const char *why) {
        std::cerr << why << "\nsee the header of backtest/run_backtest.cpp for the options\n";
        std::exit(2);
    }
}

int main(const int argc, char **argv) {
    std::uint64_t seed = 1;
    std::size_t ops = 1'000'000;
    int profile = 0;
    std::string lobster, lobsterBook;
    Price priceDivisor = 1;
    bt::Config cfg;
    cfg.minPrice = 0;
    cfg.maxPrice = 0;
    Quantity size = 10;
    Quantity maxPosition = 200;

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
        else if (a == "--latency-us") cfg.orderLatency = std::stoll(v) * 1'000;
        else if (a == "--horizon-ms") cfg.markoutHorizon = std::stoll(v) * 1'000'000;
        else if (a == "--size") size = std::stoll(v);
        else if (a == "--max-position") maxPosition = std::stoll(v);
        else if (a == "--maker-fee") cfg.makerFee = std::stod(v);
        else if (a == "--taker-fee") cfg.takerFee = std::stod(v);
        else usage(("unknown option " + a).c_str());
    }

    std::vector<bt::MarketEvent> events;
    bt::LobsterDay day;
    if (!lobster.empty()) {
        // The orderbook file seeds the book and re-syncs it after every
        // message: LOBSTER only reports the top N levels.
        std::ifstream in(lobster), books(lobsterBook);
        if (!in || !books) usage("--lobster needs a readable message file and --lobster-book its orderbook file");
        day = bt::loadLobster(in, books, priceDivisor);
        cfg.minPrice = day.minPrice - 1;
        cfg.maxPrice = day.maxPrice + 1;
        cfg.snapshotDepth = day.depth;
        std::cout << "feed: " << lobster << ", " << day.messages.size() << " messages\n";
    } else {
        const FlowParams params = flowProfile(profile);
        cfg.minPrice = params.minPrice;
        cfg.maxPrice = params.maxPrice;
        events = bt::syntheticFeed(seed, ops, params);
        std::cout << "feed: synthetic " << flowName(profile) << ", seed " << seed << ", " << events.size()
                  << " messages from " << ops << " requests\n";
    }

    bt::QuoteAtTouch strategy(size, maxPosition);
    bt::Backtester backtest(cfg, strategy);
    const auto t0 = std::chrono::steady_clock::now();
    if (!lobster.empty()) {
        backtest.seed(day.initial);
        for (std::size_t i = 0; i < day.messages.size(); ++i) {
            if (day.messages[i]) backtest.step(*day.messages[i], &day.books[i]);
        }
        backtest.finish();
    } else {
        backtest.run(events);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const bt::Stats &s = backtest.stats();
    std::printf("strategy: QuoteAtTouch size %lld, max position %lld, latency %lld us\n",
                static_cast<long long>(size), static_cast<long long>(maxPosition),
                static_cast<long long>(cfg.orderLatency / 1'000));
    std::printf("replay:   %.2f s, %.2f M messages/s, %llu messages on orders the feed never added\n", secs,
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
    return 0;
}
