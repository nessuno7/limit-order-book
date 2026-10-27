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

#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bt {
    struct Config {
        Price minPrice = 1;
        Price maxPrice = 20'000;
        std::size_t maxOrders = 1 << 20; // historical orders resting at once
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
            if (e.time < clock) throw std::invalid_argument("feed went back in time at " + std::to_string(e.time));
            clock = e.time;
            ++stats_.events;

            releaseActions(clock);
            fillAgainst(e);
            applyToBook(e);
            if (truth != nullptr) book.resyncTo(*truth);
            updateMid();
            settleMarkouts(false);
            dispatchFills();
            strat.onEvent(*this, e);
            dispatchFills(); // actions never fill synchronously, but keep the invariant cheap to see
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

        // Our order, or nullptr once it is filled or cancelled.
        [[nodiscard]] const SimOrder *order(const OrderId id) const {
            const auto it = orders.find(id);
            return it == orders.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const std::map<OrderId, SimOrder> &openOrders() const { return orders; }

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
            orders.emplace(id, o);
            actions.push_back({clock + cfg.orderLatency, ActionKind::Place, id});
            return id;
        }

        // False if the order is already gone or a cancel is already in flight.
        bool cancel(const OrderId id) {
            const auto it = orders.find(id);
            if (it == orders.end() || it->second.cancelSent) return false;
            it->second.cancelSent = true;
            actions.push_back({clock + cfg.orderLatency, ActionKind::Cancel, id});
            return true;
        }

    private:
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

        // Constant latency keeps `actions` in due order, so it is a FIFO.
        void releaseActions(const Nanos t) {
            while (!actions.empty() && actions.front().due <= t) {
                const Action a = actions.front();
                actions.pop_front();
                const auto it = orders.find(a.id);
                if (it == orders.end()) continue; // filled before the action landed
                if (a.kind == ActionKind::Cancel) orders.erase(it);
                else arrive(it->second, a.due);
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
        // earliest arrival.
        std::vector<SimOrder *> liveOrders(const Side side) {
            std::vector<SimOrder *> r;
            for (auto &[id, o]: orders) {
                if (o.live && o.remaining > 0 && o.side == side) r.push_back(&o);
            }
            std::sort(r.begin(), r.end(), [](const SimOrder *a, const SimOrder *b) {
                if (a->price != b->price) return a->side == Side::BUY ? a->price > b->price : a->price < b->price;
                return a->joinSeq < b->joinSeq;
            });
            return r;
        }

        [[nodiscard]] std::uint64_t seqOf(const OrderId id) const {
            const auto it = arrival.find(id);
            return it == arrival.end() ? 0 : it->second; // resting before the feed began: ahead of everyone
        }

        void fillAgainst(const MarketEvent &e) {
            switch (e.type) {
                case EventType::Add: {
                    // An incoming order that crosses our resting price would have traded with us.
                    const Side ours = e.side == Side::BUY ? Side::SELL : Side::BUY;
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
                    const std::uint64_t s = seqOf(e.id);
                    for (SimOrder *o: liveOrders(e.side)) {
                        if (o->price == e.price && s <= o->joinSeq) o->ahead -= std::min(o->ahead, e.quantity);
                    }
                    break;
                }
                case EventType::Execute: {
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
            if (e.type == EventType::Add) arrival[e.id] = ++seqCounter;
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
            std::erase_if(orders, [](const auto &kv) { return kv.second.remaining == 0; });
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
                std::vector<SimFill> batch;
                batch.swap(newFills);
                for (const SimFill &f: batch) strat.onFill(*this, f);
            }
        }

        Config cfg;
        Strategy &strat;
        ReplayBook book;
        std::unordered_map<OrderId, std::uint64_t> arrival; // historical order -> arrival number
        std::uint64_t seqCounter = 0;
        std::map<OrderId, SimOrder> orders;
        std::deque<Action> actions;
        std::deque<PendingMarkout> markouts;
        std::vector<SimFill> newFills;
        OrderId nextId = 1;
        Nanos clock = 0;
        double lastMid = 0.0;
        Stats stats_;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H
