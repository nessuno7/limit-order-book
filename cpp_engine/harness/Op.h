//
// The vocabulary shared by the fuzzer and the latency benchmarks: one book
// operation, its outcome, and a snapshot of a book's resting orders.
//

#ifndef LIMIT_ORDER_BOOK_HARNESS_OP_H
#define LIMIT_ORDER_BOOK_HARNESS_OP_H

#include "fast/FastBook.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <tuple>
#include <vector>

enum class OpKind : std::uint8_t { Add, Cancel, Modify };

struct Op {
    OpKind kind = OpKind::Add;
    OrderId id = 0;
    Side side = Side::BUY;              // Add only
    std::optional<Price> price;         // Add: nullopt for a market order. Modify: the new price.
    Quantity quantity = 0;              // Add, Modify
    TimeInForce tif = TimeInForce::GTC; // Add only

    static Op of(const OpKind kind, const OrderId id) {
        Op op;
        op.kind = kind;
        op.id = id;
        return op;
    }
};

// What happened, in terms every implementation can report.
enum class Status : std::uint8_t {
    Ok,       // accepted (an add may still be killed or trade nothing)
    NotFound, // cancel/modify of an id that is not resting
    Invalid,  // rejected input: non-positive quantity, duplicate id
};

struct Outcome {
    Status status = Status::Ok;
    std::vector<fast::Fill> fills;

    bool operator==(const Outcome &) const = default;
};

// One resting order, as (side, price, id, quantity). A snapshot lists bids
// best first, then asks best first, FIFO within each level.
struct RestingOrder {
    Side side;
    Price price;
    OrderId id;
    Quantity quantity;

    bool operator==(const RestingOrder &) const = default;
};

using Snapshot = std::vector<RestingOrder>;

inline const char *name(const Side s) { return s == Side::BUY ? "BUY" : "SELL"; }

inline const char *name(const TimeInForce t) {
    return t == TimeInForce::GTC ? "GTC" : t == TimeInForce::IOC ? "IOC" : "FOK";
}

inline const char *name(const Status s) {
    return s == Status::Ok ? "Ok" : s == Status::NotFound ? "NotFound" : "Invalid";
}

inline std::ostream &operator<<(std::ostream &out, const Op &op) {
    switch (op.kind) {
        case OpKind::Add:
            out << "add #" << op.id << " " << name(op.side) << " " << name(op.tif) << " " << op.quantity << " @ ";
            if (op.price.has_value()) out << *op.price;
            else out << "MKT";
            break;
        case OpKind::Cancel:
            out << "cancel #" << op.id;
            break;
        case OpKind::Modify:
            out << "modify #" << op.id << " -> " << op.quantity << " @ " << *op.price;
            break;
    }
    return out;
}

inline std::ostream &operator<<(std::ostream &out, const Outcome &o) {
    out << name(o.status) << " [";
    for (std::size_t i = 0; i < o.fills.size(); ++i) {
        const fast::Fill &f = o.fills[i];
        out << (i ? ", " : "") << "b#" << f.buyOrderId << "/s#" << f.sellOrderId << " " << f.quantity << "@" << f.price;
    }
    return out << "]";
}

inline std::ostream &operator<<(std::ostream &out, const RestingOrder &r) {
    return out << name(r.side) << " #" << r.id << " " << r.quantity << " @ " << r.price;
}

#endif //LIMIT_ORDER_BOOK_HARNESS_OP_H
