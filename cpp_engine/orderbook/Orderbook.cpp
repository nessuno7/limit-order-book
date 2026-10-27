//
// Port of python_engine/orderbook/orderbook.py
//

#include "Orderbook.h"

#include <algorithm>
#include <stdexcept>
#include <string>

OrderBook::OrderBook(std::string instrument_)
    : instrument(std::move(instrument_)) {}

std::map<Price, std::deque<OrderId>> &OrderBook::sideBook(const Side side) {
    return side == Side::BUY ? bids : asks;
}

void OrderBook::add(Order *order) {
    // A market order has no price to key a level by, so it never rests.
    if (!order->price.has_value()) return;

    if (index.count(order->orderId) != 0) {
        throw std::invalid_argument("order " + std::to_string(order->orderId) + " is already resting");
    }
    auto &level = sideBook(order->side);
    const Price price = *order->price;
    auto it = level.find(price);
    if (it != level.end()) {
        it->second.push_back(order->orderId);
    } else {
        level.emplace(price, std::deque<OrderId>{order->orderId});
    }
    // Saved for quick cancelling; the order itself is stored once.
    index[order->orderId] = order;
}

void OrderBook::deleteLevel(const Side side, const Price price) {
    sideBook(side).erase(price);
}

Order *OrderBook::cancel(const OrderId orderId) {
    // .at() so a missing id throws, matching Python's KeyError from dict.pop.
    Order *order = index.at(orderId);
    index.erase(orderId);

    auto &level = sideBook(order->side).at(*order->price);
    if (level.front() == orderId) {
        level.pop_front(); // the common case: the matcher exhausts the front of the queue
    } else {
        level.erase(std::find(level.begin(), level.end(), orderId)); // user cancel from anywhere
    }

    if (level.empty()) {
        deleteLevel(order->side, *order->price);
    }
    return order;
}

std::optional<std::pair<Price, OrderId>> OrderBook::bestBid() const {
    if (bids.empty()) return std::nullopt;
    const auto &highest = *bids.rbegin();          // max(self.bids)
    return std::make_pair(highest.first, highest.second.front());
}

std::optional<std::pair<Price, OrderId>> OrderBook::bestAsk() const {
    if (asks.empty()) return std::nullopt;
    const auto &lowest = *asks.begin();            // min(self.asks)
    return std::make_pair(lowest.first, lowest.second.front());
}
