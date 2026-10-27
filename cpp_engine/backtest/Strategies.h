//
// Reference strategies. Deliberately simple: they exist to exercise the
// backtester and to give later strategies a baseline to beat.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_STRATEGIES_H
#define LIMIT_ORDER_BOOK_BACKTEST_STRATEGIES_H

#include "backtest/Backtester.h"

namespace bt {
    // Joins the best bid and the best ask with a fixed size, and follows the
    // touch when it moves. Stops adding to a side once the position limit is
    // reached on it.
    class QuoteAtTouch : public Strategy {
    public:
        QuoteAtTouch(const Quantity quoteSize, const Quantity positionLimit)
            : size(quoteSize), maxPosition(positionLimit) {}

        void onEvent(Backtester &bt, const MarketEvent &) override {
            const auto bid = bt.bestBid();
            const auto ask = bt.bestAsk();
            if (!bid || !ask) return;
            requote(bt, bidOrder, Side::BUY, *bid, bt.position() + size <= maxPosition);
            requote(bt, askOrder, Side::SELL, *ask, -bt.position() + size <= maxPosition);
        }

    private:
        void requote(Backtester &bt, OrderId &current, const Side side, const Price target, const bool allowed) {
            const SimOrder *o = current != 0 ? bt.order(current) : nullptr;
            if (o == nullptr) current = 0; // filled or cancelled
            if (o != nullptr && (!allowed || o->price != target)) {
                bt.cancel(current);
                current = 0;
            }
            if (current == 0 && allowed) current = bt.place(side, target, size);
        }

        Quantity size;
        Quantity maxPosition;
        OrderId bidOrder = 0;
        OrderId askOrder = 0;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_STRATEGIES_H
