//
// Order builders shared by the C++ tests -- the gtc()/ioc()/fok()/mkt()
// helpers of the Python test files. The fixture owns every order it makes, so
// a test just declares `Orders o;` and hands out the pointers.
//

#ifndef LIMIT_ORDER_BOOK_TEST_HELPERS_H
#define LIMIT_ORDER_BOOK_TEST_HELPERS_H

#include "orderbook/OrderArena.h"
#include "orderbook/Trade.h"

#include <chrono>
#include <optional>
#include <vector>

// datetime(2026, 1, 1, 9, 0, 0); only differences between timestamps matter.
inline const Datetime T0 = Datetime{} + std::chrono::hours(24 * 20454 + 9);

constexpr TraderId TRADER = 1;

class Orders {
public:
    Order *make(Side side, std::optional<Price> price, Quantity qty,
                TimeInForce tif = TimeInForce::GTC, int tSeconds = 0) {
        return arena.create(side, price, qty, TRADER, tif, T0 + std::chrono::seconds(tSeconds));
    }

    Order *gtc(Side side, Price price, Quantity qty, int t = 0) { return make(side, price, qty, TimeInForce::GTC, t); }
    Order *ioc(Side side, Price price, Quantity qty, int t = 0) { return make(side, price, qty, TimeInForce::IOC, t); }
    Order *fok(Side side, Price price, Quantity qty, int t = 0) { return make(side, price, qty, TimeInForce::FOK, t); }
    Order *mkt(Side side, Quantity qty, TimeInForce tif = TimeInForce::GTC, int t = 0) {
        return make(side, std::nullopt, qty, tif, t);
    }

private:
    OrderArena arena;
};

inline Quantity totalQty(const std::vector<Trade> &trades) {
    Quantity total = 0;
    for (const Trade &t: trades) total += t.quantity;
    return total;
}

inline bool contains(const std::vector<Order *> &orders, const Order *o) {
    for (const Order *x: orders) {
        if (x->orderId == o->orderId) return true;
    }
    return false;
}

inline const Order *findById(const std::vector<Order *> &orders, OrderId id) {
    for (const Order *x: orders) {
        if (x->orderId == id) return x;
    }
    return nullptr;
}

#endif //LIMIT_ORDER_BOOK_TEST_HELPERS_H
