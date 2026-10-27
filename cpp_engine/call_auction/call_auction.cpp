//
// Port of python_engine/call_auction/call_auction.py
//

#include "call_auction.h"

#include <stdexcept>
#include <string>

CallAuction::CallAuction(PricingRule &pricing_) : pricing(pricing_) {}

std::vector<Trade> CallAuction::submit(Order *order) {
    if (pendingIds.count(order->orderId) != 0) {
        throw std::invalid_argument("order " + std::to_string(order->orderId) + " is already in this batch");
    }
    if (order->quantity <= 0) { // e.g. resubmitting an order that already filled
        throw std::invalid_argument("order " + std::to_string(order->orderId) + " has no quantity left to fill");
    }
    pricing.validate(*order);
    pending.push_back(order);
    pendingIds.insert(order->orderId);
    return {};
}

std::vector<Trade> CallAuction::run() {
    std::vector<Order *> buys;
    std::vector<Order *> sells;
    for (Order *o: pending) {
        if (o->side == Side::BUY) buys.push_back(o);
    }
    for (Order *o: pending) {
        if (o->side == Side::SELL) sells.push_back(o);
    }

    ClearResult result = pricing.clear(buys, sells);

    trades.insert(trades.end(), result.trades.begin(), result.trades.end());
    pending.clear();
    // Unfilled orders roll into the next batch.
    pending.insert(pending.end(), result.unfilled.begin(), result.unfilled.end());
    pendingIds.clear();
    for (const Order *o: pending) pendingIds.insert(o->orderId);

    return result.trades;
}
