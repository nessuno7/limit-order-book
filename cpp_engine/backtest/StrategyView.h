//
// The strategy's side of a market-data link with latency: what the exchange
// reports, on its way to the strategy (ReportQueue), and what the strategy
// believes once those reports arrive (StrategyView): a second book that lags
// the exchange's, and its own orders and position as reported so far.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_STRATEGY_VIEW_H
#define LIMIT_ORDER_BOOK_BACKTEST_STRATEGY_VIEW_H

#include "backtest/FlatContainers.h"
#include "backtest/MarketEvent.h"
#include "backtest/ReplayBook.h"
#include "backtest/SimExchange.h"

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace bt {
    // A feed message; its re-sync corrections travel in ReportQueue beside it.
    struct MarketReport {
        MarketEvent event;
        std::uint32_t fixes;
    };

    struct Accepted {
        OrderId id;
    };

    struct Cancelled {
        OrderId id;
    };

    struct Report {
        Nanos due; // when it reaches the strategy
        std::variant<MarketReport, SimFill, Accepted, Cancelled> what;
    };

    // Reports in flight. Each is due a constant latency after its exchange
    // time, and exchange time only grows, so a FIFO keeps them in due order.
    class ReportQueue {
    public:
        [[nodiscard]] bool empty() const { return reports.empty(); }
        [[nodiscard]] Nanos nextDue() const { return reports.front().due; }

        void push(const Nanos due, const MarketEvent &e, const std::span<const Correction> fixes) {
            reports.push_back({due, MarketReport{e, static_cast<std::uint32_t>(fixes.size())}});
            for (const Correction &c: fixes) pendingFixes.push_back(c);
        }

        template<class T>
        void push(const Nanos due, const T &what) { reports.push_back({due, what}); }

        Report pop() {
            Report r = reports.front();
            reports.pop_front();
            return r;
        }

        // The corrections of the MarketReport just popped, valid until the next call.
        std::span<const Correction> takeFixes(const MarketReport &m) {
            batch.clear();
            for (std::uint32_t i = 0; i < m.fixes; ++i) {
                batch.push_back(pendingFixes.front());
                pendingFixes.pop_front();
            }
            return batch;
        }

    private:
        Fifo<Report> reports;
        Fifo<Correction> pendingFixes;
        std::vector<Correction> batch;
    };

    class StrategyView {
    public:
        explicit StrategyView(const Config &cfg)
            : book_(cfg.minPrice, cfg.maxPrice, cfg.maxOrders, cfg.snapshotDepth) {}

        void seed(const std::span<const Correction> fixes) {
            book_.replay(fixes);
            book_.clearDropped();
            updateMid();
        }

        void onMarket(const MarketEvent &e, const std::span<const Correction> fixes) {
            book_.apply(e);
            book_.replay(fixes);
            book_.clearDropped();
            updateMid();
        }

        void onFill(const SimFill &f) {
            position_ += signedQuantity(f.side, f.quantity);
            if (const auto it = findById(orders_, f.id); it != orders_.end()) {
                it->remaining -= f.quantity;
                if (it->remaining <= 0) orders_.erase(it);
            }
        }

        void onAccepted(const OrderId id) {
            if (const auto it = findById(orders_, id); it != orders_.end()) it->live = true;
        }

        void onCancelled(const OrderId id) {
            if (const auto it = findById(orders_, id); it != orders_.end()) orders_.erase(it);
        }

        // An order the strategy has just sent; ids only grow.
        void add(const SimOrder &o) { orders_.push_back(o); }

        // False if, as far as the strategy knows, `id` is gone or already
        // has a cancel in flight.
        bool markCancelSent(const OrderId id) {
            const auto it = findById(orders_, id);
            if (it == orders_.end() || it->cancelSent) return false;
            it->cancelSent = true;
            return true;
        }

        [[nodiscard]] const ReplayBook &book() const { return book_; }
        [[nodiscard]] const std::vector<SimOrder> &orders() const { return orders_; }
        [[nodiscard]] Quantity position() const { return position_; }
        [[nodiscard]] double mid() const { return mid_; }

    private:
        void updateMid() {
            if (const auto m = touchMid(book_)) mid_ = *m;
        }

        ReplayBook book_;                // mdLatency behind the exchange's
        std::vector<SimOrder> orders_;   // as reported so far, sorted by id
        Quantity position_ = 0;
        double mid_ = 0.0;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_STRATEGY_VIEW_H
