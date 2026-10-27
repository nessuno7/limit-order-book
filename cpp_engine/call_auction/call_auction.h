//
// Port of python_engine/call_auction/call_auction.py
//

#ifndef LIMIT_ORDER_BOOK_CALL_AUCTION_H
#define LIMIT_ORDER_BOOK_CALL_AUCTION_H

#include "Engine.h"
#include "pricing_rule.h"

#include <unordered_set>
#include <vector>

// Collects orders without matching, then crosses them all at once when run()
// is called. The pricing rule is injected, so the SAME class supports both
// uniform and discriminatory clearing -- just swap the strategy.
class CallAuction : public MatchingEngine {
public:
    explicit CallAuction(PricingRule &pricing_);

    // Collect only; no immediate match. The order keeps the timestamp it was
    // created with, not the time the batch crossed, so a maker always exists
    // and the trade price can be the maker's price.
    // Throws std::invalid_argument for an order already in the batch, one with
    // no quantity left, or one the pricing rule cannot clear.
    std::vector<Trade> submit(Order *order) override;

    std::vector<Trade> run();

    [[nodiscard]] const std::vector<Order *> &getPending() const { return pending; }

private:
    PricingRule &pricing;
    std::vector<Order *> pending;
    std::unordered_set<OrderId> pendingIds; // O(1) duplicate check
};

#endif //LIMIT_ORDER_BOOK_CALL_AUCTION_H
