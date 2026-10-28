//
// Port of python_engine/call_auction/pricing_rule.py
//

#ifndef LIMIT_ORDER_BOOK_PRICING_RULE_H
#define LIMIT_ORDER_BOOK_PRICING_RULE_H

#include "orderbook/Order.h"
#include "orderbook/Trade.h"

#include <vector>

// Python returns the pair (trades, unfilled); naming the halves reads better
// at the call sites.
struct ClearResult {
    std::vector<Trade> trades;
    std::vector<Order *> unfilled;
};

// Given the orders collected during a call auction, decide the clearing
// price(s) and how each order fills.
class PricingRule {
public:
    virtual ~PricingRule() = default;

    // Throws std::invalid_argument if this rule cannot clear `order`. Called
    // when the order is submitted, so a bad order is refused up front instead
    // of sitting in the batch and failing every later run().
    virtual void validate(const Order & /*order*/) const {}

    // Fills are applied to the orders passed in: `unfilled` holds the same
    // pointers, with their quantities reduced by what traded.
    virtual ClearResult clear(const std::vector<Order *> &buys,
                              const std::vector<Order *> &sells) = 0;
};

#endif //LIMIT_ORDER_BOOK_PRICING_RULE_H
