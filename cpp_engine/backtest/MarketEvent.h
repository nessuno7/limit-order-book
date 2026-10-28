//
// One message of a market-by-order feed: what the exchange did to one
// resting order. The vocabulary is LOBSTER's (types 1-4), which ITCH's
// add / cancel / delete / execute messages map onto directly.
//
// Every message carries the side and price of the order it names, as LOBSTER
// does. The backtester relies on that: orders resting before the feed starts
// are never added, but their executions and cancels still move the queue.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_MARKET_EVENT_H
#define LIMIT_ORDER_BOOK_BACKTEST_MARKET_EVENT_H

#include "Types.h"
#include "orderbook/Order.h"

#include <cstdint>
#include <ostream>

namespace bt {
    using Nanos = std::int64_t; // feed time, nanoseconds since midnight

    enum class EventType : std::uint8_t {
        Add,     // a new order rests (LOBSTER 1)
        Cancel,  // part of a resting order is cancelled; quantity = amount removed (LOBSTER 2)
        Delete,  // a resting order is removed; quantity = what was left (LOBSTER 3)
        Execute, // a resting order trades; quantity = amount executed (LOBSTER 4)
    };

    struct MarketEvent {
        Nanos time = 0;
        EventType type = EventType::Add;
        OrderId id = 0;
        Side side = Side::BUY; // side of the resting order, not of the aggressor
        Price price = 0;
        Quantity quantity = 0;

        bool operator==(const MarketEvent &) const = default;
    };

    // Side arithmetic, for an order on `side`.
    inline Side opposite(const Side side) { return side == Side::BUY ? Side::SELL : Side::BUY; }

    // True if `a` is a strictly better price than `b`: higher for a bid, lower for an ask.
    inline bool better(const Side side, const Price a, const Price b) { return side == Side::BUY ? a > b : a < b; }
    inline bool atLeastAsGood(const Side side, const Price a, const Price b) { return !better(side, b, a); }

    // +q for a buy, -q for a sell: the change in position.
    inline Quantity signedQuantity(const Side side, const Quantity q) { return side == Side::BUY ? q : -q; }

    inline const char *name(const EventType t) {
        switch (t) {
            case EventType::Add: return "Add";
            case EventType::Cancel: return "Cancel";
            case EventType::Delete: return "Delete";
            case EventType::Execute: return "Execute";
        }
        return "?";
    }

    inline std::ostream &operator<<(std::ostream &out, const MarketEvent &e) {
        return out << e.time << " " << name(e.type) << " #" << e.id << " " << (e.side == Side::BUY ? "BUY" : "SELL")
               << " " << e.quantity << " @ " << e.price;
    }
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_MARKET_EVENT_H
