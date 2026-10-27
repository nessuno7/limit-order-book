//
// Port of python_engine/orderbook/order.py
//

#include "Order.h"

#include <stdexcept>
#include <string>

namespace {
    OrderId nextOrderId() {
        static OrderId counter = 0;
        return ++counter;
    }

    std::uint64_t nextArrival() {
        static std::uint64_t counter = 0;
        return ++counter;
    }

    std::uint64_t nextSymbolId() {
        static std::uint64_t counter = 0;
        return ++counter;
    }

    Quantity requirePositive(const Quantity quantity) {
        if (quantity <= 0) {
            throw std::invalid_argument("quantity must be positive, got " + std::to_string(quantity));
        }
        return quantity;
    }
}

Symbol::Symbol(std::string instrument_)
    : instrument(std::move(instrument_)), id(nextSymbolId()) {}

Order::Order(const Side side_,
             std::optional<Price> price_,
             const Quantity quantity_,
             const TraderId traderId_,
             const TimeInForce tif_,
             std::optional<Datetime> timestamp_)
    : side(side_),
      price(price_),
      quantity(requirePositive(quantity_)),
      traderId(traderId_),
      tif(tif_),
      orderId(nextOrderId()),
      timestamp(timestamp_.value_or(std::chrono::system_clock::now())),
      seq(nextArrival()) {}

void Order::renew(std::optional<Datetime> timestamp_) {
    timestamp = timestamp_.value_or(std::chrono::system_clock::now());
    seq = nextArrival();
}
