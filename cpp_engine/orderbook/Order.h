//
// Created by Nicola on 09/09/2026.
//

#ifndef LIMIT_ORDER_BOOK_ORDER_H
#define LIMIT_ORDER_BOOK_ORDER_H

#include <chrono>
#include <cstdint>

using Datetime = std::chrono::system_clock::time_point;

enum class TimeInForce: std::uint8_t {
    GTC, // good till cancel
    IOC, // immediate or cancel
    FOK // fill or kill
};

enum class Side: std::uint8_t {
    BUY,
    SELL
};

class Order {
    Side side;
    std::uint64_t price;
    std::uint64_t quantity;
    TimeInForce tif;
    std::uint64_t orderId;
    Datetime timestamp;

public:

    Order(Side s, std::uint64_t pr, std::uint64_t qt, TimeInForce tif, std::uint64_t id, Datetime t): side(s), price(pr), quantity(qt), tif(tif), orderId(id), timestamp(t) {}

    Order(Side s, std::uint64_t pr, std::uint64_t qt, TimeInForce tif, std::uint64_t id): side(s), price(pr), quantity(qt), tif(tif), orderId(id) {
        timestamp = std::chrono::system_clock::now();
    }

    Side getSide() const { return side; }
    std::uint64_t getPrice() const { return price; }
    std::uint64_t getQuantity() const { return quantity; }
    TimeInForce getTIF() const { return tif; }
    std::uint64_t getOrderId() const { return orderId; }


};


#endif //LIMIT_ORDER_BOOK_ORDER_H
