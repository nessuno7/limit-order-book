//
// Port of python_engine/call_auction/rules/discriminatory.py
//

#ifndef LIMIT_ORDER_BOOK_DISCRIMINATORY_H
#define LIMIT_ORDER_BOOK_DISCRIMINATORY_H

#include "call_auction/pricing_rule.h"

#include <vector>

// Pay-as-bid clearing: priority is price, then time, and each trade prints at
// the resting (older) order's price. GTC remainders roll into the next batch;
// IOC and FOK remainders are dropped.
class Discriminatory : public PricingRule {
public:
    // Every trade prints at a limit price, so market orders are refused with
    // std::invalid_argument. Use Uniform for market orders.
    void validate(const Order &order) const override;

    ClearResult clear(const std::vector<Order *> &buys,
                      const std::vector<Order *> &sells) override;
};

#endif //LIMIT_ORDER_BOOK_DISCRIMINATORY_H
