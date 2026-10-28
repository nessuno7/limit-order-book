//
// Port of python_engine/call_auction/rules/uniform.py
//

#ifndef LIMIT_ORDER_BOOK_UNIFORM_H
#define LIMIT_ORDER_BOOK_UNIFORM_H

#include "call_auction/PricingRule.h"
#include "orderbook/OrderArena.h"

#include <map>
#include <vector>

// Single-price clearing: every trade in the batch prints at p*, the price that
// maximises executed volume. Each candidate price is simulated in full on
// copies of the orders, and only the winner's simulation is written back.
class Uniform : public PricingRule {
public:
    ClearResult clear(const std::vector<Order *> &buys,
                      const std::vector<Order *> &sells) override;

protected:
    // Picks among prices that tie on both volume and surplus: the highest,
    // as test_uniform.py's docstring specifies. Deterministic on purpose.
    virtual Price chooseTieBreak(const std::vector<Price> &tiedPrices);

private:
    static void populateLevels(const std::vector<Order *> &orders,
                               std::map<Price, std::vector<Order *>> &levels);

    // Returns *copies* (owned by `pool`) of every order eligible at
    // `priceLevel`, oldest first. The simulation mutates their quantities, so
    // the originals must not be touched -- this is Python's `copy.copy(o)`.
    static std::vector<Order *> createOrderList(Price priceLevel,
                                                const std::map<Price, std::vector<Order *>> &orders,
                                                Side side,
                                                const std::vector<Order *> &marketOrders,
                                                OrderArena &pool);
};

#endif //LIMIT_ORDER_BOOK_UNIFORM_H
