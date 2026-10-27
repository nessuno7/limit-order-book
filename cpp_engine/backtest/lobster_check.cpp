//
// Rebuilds a LOBSTER day passively and checks the book against LOBSTER's own
// orderbook file after every message (ROADMAP Part 3's oracle).
//
//   lob_lobster_check MESSAGE_FILE ORDERBOOK_FILE [--price-divisor D] [--no-resync]
//
// The book is seeded from the first snapshot. After each message it is
// compared with the snapshot *before* any re-sync, so the match rate measures
// what the messages alone explain. Unless --no-resync is given, the book is
// then re-synced to the snapshot, so one gap does not spoil every later row.
//

#include "backtest/Feeds.h"
#include "backtest/ReplayBook.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {
    void print(const char *label, const bt::TopOfBook &t) {
        std::printf("    %-6s bids:", label);
        for (const auto &[p, q]: t.bids) std::printf(" %lldx%lld", static_cast<long long>(p), static_cast<long long>(q));
        std::printf("  asks:");
        for (const auto &[p, q]: t.asks) std::printf(" %lldx%lld", static_cast<long long>(p), static_cast<long long>(q));
        std::printf("\n");
    }
}

int main(const int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: lob_lobster_check MESSAGE_FILE ORDERBOOK_FILE [--price-divisor D] [--no-resync]\n";
        return 2;
    }
    Price divisor = 1;
    bool resync = true;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--price-divisor" && i + 1 < argc) divisor = std::stoll(argv[++i]);
        else if (a == "--no-resync") resync = false;
        else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }
    std::ifstream messages(argv[1]), books(argv[2]);
    if (!messages || !books) {
        std::cerr << "cannot open the input files\n";
        return 2;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const bt::LobsterDay day = bt::loadLobster(messages, books, divisor);
    const auto t1 = std::chrono::steady_clock::now();
    if (day.books.empty()) {
        std::cerr << "empty files\n";
        return 2;
    }
    const std::size_t depth = day.depth;

    std::size_t maxOrders = 1'024;
    for (const auto &m: day.messages) if (m && m->type == bt::EventType::Add) ++maxOrders;
    bt::ReplayBook book(day.minPrice - 1, day.maxPrice + 1, maxOrders, depth);
    book.resyncTo(day.initial);
    const bt::ReplayStats seeded = book.stats();

    std::size_t visible = 0, exact = 0, touch = 0, inner = 0, shown = 0;
    const auto head = [](const bt::TopOfBook &t, const std::size_t k) {
        bt::TopOfBook h;
        h.bids.assign(t.bids.begin(), t.bids.begin() + static_cast<std::ptrdiff_t>(std::min(k, t.bids.size())));
        h.asks.assign(t.asks.begin(), t.asks.begin() + static_cast<std::ptrdiff_t>(std::min(k, t.asks.size())));
        return h;
    };
    std::uint64_t resyncs = 0;
    for (std::size_t i = 0; i < day.books.size(); ++i) {
        const auto &m = day.messages[i];
        if (m) {
            book.apply(*m);
            ++visible;
        }
        const bt::TopOfBook ours = book.top(depth);
        const bt::TopOfBook &truth = day.books[i];
        const auto best = [](const auto &v) { return v.empty() ? std::pair<Price, Quantity>{0, 0} : v.front(); };
        const bool touchOk = best(ours.bids) == best(truth.bids) && best(ours.asks) == best(truth.asks);
        if (ours == truth) ++exact;
        // Level N can be one the feed never described, surfacing as a level above clears.
        if (depth > 1 && head(ours, depth - 1) == head(truth, depth - 1)) ++inner;
        if (touchOk) ++touch;
        else if (shown < 3) { // the touch is what a strategy trades on: show where it first goes wrong
            ++shown;
            std::printf("  touch mismatch after line %zu", i + 1);
            if (m) std::cout << " (" << *m << ")";
            std::printf("\n");
            print("ours", ours);
            print("lobster", truth);
        }
        if (resync) resyncs += book.resyncTo(truth);
    }
    const auto t2 = std::chrono::steady_clock::now();

    const double n = static_cast<double>(day.books.size());
    const bt::ReplayStats &s = book.stats();
    std::printf("file:        %s (%zu levels)\n", argv[1], depth);
    std::printf("lines:       %zu (%zu change the visible book)\n", day.books.size(), visible);
    std::printf("seeded:      %llu levels from the first snapshot\n",
                static_cast<unsigned long long>(seeded.resyncedLevels));
    std::printf("exact match: %zu / %zu rows (%.3f%%), all %zu levels both sides, before re-sync\n", exact,
                day.books.size(), 100.0 * static_cast<double>(exact) / n, depth);
    if (depth > 1) {
        std::printf("top %zu match: %zu / %zu rows (%.3f%%), the levels every message reports on\n", depth - 1, inner,
                    day.books.size(), 100.0 * static_cast<double>(inner) / n);
    }
    std::printf("touch match: %zu / %zu rows (%.3f%%), best bid and ask, price and size\n", touch, day.books.size(),
                100.0 * static_cast<double>(touch) / n);
    std::printf("unseen ids:  %llu absorbed by placeholder volume, %llu unexplained\n",
                static_cast<unsigned long long>(s.placeholderHits), static_cast<unsigned long long>(s.unexplained));
    std::printf("folded:      %llu tracked orders became placeholder volume as their level left view\n",
                static_cast<unsigned long long>(s.foldedOrders));
    std::printf("dropped:     %lld tracked shares a re-sync had to remove\n", static_cast<long long>(s.droppedKnown));
    std::printf("re-syncs:    %s%llu levels after seeding, %lld shares moved\n", resync ? "" : "(off) ",
                static_cast<unsigned long long>(resyncs),
                static_cast<long long>(s.resyncedVolume - seeded.resyncedVolume));
    std::printf("time:        load %.2f s, replay+check %.2f s (%.2f M lines/s)\n",
                std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count(),
                n / std::chrono::duration<double>(t2 - t1).count() / 1e6);
    // Pass/fail on what the feed can explain: the top N-1 levels (the touch for a one-level file
    // is informational only, since the next level is never described).
    return depth <= 1 || inner == day.books.size() ? 0 : 1;
}
