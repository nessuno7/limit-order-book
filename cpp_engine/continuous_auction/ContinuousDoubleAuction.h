//
// Port of python_engine/continuous_auction/continuous_auction.py
//

#ifndef LIMIT_ORDER_BOOK_CONTINUOUS_AUCTION_H
#define LIMIT_ORDER_BOOK_CONTINUOUS_AUCTION_H

#include "Engine.h"

#include <optional>
#include <vector>

// Each incoming order matches immediately against the best opposing resting
// orders by price-time priority. Trades print at the *resting* order's price,
// so execution is inherently discriminatory (different fills, different
// prices). Any GTC remainder rests.
class ContinuousDoubleAuction : public MatchingEngine {
public:
    // Throws std::invalid_argument if `order` is already resting or has no
    // quantity left (e.g. it already filled).
    std::vector<Trade> submit(Order *order) override;

    // Amends a resting order. Shrinking it at the same price keeps its place
    // in the queue. Any other change (a new price, or more quantity) loses
    // time priority: the order leaves the book and is submitted again, so a
    // new price that crosses trades immediately.
    // Throws std::out_of_range if `orderId` is not resting, and
    // std::invalid_argument if `newQuantity` is not positive.
    std::vector<Trade> modify(OrderId orderId, Price newPrice, Quantity newQuantity,
                              std::optional<Datetime> timestamp = std::nullopt);

private:
    // Returns true while `order` still has quantity left to fill.
    bool gtcHelper(Price bestPrice, Order *ctr, Order *order, std::vector<Trade> &newTrades);

    // True when the book holds enough opposing volume within the limit to
    // fill `order` completely.
    bool fokWalk(const Order *order);

    // Rests a GTC limit remainder; anything else is dropped.
    void rest(Order *order);
};

#endif //LIMIT_ORDER_BOOK_CONTINUOUS_AUCTION_H
