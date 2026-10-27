//
// Event-driven market-making backtester over a market-by-order feed.
//
// The historical book is rebuilt passively in a FastBook, exactly as the feed
// reports it. The strategy's own orders never enter that book: they live in a
// simulated queue beside it, and each feed message decides whether they would
// have traded. Design decisions (see ROADMAP.md, Part 4):
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
//   Latency is a constant order-entry delay: a placement or cancel sent at t
//   takes effect before the first message at or after t + orderLatency, so a
//   cancelled order can still fill while the cancel is in flight.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H
#define LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H

#include "backtest/FlatContainers.h"
#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bt {
    struct Config {
        Price minPrice = 1;
        Price maxPrice = 20'000;
        std::size_t maxOrders = 4'096;  // starting capacity for historical orders; the book grows past it
        std::size_t snapshotDepth = 0;   // levels per side in the feed's snapshots, if it has them (LOBSTER's N)
        Nanos orderLatency = 0;          // strategy -> exchange, for placements and cancels
        Nanos markoutHorizon = 1'000'000'000; // adverse selection is measured this long after each fill
        double makerFee = 0.0;           // per share, in ticks; negative for a rebate
        double takerFee = 0.0;
    };

    // One of our orders, as the simulated exchange sees it.
    struct SimOrder {
        OrderId id = 0;
        Side side = Side::BUY;
        Price price = 0;
        Quantity remaining = 0;
        Quantity ahead = 0;          // historical volume in front of us at our price
        std::uint64_t joinSeq = 0;   // historical orders with a sequence number <= this are ahead
        bool live = false;           // false while the placement is in flight
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

    class Backtester;

    // Called after every feed message, and once per fill. Both see the book
    // after the message is applied; actions taken here are subject to the
    // order-entry latency.
    class Strategy {
    public:
        virtual ~Strategy() = default;
        virtual void onEvent(Backtester &, const MarketEvent &) {}
        virtual void onFill(Backtester &, const SimFill &) {}
    };

    class Backtester {
    public:
        Backtester(const Config &config, Strategy &strategy)
            : cfg(config), strat(strategy), book(config.minPrice, config.maxPrice, config.maxOrders, config.snapshotDepth) {}

        // Feed one message. Messages must arrive in time order. `truth`, when
        // given, is the feed's own snapshot after this message: the book is
        // re-synced to it before the strategy looks.
        void step(const MarketEvent &e, const TopOfBook *truth = nullptr) {
            stepWith(e, [&] { if (truth != nullptr) book.resyncTo(*truth); });
        }

        // The same, with the re-sync precomputed: `fixes` are the corrections
        // ReplayBook::resyncTo recorded for this message (see DayCache.h).
        void step(const MarketEvent &e, const std::span<const Correction> fixes) {
            stepWith(e, [&] { book.replay(fixes); });
        }

        template<class Events>
        void run(const Events &events) {
            for (const MarketEvent &e: events) step(e);
            finish();
        }

        // Load the book as it stood before the first message (the feed's
        // first snapshot, with that message undone).
        void seed(const TopOfBook &levels) {
            book.resyncTo(levels);
            updateMid();
        }

        // The same, from the corrections recorded when seeding.
        void seed(const std::span<const Correction> fixes) {
            book.replay(fixes);
            updateMid();
        }

        // Settle the remaining markouts at the last mid.
        void finish() { settleMarkouts(true); }

        // ── what a strategy can see and do ───────────────────────────────

        [[nodiscard]] Nanos now() const { return clock; }
        [[nodiscard]] std::optional<Price> bestBid() const { return book.fastBook().bestBid(); }
        [[nodiscard]] std::optional<Price> bestAsk() const { return book.fastBook().bestAsk(); }
        [[nodiscard]] Quantity levelQuantity(const Side side, const Price p) const {
            return book.fastBook().levelQuantity(side, p);
        }
        [[nodiscard]] double mid() const { return lastMid; }
        [[nodiscard]] Quantity position() const { return stats_.position; }
        [[nodiscard]] const Stats &stats() const { return stats_; }
        [[nodiscard]] const ReplayBook &marketBook() const { return book; }

        // Our order, or nullptr once it is filled or cancelled. The pointer
        // is valid until the next place(), step() or seed().
        [[nodiscard]] const SimOrder *order(const OrderId id) const {
            const auto it = findOrder(id);
            return it == orders.end() ? nullptr : &*it;
        }

        // Our open orders, by id.
        [[nodiscard]] std::span<const SimOrder> openOrders() const { return orders; }

        // Send a GTC limit order; it reaches the exchange after the latency.
        // Any part that crosses the book then trades, the rest queues.
        OrderId place(const Side side, const Price price, const Quantity quantity) {
            if (quantity <= 0) throw std::invalid_argument("quantity must be positive");
            if (price < cfg.minPrice || price > cfg.maxPrice) throw std::out_of_range("price outside the ladder");
            const OrderId id = nextId++;
            SimOrder o;
            o.id = id;
            o.side = side;
            o.price = price;
            o.remaining = quantity;
            orders.push_back(o); // ids only grow, so `orders` stays sorted
            actions.push_back({clock + cfg.orderLatency, ActionKind::Place, id});
            return id;
        }

        // False if the order is already gone or a cancel is already in flight.
        bool cancel(const OrderId id) {
            const auto it = findOrder(id);
            if (it == orders.end() || it->cancelSent) return false;
            it->cancelSent = true;
            actions.push_back({clock + cfg.orderLatency, ActionKind::Cancel, id});
            return true;
        }

    private:
        std::vector<SimOrder>::iterator findOrder(const OrderId id) {
            const auto it = std::lower_bound(orders.begin(), orders.end(), id,
                                             [](const SimOrder &o, const OrderId x) { return o.id < x; });
            return it != orders.end() && it->id == id ? it : orders.end();
        }

        [[nodiscard]] std::vector<SimOrder>::const_iterator findOrder(const OrderId id) const {
            return const_cast<Backtester *>(this)->findOrder(id);
        }

        enum class ActionKind : std::uint8_t { Place, Cancel };

        struct Action {
            Nanos due;
            ActionKind kind;
            OrderId id;
        };

        struct PendingMarkout {
            Nanos due;
            Side side;
            Price price;
            Quantity quantity;
        };

        template<class Resync>
        void stepWith(const MarketEvent &e, Resync &&resync) {
            if (e.time < clock) throw std::invalid_argument("feed went back in time at " + std::to_string(e.time));
            clock = e.time;
            ++stats_.events;

            releaseActions(clock);
            fillAgainst(e);
            applyToBook(e);
            resync();
            if (!book.dropped().empty()) retireDropped();
            updateMid();
            settleMarkouts(false);
            dispatchFills();
            strat.onEvent(*this, e);
            dispatchFills(); // actions never fill synchronously, but keep the invariant cheap to see
        }

        // Constant latency keeps `actions` in due order, so it is a FIFO.
        void releaseActions(const Nanos t) {
            while (!actions.empty() && actions.front().due <= t) {
                const Action a = actions.front();
                actions.pop_front();
                const auto it = findOrder(a.id);
                if (it == orders.end()) continue; // filled before the action landed
                if (a.kind == ActionKind::Cancel) orders.erase(it);
                else arrive(*it, a.due);
            }
            eraseFilled();
        }

        // Our order reaches the exchange: take what crosses, queue the rest
        // behind everything already resting at its price.
        void arrive(SimOrder &o, const Nanos t) {
            const Side opp = o.side == Side::BUY ? Side::SELL : Side::BUY;
            book.fastBook().forEachLevel(opp, [&](const Price p, const Quantity available) {
                if (o.side == Side::BUY ? p > o.price : p < o.price) return false;
                const Quantity q = std::min(available, o.remaining);
                fill(o, p, q, false, t);
                return o.remaining > 0;
            });
            o.live = true;
            o.ahead = levelQuantity(o.side, o.price);
            o.joinSeq = seqCounter;
        }

        // Live orders on one side, in exchange priority: best price, then
        // earliest arrival. The result lives in a reused buffer.
        const std::vector<SimOrder *> &liveOrders(const Side side) {
            std::vector<SimOrder *> &r = liveScratch;
            r.clear();
            for (SimOrder &o: orders) {
                if (o.live && o.remaining > 0 && o.side == side) r.push_back(&o);
            }
            std::sort(r.begin(), r.end(), [](const SimOrder *a, const SimOrder *b) {
                if (a->price != b->price) return a->side == Side::BUY ? a->price > b->price : a->price < b->price;
                return a->joinSeq < b->joinSeq;
            });
            return r;
        }

        // 0: resting before the feed began, ahead of everyone.
        [[nodiscard]] std::uint64_t seqOf(const OrderId id) const {
            const std::uint64_t s = arrival.get(id, 0);
            return s != 0 ? s : departed.get(id, 0);
        }

        // Orders a re-sync removed keep their arrival number, as later
        // messages may still name them, but move out of the map every add
        // and delete touches, which then holds only resting orders and stays
        // in cache.
        void retireDropped() {
            for (const OrderId id: book.dropped()) {
                if (const std::uint64_t s = arrival.get(id, 0); s != 0) {
                    departed.set(id, s);
                    arrival.erase(id);
                }
            }
            book.clearDropped();
        }

        // Whether any live order on `side` satisfies `p`. Most messages are
        // nowhere near our orders; this lets them skip building and sorting
        // the priority list.
        template<class P>
        [[nodiscard]] bool anyLive(const Side side, P &&p) const {
            for (const SimOrder &o: orders) {
                if (o.live && o.remaining > 0 && o.side == side && p(o)) return true;
            }
            return false;
        }

        void fillAgainst(const MarketEvent &e) {
            switch (e.type) {
                case EventType::Add: {
                    // An incoming order that crosses our resting price would have traded with us.
                    const Side ours = e.side == Side::BUY ? Side::SELL : Side::BUY;
                    const auto crossesUs = [&](const SimOrder &o) {
                        return ours == Side::BUY ? e.price <= o.price : e.price >= o.price;
                    };
                    if (!anyLive(ours, crossesUs)) return;
                    Quantity budget = e.quantity;
                    for (SimOrder *o: liveOrders(ours)) {
                        if (budget == 0) break;
                        const bool crosses = ours == Side::BUY ? e.price <= o->price : e.price >= o->price;
                        if (!crosses) break;
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
                    if (!anyLive(e.side, [&](const SimOrder &o) { return o.price == e.price; })) return;
                    const std::uint64_t s = seqOf(e.id);
                    for (SimOrder *o: liveOrders(e.side)) {
                        if (o->price == e.price && s <= o->joinSeq) o->ahead -= std::min(o->ahead, e.quantity);
                    }
                    break;
                }
                case EventType::Execute: {
                    const auto reaches = [&](const SimOrder &o) { // at the executed price or better
                        return e.side == Side::BUY ? o.price >= e.price : o.price <= e.price;
                    };
                    if (!anyLive(e.side, reaches)) return;
                    const std::uint64_t s = seqOf(e.id);
                    Quantity budget = e.quantity;
                    for (SimOrder *o: liveOrders(e.side)) {
                        const bool better = e.side == Side::BUY ? o->price > e.price : o->price < e.price;
                        if (o->price == e.price && s <= o->joinSeq) {
                            o->ahead -= std::min(o->ahead, e.quantity); // the hit order was in front of us
                        } else if ((better || o->price == e.price) && budget > 0) {
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
            if (book.apply(e) != ReplayBook::Applied::Known) {
                ++stats_.unknownOrders;
                return;
            }
            if (e.type == EventType::Add) {
                arrival.set(e.id, ++seqCounter);
                if (departed.size() > 0) departed.erase(e.id); // a reused id starts afresh
            }
            else if (!book.fastBook().contains(e.id)) arrival.erase(e.id);
        }

        void fill(SimOrder &o, const Price p, const Quantity q, const bool maker, const Nanos t) {
            if (q <= 0) return;
            o.remaining -= q;
            const bool buy = o.side == Side::BUY;
            stats_.position += buy ? q : -q;
            stats_.cash += (buy ? -p : p) * q;
            (buy ? stats_.boughtVolume : stats_.soldVolume) += q;
            (maker ? stats_.makerVolume : stats_.takerVolume) += q;
            stats_.fees += static_cast<double>(q) * (maker ? cfg.makerFee : cfg.takerFee);
            stats_.maxAbsPosition = std::max(stats_.maxAbsPosition, stats_.position < 0 ? -stats_.position : stats_.position);
            ++stats_.fills;
            markouts.push_back({t + cfg.markoutHorizon, o.side, p, q});
            newFills.push_back({t, o.id, o.side, p, q, maker});
        }

        void eraseFilled() {
            std::erase_if(orders, [](const SimOrder &o) { return o.remaining == 0; });
        }

        void updateMid() {
            const auto bid = bestBid();
            const auto ask = bestAsk();
            if (bid && ask) lastMid = (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
        }

        // Fill times are non-decreasing, so `markouts` is in due order.
        void settleMarkouts(const bool all) {
            while (!markouts.empty() && (all || markouts.front().due <= clock)) {
                const PendingMarkout &m = markouts.front();
                const double edge = m.side == Side::BUY ? lastMid - static_cast<double>(m.price)
                                                        : static_cast<double>(m.price) - lastMid;
                stats_.markout += edge * static_cast<double>(m.quantity);
                stats_.markoutVolume += m.quantity;
                markouts.pop_front();
            }
        }

        void dispatchFills() {
            // A callback may place or cancel, but never fills synchronously,
            // so swapping out the buffer is enough.
            while (!newFills.empty()) {
                fillBatch.clear();
                fillBatch.swap(newFills); // the two buffers trade places, keeping their capacity
                for (const SimFill &f: fillBatch) strat.onFill(*this, f);
            }
        }

        Config cfg;
        Strategy &strat;
        ReplayBook book;
        IdMap<std::uint64_t> arrival;  // resting historical order -> arrival number
        IdMap<std::uint64_t> departed; // the same, for orders a re-sync removed
        std::uint64_t seqCounter = 0;
        std::vector<SimOrder> orders; // open orders, sorted by id
        std::vector<SimOrder *> liveScratch;
        Fifo<Action> actions;
        Fifo<PendingMarkout> markouts;
        std::vector<SimFill> newFills, fillBatch;
        OrderId nextId = 1;
        Nanos clock = 0;
        double lastMid = 0.0;
        Stats stats_;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H
