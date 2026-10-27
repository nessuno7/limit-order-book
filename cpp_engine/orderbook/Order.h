//
// Port of python_engine/orderbook/order.py
//

#ifndef LIMIT_ORDER_BOOK_ORDER_H
#define LIMIT_ORDER_BOOK_ORDER_H

#include "Types.h"

#include <optional>
#include <string>
#include <utility>

enum class Side: std::uint8_t {
    BUY,
    SELL
};

enum class TimeInForce: std::uint8_t {
    GTC, // good-till-cancel: unfilled remainder rests in the book
    IOC, // immediate-or-cancel: fills what it can now, rest is dropped
    FOK  // fill-or-kill: fills in full immediately or is cancelled
};

// Mirrors the Python dataclass. The engine never reads it -- OrderBook is
// constructed with a bare instrument name -- but it is part of the module.
struct Symbol {
    std::string instrument; // e.g. "AAPL"
    std::uint64_t id;

    explicit Symbol(std::string instrument_);
};

struct Order {
    // Public data, not getters-over-privates: every algorithm here mutates
    // `quantity` in place through an alias, exactly as the Python does.
    Side side;
    std::optional<Price> price; // nullopt == market order
    Quantity quantity;
    TraderId traderId;
    TimeInForce tif;
    OrderId orderId;
    Datetime timestamp;
    // Arrival counter. Timestamps can tie, so priority is (timestamp, seq).
    std::uint64_t seq;

    // Stands in for Python's uuid4(). Only identity matters here, and a
    // counter makes ids readable when tracing a book by hand.
    //
    // Throws std::invalid_argument for a non-positive quantity. Only checked
    // here: the engines legitimately drive quantity down to 0 as it fills.
    Order(Side side_,
          std::optional<Price> price_,
          Quantity quantity_,
          TraderId traderId_,
          TimeInForce tif_ = TimeInForce::GTC,
          std::optional<Datetime> timestamp_ = std::nullopt);

    // The implicit copy constructor is what `copy.copy(o)` maps to, and it
    // must carry `orderId` and `seq` across unchanged -- Uniform keys its
    // write-back by id and sorts copies by priority.
    Order(const Order &) = default;
    Order &operator=(const Order &) = default;

    [[nodiscard]] bool isMarket() const { return !price.has_value(); }

    // Moves the order to the back of time priority: a new timestamp (now, by
    // default) and a new arrival number. Used when an amendment loses its
    // place in the queue.
    void renew(std::optional<Datetime> timestamp_ = std::nullopt);

    // Time priority: older first, ties broken by arrival.
    [[nodiscard]] std::pair<Datetime, std::uint64_t> priority() const { return {timestamp, seq}; }

    [[nodiscard]] Side getSide() const { return side; }
    [[nodiscard]] std::optional<Price> getPrice() const { return price; }
    [[nodiscard]] Quantity getQuantity() const { return quantity; }
    [[nodiscard]] TimeInForce getTIF() const { return tif; }
    [[nodiscard]] OrderId getOrderId() const { return orderId; }
    [[nodiscard]] Datetime getTimestamp() const { return timestamp; }
};

#endif //LIMIT_ORDER_BOOK_ORDER_H
