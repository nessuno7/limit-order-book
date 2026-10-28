//
// Event-driven market-making backtester over a market-by-order feed.
//
// Three parts:
//   SimExchange   the historical book, our orders queued beside it, our
//                 fills and their accounting (see SimExchange.h for when we
//                 fill);
//   StrategyView  what the strategy believes under market-data latency;
//   Backtester    the two latency legs between them, and the Strategy API.
//
// Latency is constant, and has two legs, as in hftbacktest:
//   - order entry (orderLatency): a placement or cancel sent at t takes
//     effect before the first message at or after t + orderLatency, so a
//     cancelled order can still fill while the cancel is in flight;
//   - market data (mdLatency): everything the exchange sends us, book
//     messages, fills, and acknowledgements of placements and cancels,
//     reaches the strategy mdLatency after it happened. The strategy sees
//     a second book that lags the exchange's, and its own orders and
//     position as the reports it has received describe them. So it can
//     quote off a stale touch, and re-send a quote that has already filled.
// A decision therefore reaches the exchange mdLatency + orderLatency after
// the message that prompted it. With mdLatency = 0 the strategy sees the
// exchange's own state, and no second book is kept.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H
#define LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H

#include "backtest/FlatContainers.h"
#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"
#include "backtest/SimExchange.h"
#include "backtest/StrategyView.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace bt {
    class Backtester;

    // Called after every feed message, and once per fill, as the strategy
    // receives them: mdLatency after the exchange saw them. Both see the
    // book after the message is applied; actions taken here are subject to
    // the order-entry latency.
    class Strategy {
    public:
        virtual ~Strategy() = default;
        virtual void onEvent(Backtester &, const MarketEvent &) {}
        virtual void onFill(Backtester &, const SimFill &) {}
    };

    class Backtester {
    public:
        Backtester(const Config &config, Strategy &strategy) : cfg(config), strat(strategy), exchange(config) {
            if (cfg.mdLatency < 0 || cfg.orderLatency < 0) throw std::invalid_argument("latency cannot be negative");
            if (cfg.mdLatency > 0) {
                view.emplace(config);
                seenBook = &view->book();
                seenOrders = &view->orders();
            }
        }

        // The strategy's view points into the object.
        Backtester(const Backtester &) = delete;
        Backtester &operator=(const Backtester &) = delete;

        // Feed one message. Messages must arrive in time order. `truth`, when
        // given, is the feed's own snapshot after this message: the book is
        // re-synced to it before the strategy looks.
        void step(const MarketEvent &e, const TopOfBook *truth = nullptr) {
            stepWith(e, [&](ReplayBook &book) -> std::span<const Correction> {
                if (truth == nullptr) return {};
                if (!view) {
                    book.resyncTo(*truth);
                    return {};
                }
                fixScratch.clear(); // the lagging book repeats what this re-sync does
                book.resyncTo(*truth, &fixScratch);
                return fixScratch;
            });
        }

        // The same, with the re-sync precomputed: `fixes` are the corrections
        // ReplayBook::resyncTo recorded for this message (see DayCache.h).
        void step(const MarketEvent &e, const std::span<const Correction> fixes) {
            stepWith(e, [&](ReplayBook &book) {
                book.replay(fixes);
                return fixes;
            });
        }

        template<class Events>
        void run(const Events &events) {
            for (const MarketEvent &e: events) step(e);
            finish();
        }

        // Load the book as it stood before the first message (the feed's
        // first snapshot, with that message undone).
        void seed(const TopOfBook &levels) {
            fixScratch.clear();
            exchange.seed(levels, view ? &fixScratch : nullptr);
            if (view) view->seed(fixScratch);
        }

        // The same, from the corrections recorded when seeding.
        void seed(const std::span<const Correction> fixes) {
            exchange.seed(fixes);
            if (view) view->seed(fixes);
        }

        // Settle the remaining markouts at the last mid, and deliver what is
        // still on its way to the strategy.
        void finish() {
            exchange.finish();
            if (view) deliver(0, true);
        }

        // ── what a strategy can see and do ───────────────────────────────
        // With market-data latency, all of it is the strategy's delayed view:
        // the lagging book, and its orders and position as reported so far.

        [[nodiscard]] Nanos now() const { return localClock; }
        [[nodiscard]] std::optional<Price> bestBid() const { return seenBook->fastBook().bestBid(); }
        [[nodiscard]] std::optional<Price> bestAsk() const { return seenBook->fastBook().bestAsk(); }
        [[nodiscard]] Quantity levelQuantity(const Side side, const Price p) const {
            return seenBook->fastBook().levelQuantity(side, p);
        }
        [[nodiscard]] double mid() const { return view ? view->mid() : exchange.mid(); }
        [[nodiscard]] Quantity position() const { return view ? view->position() : exchange.stats().position; }
        [[nodiscard]] const ReplayBook &marketBook() const { return *seenBook; }

        // Our order, or nullptr once it is filled or cancelled. The pointer
        // is valid until the next place(), step() or seed().
        [[nodiscard]] const SimOrder *order(const OrderId id) const {
            const auto it = findById(*seenOrders, id);
            return it == seenOrders->end() ? nullptr : &*it;
        }

        // Our open orders, by id.
        [[nodiscard]] std::span<const SimOrder> openOrders() const { return *seenOrders; }

        // Send a GTC limit order; it reaches the exchange after the latency.
        // Any part that crosses the book then trades, the rest queues.
        OrderId place(const Side side, const Price price, const Quantity quantity) {
            if (quantity <= 0) throw std::invalid_argument("quantity must be positive");
            if (price < cfg.minPrice || price > cfg.maxPrice) throw std::out_of_range("price outside the ladder");
            const SimOrder o{.id = nextId++, .side = side, .price = price, .remaining = quantity};
            exchange.add(o);
            if (view) view->add(o);
            actions.push_back({landingTime(), ActionKind::Place, o.id});
            return o.id;
        }

        // False if, as far as the strategy knows, the order is already gone
        // or a cancel is already in flight. With market-data latency the
        // order may have filled meanwhile; the cancel then does nothing.
        bool cancel(const OrderId id) {
            if (!(view ? view->markCancelSent(id) : exchange.markCancelSent(id))) return false;
            actions.push_back({landingTime(), ActionKind::Cancel, id});
            return true;
        }

        // ── the exchange's side, for reports and tests ───────────────────

        [[nodiscard]] const Stats &stats() const { return exchange.stats(); }
        [[nodiscard]] double exchangeMid() const { return exchange.mid(); }
        [[nodiscard]] const ReplayBook &exchangeBook() const { return exchange.book(); }
        [[nodiscard]] const SimOrder *exchangeOrder(const OrderId id) const { return exchange.order(id); }
        [[nodiscard]] std::span<const SimOrder> exchangeOrders() const { return exchange.orders(); }

    private:
        enum class ActionKind : std::uint8_t { Place, Cancel };

        // A placement or cancel on its way to the exchange.
        struct Action {
            Nanos due;
            ActionKind kind;
            OrderId id;
        };

        template<class... F>
        struct Overloaded : F... {
            using F::operator()...;
        };

        // When an action sent now reaches the exchange. Inside a callback
        // the strategy's time is never behind the exchange's; an action sent
        // from outside one, between steps, cannot land in the exchange's past.
        [[nodiscard]] Nanos landingTime() const { return std::max(localClock + cfg.orderLatency, exchange.clock()); }

        template<class Resync>
        void stepWith(const MarketEvent &e, Resync &&resync) {
            if (e.time < exchange.clock()) throw std::invalid_argument("feed went back in time at " + std::to_string(e.time));
            // At equal times the exchange goes first: a report due exactly
            // now reaches the strategy after this message is processed.
            if (view) deliver(e.time, false);
            else localClock = e.time;

            // Constant latency keeps `actions` in due order, so it is a FIFO.
            while (!actions.empty() && actions.front().due <= e.time) landNextAction();
            const std::span<const Correction> fixes = exchange.onMessage(e, resync);

            if (view) {
                forwardFills();
                inbox.push(e.time + cfg.mdLatency, e, fixes);
                return;
            }
            dispatchFills();
            strat.onEvent(*this, e);
            dispatchFills(); // actions never fill synchronously, but keep the invariant cheap to see
        }

        void landNextAction() {
            const Action a = actions.front();
            actions.pop_front();
            const bool landed = a.kind == ActionKind::Cancel ? exchange.cancel(a.id) : exchange.arrive(a.id, a.due);
            if (!landed || !view) return;
            forwardFills(); // a placement that crosses fills before it is acknowledged
            if (a.kind == ActionKind::Cancel) inbox.push(a.due + cfg.mdLatency, Cancelled{a.id});
            else inbox.push(a.due + cfg.mdLatency, Accepted{a.id});
        }

        // Delivers the reports due before `until` (or all of them), each at
        // its own time. Actions the strategy sends meanwhile reach the
        // exchange in between, in time order, so exchange time only grows
        // and the inbox stays in due order. Once the feed has ended (`all`),
        // actions no longer reach the exchange.
        void deliver(const Nanos until, const bool all) {
            for (;;) {
                const bool haveReport = !inbox.empty() && (all || inbox.nextDue() < until);
                const bool haveAction = !all && !actions.empty() && actions.front().due < until;
                if (!haveReport && !haveAction) return;
                if (haveAction && (!haveReport || actions.front().due <= inbox.nextDue())) {
                    landNextAction(); // the exchange goes first at equal times
                    continue;
                }
                receive(inbox.pop());
            }
        }

        void receive(const Report &r) {
            localClock = r.due;
            std::visit(Overloaded{
                           [&](const MarketReport &m) {
                               view->onMarket(m.event, inbox.takeFixes(m));
                               strat.onEvent(*this, m.event);
                           },
                           [&](const SimFill &f) {
                               view->onFill(f);
                               strat.onFill(*this, f);
                           },
                           [&](const Accepted &a) { view->onAccepted(a.id); },
                           [&](const Cancelled &c) { view->onCancelled(c.id); },
                       }, r.what);
        }

        // With market-data latency: the exchange's new fills start towards the strategy.
        void forwardFills() {
            if (!exchange.hasFills()) return;
            exchange.takeFills(fillBatch);
            for (const SimFill &f: fillBatch) inbox.push(f.time + cfg.mdLatency, f);
        }

        // Without it: the strategy hears of them now. A callback may place
        // or cancel, but never fills synchronously, so one swap is enough.
        void dispatchFills() {
            while (exchange.hasFills()) {
                exchange.takeFills(fillBatch);
                for (const SimFill &f: fillBatch) strat.onFill(*this, f);
            }
        }

        Config cfg;
        Strategy &strat;
        SimExchange exchange;
        Fifo<Action> actions;             // strategy -> exchange

        // What the strategy reads: the exchange's state, or the lagging
        // view. Chosen once: a `view ?` on every call costs 4% of a replay.
        const ReplayBook *seenBook = &exchange.book();
        const std::vector<SimOrder> *seenOrders = &exchange.orders();

        OrderId nextId = 1;
        Nanos localClock = 0; // the strategy's time; the exchange's without mdLatency
        std::vector<SimFill> fillBatch;
        std::vector<Correction> fixScratch;

        // Only with market-data latency. Kept last: the rest is what every
        // message touches.
        ReportQueue inbox;                // exchange -> strategy
        std::optional<StrategyView> view;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_BACKTESTER_H
