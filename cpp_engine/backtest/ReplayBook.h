//
// A FastBook rebuilt passively from a market-by-order feed that does not
// report every order.
//
// LOBSTER files start with a book already populated, and only carry the
// messages that touch the top N levels at the time. So:
//   - orders resting before the file starts are never added;
//   - an order added deeper than level N is never added either, but its
//     cancels and executions appear once the book moves and its level comes
//     into view;
//   - an order the feed did add can leave while its level is out of view,
//     and that removal is never sent.
//
// Volume the feed cannot account for is held in one placeholder order per
// price level. Placeholders are seeded from a level snapshot, shrunk by
// messages that name orders the book has never seen, and re-synced to the
// feed's own snapshots. When a level drops out of view its tracked orders are
// folded into its placeholder, so every order the book still holds by id was
// watched continuously, and only placeholder volume is ever corrected.
//
// Placeholders stand for orders whose arrival time is unknown; a simulated
// queue treats them as ahead.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_REPLAY_BOOK_H
#define LIMIT_ORDER_BOOK_BACKTEST_REPLAY_BOOK_H

#include "backtest/MarketEvent.h"
#include "fast/FastBook.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace bt {
    // The top levels of a book, best first: (price, quantity).
    struct TopOfBook {
        std::vector<std::pair<Price, Quantity>> bids;
        std::vector<std::pair<Price, Quantity>> asks;

        bool operator==(const TopOfBook &) const = default;
    };

    struct ReplayStats {
        std::uint64_t placeholderHits = 0; // messages on unseen orders absorbed by a placeholder
        std::uint64_t unexplained = 0;     // messages on unseen orders at a level with no placeholder
        std::uint64_t resyncedLevels = 0;  // levels corrected to match a snapshot
        Quantity resyncedVolume = 0;       // absolute volume those corrections moved
        Quantity droppedKnown = 0;         // tracked volume a correction had to remove (should stay 0)
        std::uint64_t foldedOrders = 0;    // tracked orders folded into placeholders as their level left view
    };

    class ReplayBook {
    public:
        // Placeholder ids live at the top of the id space, away from any feed's.
        static constexpr OrderId PLACEHOLDER_BASE = OrderId{1} << 62;

        // `snapshotDepth` is the number of levels per side the feed's
        // snapshots show (LOBSTER's N); 0 if unknown, in which case a level
        // is never known to be out of view.
        ReplayBook(const Price minPrice, const Price maxPrice, const std::size_t maxOrders,
                   const std::size_t snapshotDepth = 0)
            : lo(minPrice), hi(maxPrice), depth(snapshotDepth), book(minPrice, maxPrice, maxOrders) {}

        enum class Applied : std::uint8_t { Known, Placeholder, Unexplained };

        Applied apply(const MarketEvent &e) {
            if (e.type == EventType::Add) {
                book.restPassive(e.id, e.side, e.price, e.quantity);
                return Applied::Known;
            }
            if (const auto info = book.find(e.id)) {
                if (e.type == EventType::Delete) book.cancel(e.id);
                else book.reduce(e.id, std::min(e.quantity, info->quantity));
                return Applied::Known;
            }
            if (takeFromPlaceholder(e.side, e.price, e.quantity) > 0) {
                ++stats_.placeholderHits;
                return Applied::Placeholder;
            }
            ++stats_.unexplained;
            return Applied::Unexplained;
        }

        // Make the levels the snapshot shows match it, and empty the levels it
        // proves are empty. Missing volume goes into the level's placeholder;
        // excess comes out of the placeholder (and, should that not suffice,
        // from the oldest tracked orders). Levels below a full snapshot's
        // depth are out of view: their tracked orders are folded into
        // placeholders. Returns the number of levels changed.
        std::size_t resyncTo(const TopOfBook &truth) {
            return resyncSide(Side::BUY, truth.bids) + resyncSide(Side::SELL, truth.asks);
        }

        [[nodiscard]] TopOfBook top(const std::size_t levels) const {
            TopOfBook t;
            for (const Side side: {Side::BUY, Side::SELL}) {
                auto &out = side == Side::BUY ? t.bids : t.asks;
                book.forEachLevel(side, [&](const Price p, const Quantity q) {
                    out.emplace_back(p, q);
                    return out.size() < levels;
                });
            }
            return t;
        }

        [[nodiscard]] static bool isPlaceholder(const OrderId id) { return id >= PLACEHOLDER_BASE; }
        [[nodiscard]] const fast::FastBook &fastBook() const { return book; }
        [[nodiscard]] const ReplayStats &stats() const { return stats_; }

    private:
        using Key = std::pair<Side, Price>;
        using Levels = std::vector<std::pair<Price, Quantity>>;

        static bool worse(const Side side, const Price a, const Price b) { return side == Side::BUY ? a < b : a > b; }

        Quantity takeFromPlaceholder(const Side side, const Price price, const Quantity quantity) {
            const auto it = placeholders.find({side, price});
            if (it == placeholders.end()) return 0;
            const auto info = book.find(it->second);
            if (!info) {
                placeholders.erase(it);
                return 0;
            }
            const Quantity q = std::min(quantity, info->quantity);
            book.reduce(it->second, q);
            if (q == info->quantity) placeholders.erase(it);
            return q;
        }

        void addToPlaceholder(const Side side, const Price price, const Quantity quantity) {
            const auto it = placeholders.find({side, price});
            Quantity total = quantity;
            OrderId id;
            if (it != placeholders.end()) {
                id = it->second;
                if (const auto info = book.find(id)) {
                    total += info->quantity;
                    book.cancel(id); // FastBook cannot grow an order; re-adding moves it, which no one observes
                }
            } else {
                id = PLACEHOLDER_BASE + nextPlaceholder++;
                placeholders.emplace(Key{side, price}, id);
            }
            book.restPassive(id, side, price, total);
        }

        // Tracked orders at one level, front of the queue first. The result
        // lives in a reused buffer, valid until the next call.
        const std::vector<std::pair<OrderId, Quantity>> &trackedAt(const Side side, const Price p) {
            tracked.clear();
            book.forEachOrderAt(side, p, [&](const OrderId id, const Quantity q) {
                if (!isPlaceholder(id)) tracked.emplace_back(id, q);
            });
            return tracked;
        }

        void fold(const Side side, const Price p) {
            Quantity total = 0;
            for (const auto &[id, q]: trackedAt(side, p)) {
                book.cancel(id);
                total += q;
                ++stats_.foldedOrders;
            }
            if (total > 0) addToPlaceholder(side, p, total);
        }

        std::size_t resyncSide(const Side side, const Levels &levels) {
            Levels &previous = side == Side::BUY ? lastBids : lastAsks;
            const bool full = depth > 0 && levels.size() >= depth;
            const auto shown = [&](const Price p) {
                return std::any_of(levels.begin(), levels.end(), [&](const auto &l) { return l.first == p; });
            };

            // Levels that were in view and now lie below a full snapshot.
            if (full) {
                for (const auto &[p, q]: previous) {
                    if (worse(side, p, levels.back().first) && book.levelQuantity(side, p) > 0) fold(side, p);
                }
            }

            // Our levels inside the view that the snapshot does not show are empty.
            std::size_t changed = 0;
            ours.clear();
            book.forEachLevel(side, [&](const Price p, const Quantity q) {
                if (full && worse(side, p, levels.back().first)) return false;
                if (!full && depth == 0 && !levels.empty() && worse(side, p, levels.back().first)) return false;
                ours.emplace_back(p, q);
                return true;
            });
            for (const auto &[p, q]: ours) {
                if (!shown(p)) changed += adjust(side, p, q, 0);
            }
            for (const auto &[p, q]: levels) {
                if (p < lo || p > hi) continue;
                changed += adjust(side, p, book.levelQuantity(side, p), q);
            }
            previous = levels;
            return changed;
        }

        std::size_t adjust(const Side side, const Price p, const Quantity have, const Quantity want) {
            if (have == want) return 0;
            if (want > have) {
                addToPlaceholder(side, p, want - have);
            } else {
                Quantity excess = have - want;
                excess -= takeFromPlaceholder(side, p, excess);
                // Tracked volume the snapshot says is gone. Folding should make
                // this rare; trim from the front, where the oldest orders are.
                for (const auto &[id, q]: trackedAt(side, p)) {
                    if (excess == 0) break;
                    const Quantity take = std::min(excess, q);
                    book.reduce(id, take);
                    excess -= take;
                    stats_.droppedKnown += take;
                }
            }
            stats_.resyncedVolume += want > have ? want - have : have - want;
            ++stats_.resyncedLevels;
            return 1;
        }

        Price lo, hi;
        std::size_t depth;
        fast::FastBook book;
        std::map<Key, OrderId> placeholders;
        OrderId nextPlaceholder = 0;
        Levels lastBids, lastAsks; // the previous snapshot, to see levels leave view
        Levels ours;               // scratch for resyncSide, kept to avoid allocating per snapshot
        std::vector<std::pair<OrderId, Quantity>> tracked; // scratch for trackedAt
        ReplayStats stats_;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_REPLAY_BOOK_H
