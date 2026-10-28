//
// The simulated exchange behind the backtester: the historical book rebuilt
// from the feed, our orders queued beside it, and the accounting of our
// fills. It knows nothing about latency; Backtester decides when our actions
// reach it and when what it reports reaches the strategy.
//
// The historical book is rebuilt passively in a FastBook, exactly as the feed
// reports it. Our orders never enter that book: they live in a simulated
// queue beside it, and each feed message decides whether they would have
// traded. Design decisions (see ROADMAP.md, Part 4):
//
//   Queue position is exact, not modelled. With order ids in the feed, every
//   historical order has an arrival number, so for each cancel or execution
//   we know whether it was ahead of us or behind us. Only volume ahead of us
//   shrinks our queue position. The pessimistic / optimistic / proportional
//   models are only needed for level-aggregated data, which this is not.
//
//   We fill when the feed proves an aggressor reached us:
//     - an execution of an order behind us at our price, or of any order at
//       a price worse than ours (FIFO says we were hit first);
//     - an incoming order that crosses our resting price (it would have
//       traded with us instead of resting);
//     - our own order crossing the book when it arrives (we take liquidity
//       at the resting prices).
//
//   No market impact. Our fills do not remove historical liquidity and
//   historical messages are applied unchanged, so the same aggressor volume
//   can fill both us and the order it really hit. That is optimistic, and
//   fine while our size is small next to the book's.
//
//   Orders the feed never introduced (LOBSTER starts mid-day, and only
//   reports the top N levels) are held as placeholder volume by ReplayBook,
//   seeded and optionally re-synced from level snapshots. They count as
//   ahead of us.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_SIM_EXCHANGE_H
#define LIMIT_ORDER_BOOK_BACKTEST_SIM_EXCHANGE_H

#include "backtest/FlatContainers.h"
#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace bt {
    struct Config {
        Price minPrice = 1;
        Price maxPrice = 20'000;
        std::size_t maxOrders = 4'096;  // starting capacity for historical orders; the book grows past it
        std::size_t snapshotDepth = 0;   // levels per side in the feed's snapshots, if it has them (LOBSTER's N)
        Nanos orderLatency = 0;          // strategy -> exchange, for placements and cancels
        Nanos mdLatency = 0;             // exchange -> strategy: book messages, fills, acknowledgements
        Nanos markoutHorizon = 1'000'000'000; // adverse selection is measured this long after each fill
        double makerFee = 0.0;           // per share, in ticks; negative for a rebate
        double takerFee = 0.0;
    };

    // One of our orders.
    struct SimOrder {
        OrderId id = 0;
        Side side = Side::BUY;
        Price price = 0;
        Quantity remaining = 0;
        Quantity ahead = 0;          // historical volume in front of us at our price
        std::uint64_t joinSeq = 0;   // historical orders with a sequence number <= this are ahead
        bool live = false;           // false while the placement is in flight (or, with
                                     // market-data latency, until its acknowledgement arrives)
        bool cancelSent = false;
    };

    struct SimFill {
        Nanos time = 0;
        OrderId id = 0;
        Side side = Side::BUY;
        Price price = 0;
        Quantity quantity = 0;
        bool maker = true;
    };

    struct Stats {
        std::uint64_t events = 0;
        std::uint64_t unknownOrders = 0; // cancels/executions of orders resting before the feed began
        std::uint64_t fills = 0;
        Quantity makerVolume = 0;
        Quantity takerVolume = 0;
        Quantity boughtVolume = 0;
        Quantity soldVolume = 0;
        Quantity position = 0;
        Quantity maxAbsPosition = 0;
        std::int64_t cash = 0;       // ticks x shares, exact
        double fees = 0.0;           // ticks
        double markout = 0.0;        // sum over fills of signed (mid after horizon - fill price) x quantity
        Quantity markoutVolume = 0;

        // Mark-to-mid profit, in ticks, after fees.
        [[nodiscard]] double pnl(const double mid) const {
            return static_cast<double>(cash) + static_cast<double>(position) * mid - fees;
        }

        // Average markout per share: negative means fills were followed by
        // the price moving against us (adverse selection).
        [[nodiscard]] double markoutPerShare() const {
            return markoutVolume > 0 ? markout / static_cast<double>(markoutVolume) : 0.0;
        }
    };

    // The order with `id` in a list sorted by id, or end().
    template<class List>
    auto findById(List &list, const OrderId id) -> decltype(list.begin()) {
        const auto it = std::lower_bound(list.begin(), list.end(), id,
                                         [](const SimOrder &o, const OrderId x) { return o.id < x; });
        return it != list.end() && it->id == id ? it : list.end();
    }

    // Halfway between the best bid and ask; nullopt while either side is empty.
    inline std::optional<double> touchMid(const ReplayBook &book) {
        const auto bid = book.fastBook().bestBid();
        const auto ask = book.fastBook().bestAsk();
        if (!bid || !ask) return std::nullopt;
        return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
    }

    class SimExchange {
    public:
        explicit SimExchange(const Config &config)
            : cfg(config), book_(config.minPrice, config.maxPrice, config.maxOrders, config.snapshotDepth) {}

        // ── our orders ───────────────────────────────────────────────────

        // An order the strategy has sent. It takes no part in matching
        // until its placement lands (arrive).
        void add(const SimOrder &o) { orders_.push_back(o); } // ids only grow, so `orders_` stays sorted

        // The placement of `id` lands at `t`: whatever crosses the book
        // trades at the resting prices, the rest queues behind everything
        // already at its price. False if the order is no longer open.
        bool arrive(const OrderId id, const Nanos t) {
            const auto it = findById(orders_, id);
            if (it == orders_.end()) return false;
            SimOrder &o = *it;
            book_.fastBook().forEachLevel(opposite(o.side), [&](const Price p, const Quantity available) {
                if (!atLeastAsGood(o.side, o.price, p)) return false;
                fill(o, p, std::min(available, o.remaining), false, t);
                return o.remaining > 0;
            });
            if (o.remaining == 0) {
                orders_.erase(it);
                return true;
            }
            o.live = true;
            o.ahead = book_.fastBook().levelQuantity(o.side, o.price);
            o.joinSeq = seqCounter;
            return true;
        }

        // The cancel of `id` lands. False if the order had already filled.
        bool cancel(const OrderId id) {
            const auto it = findById(orders_, id);
            if (it == orders_.end()) return false;
            orders_.erase(it);
            return true;
        }

        // False if `id` is gone or already has a cancel in flight.
        bool markCancelSent(const OrderId id) {
            const auto it = findById(orders_, id);
            if (it == orders_.end() || it->cancelSent) return false;
            it->cancelSent = true;
            return true;
        }

        [[nodiscard]] bool hasFills() const { return !fills.empty(); }

        // Moves the fills made since the last call into `out`. The two
        // buffers trade places, so neither loses its capacity.
        void takeFills(std::vector<SimFill> &out) {
            out.clear();
            out.swap(fills);
        }

        // ── the feed ─────────────────────────────────────────────────────

        // Applies one feed message: fills our orders it reaches, updates the
        // historical book, then lets `resync(book)` correct it. Returns the
        // corrections `resync` reports.
        template<class Resync>
        std::span<const Correction> onMessage(const MarketEvent &e, Resync &&resync) {
            clock_ = e.time;
            ++stats_.events;
            fillAgainst(e);
            applyToBook(e);
            const std::span<const Correction> fixes = resync(book_);
            if (!book_.dropped().empty()) retireDropped();
            updateMid();
            settleMarkouts(false);
            return fixes;
        }

        // Loads the book as it stood before the first message.
        void seed(const TopOfBook &levels, std::vector<Correction> *record) {
            book_.resyncTo(levels, record);
            updateMid();
        }

        void seed(const std::span<const Correction> fixes) {
            book_.replay(fixes);
            updateMid();
        }

        // Settles the remaining markouts at the last mid.
        void finish() { settleMarkouts(true); }

        // ── state ────────────────────────────────────────────────────────

        [[nodiscard]] Nanos clock() const { return clock_; }
        [[nodiscard]] double mid() const { return mid_; }
        [[nodiscard]] const Stats &stats() const { return stats_; }
        [[nodiscard]] const ReplayBook &book() const { return book_; }
        [[nodiscard]] const std::vector<SimOrder> &orders() const { return orders_; }

        [[nodiscard]] const SimOrder *order(const OrderId id) const {
            const auto it = findById(orders_, id);
            return it == orders_.end() ? nullptr : &*it;
        }

    private:
        struct PendingMarkout {
            Nanos due;
            Side side;
            Price price;
            Quantity quantity;
        };

        void fillAgainst(const MarketEvent &e) {
            switch (e.type) {
                case EventType::Add: {
                    // An incoming order that crosses our resting price would have traded with us.
                    const Side ours = opposite(e.side);
                    const auto crossesUs = [&](const SimOrder &o) { return atLeastAsGood(ours, o.price, e.price); };
                    if (!anyLive(ours, crossesUs)) return;
                    Quantity budget = e.quantity;
                    for (SimOrder *o: liveOrders(ours)) {
                        if (budget == 0 || !crossesUs(*o)) break;
                        // Volume in front of us at our price trades first. A consistent feed
                        // never has any here (the add would have executed against it), but
                        // a hand-made or damaged one might.
                        budget -= std::min(budget, o->ahead);
                        const Quantity q = std::min(budget, o->remaining);
                        fill(*o, o->price, q, true, e.time);
                        budget -= q;
                    }
                    break;
                }
                case EventType::Cancel:
                case EventType::Delete: {
                    const auto atPrice = [&](const SimOrder &o) { return o.price == e.price; };
                    if (!anyLive(e.side, atPrice)) return;
                    const std::uint64_t s = seqOf(e.id);
                    for (SimOrder *o: liveOrders(e.side)) {
                        if (atPrice(*o) && s <= o->joinSeq) o->ahead -= std::min(o->ahead, e.quantity);
                    }
                    break;
                }
                case EventType::Execute: {
                    // The executed order sat at e.price; we are reached at that price or better.
                    const auto reached = [&](const SimOrder &o) { return atLeastAsGood(e.side, o.price, e.price); };
                    if (!anyLive(e.side, reached)) return;
                    const std::uint64_t s = seqOf(e.id);
                    Quantity budget = e.quantity;
                    for (SimOrder *o: liveOrders(e.side)) {
                        if (o->price == e.price && s <= o->joinSeq) {
                            o->ahead -= std::min(o->ahead, e.quantity); // the hit order was in front of us
                        } else if (reached(*o) && budget > 0) {
                            o->ahead = 0; // the aggressor got past everything in front of us
                            const Quantity q = std::min(budget, o->remaining);
                            fill(*o, o->price, q, true, e.time);
                            budget -= q;
                        }
                    }
                    break;
                }
            }
            eraseFilled();
        }

        void applyToBook(const MarketEvent &e) {
            if (book_.apply(e) != ReplayBook::Applied::Known) {
                ++stats_.unknownOrders;
                return;
            }
            if (e.type == EventType::Add) {
                arrival.set(e.id, ++seqCounter);
                if (departed.size() > 0) departed.erase(e.id); // a reused id starts afresh
            }
            else if (!book_.fastBook().contains(e.id)) arrival.erase(e.id);
        }

        // Whether any live order on `side` satisfies `p`. Most messages are
        // nowhere near our orders; this lets them skip building and sorting
        // the priority list.
        template<class P>
        [[nodiscard]] bool anyLive(const Side side, P &&p) const {
            for (const SimOrder &o: orders_) {
                if (o.live && o.remaining > 0 && o.side == side && p(o)) return true;
            }
            return false;
        }

        // Live orders on one side, in exchange priority: best price, then
        // earliest arrival. The result lives in a reused buffer.
        const std::vector<SimOrder *> &liveOrders(const Side side) {
            liveScratch.clear();
            for (SimOrder &o: orders_) {
                if (o.live && o.remaining > 0 && o.side == side) liveScratch.push_back(&o);
            }
            std::sort(liveScratch.begin(), liveScratch.end(), [](const SimOrder *a, const SimOrder *b) {
                if (a->price != b->price) return better(a->side, a->price, b->price);
                return a->joinSeq < b->joinSeq;
            });
            return liveScratch;
        }

        // An order's arrival number; 0 for one resting before the feed
        // began, which is ahead of everyone.
        [[nodiscard]] std::uint64_t seqOf(const OrderId id) const {
            const std::uint64_t s = arrival.get(id, 0);
            return s != 0 ? s : departed.get(id, 0);
        }

        // Orders a re-sync removed keep their arrival number, as later
        // messages may still name them, but move out of the map every add
        // and delete touches, which then holds only resting orders and stays
        // in cache.
        void retireDropped() {
            for (const OrderId id: book_.dropped()) {
                if (const std::uint64_t s = arrival.get(id, 0); s != 0) {
                    departed.set(id, s);
                    arrival.erase(id);
                }
            }
            book_.clearDropped();
        }

        void fill(SimOrder &o, const Price p, const Quantity q, const bool maker, const Nanos t) {
            if (q <= 0) return;
            o.remaining -= q;
            const Quantity delta = signedQuantity(o.side, q);
            stats_.position += delta;
            stats_.cash -= p * delta;
            (o.side == Side::BUY ? stats_.boughtVolume : stats_.soldVolume) += q;
            (maker ? stats_.makerVolume : stats_.takerVolume) += q;
            stats_.fees += static_cast<double>(q) * (maker ? cfg.makerFee : cfg.takerFee);
            stats_.maxAbsPosition = std::max(stats_.maxAbsPosition, std::abs(stats_.position));
            ++stats_.fills;
            markouts.push_back({t + cfg.markoutHorizon, o.side, p, q});
            fills.push_back({t, o.id, o.side, p, q, maker});
        }

        void eraseFilled() {
            std::erase_if(orders_, [](const SimOrder &o) { return o.remaining == 0; });
        }

        void updateMid() {
            if (const auto m = touchMid(book_)) mid_ = *m;
        }

        // Fill times are non-decreasing, so `markouts` is in due order.
        void settleMarkouts(const bool all) {
            while (!markouts.empty() && (all || markouts.front().due <= clock_)) {
                const PendingMarkout &m = markouts.front();
                const double edge = m.side == Side::BUY ? mid_ - static_cast<double>(m.price)
                                                        : static_cast<double>(m.price) - mid_;
                stats_.markout += edge * static_cast<double>(m.quantity);
                stats_.markoutVolume += m.quantity;
                markouts.pop_front();
            }
        }

        Config cfg;
        ReplayBook book_;
        IdMap<std::uint64_t> arrival;  // resting historical order -> arrival number
        IdMap<std::uint64_t> departed; // the same, for orders a re-sync removed
        std::uint64_t seqCounter = 0;
        std::vector<SimOrder> orders_; // open orders, sorted by id
        std::vector<SimOrder *> liveScratch;
        std::vector<SimFill> fills;    // made since the last takeFills()
        Fifo<PendingMarkout> markouts;
        Nanos clock_ = 0;
        double mid_ = 0.0;
        Stats stats_;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_SIM_EXCHANGE_H
