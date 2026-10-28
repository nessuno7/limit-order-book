//
// Port of python_engine/engine.py
//
// An OrderBook holds resting orders; a MatchingEngine decides the rule by
// which an incoming order matches against it. Every market type (continuous,
// uniform call auction, discriminatory call auction) is just a different
// MatchingEngine, so they all share ONE interface.
//
// Two independent axes:
//     timing  : continuous matching   vs.  call auction (batch then cross)
//     pricing : uniform (one price)   vs.  discriminatory (pay-as-bid)
//

#ifndef LIMIT_ORDER_BOOK_ENGINE_H
#define LIMIT_ORDER_BOOK_ENGINE_H

#include "orderbook/OrderBook.h"
#include "orderbook/Trade.h"

#include <vector>

class MatchingEngine {
public:
    MatchingEngine();
    virtual ~MatchingEngine() = default;

    MatchingEngine(const MatchingEngine &) = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;

    OrderBook book;
    std::vector<Trade> trades;

    // Accept one order; return the trades it generated (maybe empty).
    virtual std::vector<Trade> submit(Order *order) = 0;
};

#endif //LIMIT_ORDER_BOOK_ENGINE_H
