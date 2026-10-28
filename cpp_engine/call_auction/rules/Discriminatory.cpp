//
// Port of python_engine/call_auction/rules/discriminatory.py
//

#include "Discriminatory.h"

#include "call_auction/FillOrKill.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

using fok::popFront;

void Discriminatory::validate(const Order &order) const {
    if (!order.price.has_value()) {
        throw std::invalid_argument("Discriminatory clearing requires limit orders, got a market order");
    }
}

ClearResult Discriminatory::clear(const std::vector<Order *> &buys,
                                  const std::vector<Order *> &sells) {
    for (const Order *o: buys) validate(*o);
    for (const Order *o: sells) validate(*o);

    // Buys: most expensive on top (price descending), then oldest first.
    std::vector<Order *> sortedBuys = buys;
    std::stable_sort(sortedBuys.begin(), sortedBuys.end(), [](const Order *a, const Order *b) {
        if (*a->price != *b->price) return *a->price > *b->price;
        return a->priority() < b->priority();
    });

    // Sells: cheapest on top (price ascending), then oldest first.
    std::vector<Order *> sortedSells = sells;
    std::stable_sort(sortedSells.begin(), sortedSells.end(), [](const Order *a, const Order *b) {
        if (*a->price != *b->price) return *a->price < *b->price;
        return a->priority() < b->priority();
    });

    std::vector<Trade> trades;

    Order *buyOrder = sortedBuys.empty() ? nullptr : popFront(sortedBuys);
    Order *sellOrder = sortedSells.empty() ? nullptr : popFront(sortedSells);

    while (buyOrder != nullptr && sellOrder != nullptr) {
        {
            std::vector<Order *> opp{sellOrder};
            opp.insert(opp.end(), sortedSells.begin(), sortedSells.end());
            buyOrder = fok::helper(buyOrder, sortedBuys, opp);
        }
        if (buyOrder == nullptr) break;
        {
            std::vector<Order *> opp{buyOrder};
            opp.insert(opp.end(), sortedBuys.begin(), sortedBuys.end());
            sellOrder = fok::helper(sellOrder, sortedSells, opp);
        }
        if (sellOrder == nullptr) break;

        const OrderId bId = buyOrder->orderId;
        const OrderId sId = sellOrder->orderId;

        // Beyond the limit on both sides: the buy would go over and the sell
        // would go under.
        if (*buyOrder->price < *sellOrder->price) break;

        // Price is set by time priority: the resting (older) order's price.
        const Price restingPrice = buyOrder->priority() < sellOrder->priority()
                                       ? *buyOrder->price
                                       : *sellOrder->price;

        Quantity restingQuantity;
        if (buyOrder->quantity == sellOrder->quantity) {
            restingQuantity = sellOrder->quantity;
            buyOrder->quantity = sellOrder->quantity = 0;
            buyOrder = sortedBuys.empty() ? nullptr : popFront(sortedBuys);
            sellOrder = sortedSells.empty() ? nullptr : popFront(sortedSells);
        } else if (buyOrder->quantity < sellOrder->quantity) {
            restingQuantity = buyOrder->quantity;
            sellOrder->quantity -= restingQuantity;
            buyOrder->quantity = 0;
            buyOrder = sortedBuys.empty() ? nullptr : popFront(sortedBuys);
        } else {
            restingQuantity = sellOrder->quantity;
            buyOrder->quantity -= restingQuantity;
            sellOrder->quantity = 0;
            sellOrder = sortedSells.empty() ? nullptr : popFront(sortedSells);
        }

        trades.emplace_back(restingPrice, restingQuantity, bId, sId);
    }

    // IOC and FOK never roll over.
    std::vector<Order *> unfilled;
    for (Order *o: sortedSells) {
        if (o->tif == TimeInForce::GTC) unfilled.push_back(o);
    }
    for (Order *o: sortedBuys) {
        if (o->tif == TimeInForce::GTC) unfilled.push_back(o);
    }
    if (buyOrder != nullptr && buyOrder->quantity > 0 && buyOrder->tif == TimeInForce::GTC) {
        unfilled.push_back(buyOrder);
    }
    if (sellOrder != nullptr && sellOrder->quantity > 0 && sellOrder->tif == TimeInForce::GTC) {
        unfilled.push_back(sellOrder);
    }

    return ClearResult{std::move(trades), std::move(unfilled)};
}
