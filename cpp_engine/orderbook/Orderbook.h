//
// Port of python_engine/orderbook/orderbook.py
//

#ifndef LIMIT_ORDER_BOOK_ORDERBOOK_H
#define LIMIT_ORDER_BOOK_ORDERBOOK_H

#include "Order.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

// Price is always an integer number of ticks.
//
// The book is an index, not an owner: levels hold ids and `index` holds
// non-owning pointers, so the caller (an OrderArena) decides how long orders
// live. std::map gives ascending iteration, which is what `sorted(dict)`,
// `min(asks)` and `max(bids)` rely on in the Python.
class OrderBook {
public:
    explicit OrderBook(std::string instrument_);

    std::string instrument;
    std::map<Price, std::deque<OrderId>> bids;   // price -> resting order ids (buy side)
    std::map<Price, std::deque<OrderId>> asks;   // sell side
    std::unordered_map<OrderId, Order *> index;  // order_id -> Order, used for cancels

    std::map<Price, std::deque<OrderId>> &sideBook(Side side);

    // Rest an order. Caller guarantees it is non-marketable right now. A
    // market order is ignored (it has no level); an id that is already
    // resting throws std::invalid_argument.
    void add(Order *order);

    void deleteLevel(Side side, Price price);

    // Cancels an order from the index and its level, wherever it sits in the
    // queue, dropping the level when it empties. Returns the cancelled order;
    // an unknown id throws std::out_of_range.
    Order *cancel(OrderId orderId);

    // Highest price a trader is willing to buy at.
    [[nodiscard]] std::optional<std::pair<Price, OrderId>> bestBid() const;

    // Lowest price the market is willing to sell at.
    [[nodiscard]] std::optional<std::pair<Price, OrderId>> bestAsk() const;
};

#endif //LIMIT_ORDER_BOOK_ORDERBOOK_H
