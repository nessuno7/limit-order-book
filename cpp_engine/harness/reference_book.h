//
// The oracle for the fuzzer: a deliberately naive continuous order book.
//
// Written from the matching rules, not from either real implementation, and
// kept small enough to check by eye. Each side is one std::vector sorted best
// first (bids high to low, asks low to high), FIFO among equal prices.
// Everything is a linear scan: insertion finds its position, matching eats
// the front, cancel searches. Slow on purpose, so there is no cleverness for
// a bug to hide in.
//
// Rules:
//   - A buy crosses an ask priced <= its limit; a sell crosses a bid priced
//     >= its limit; a market order (no limit) crosses anything.
//   - Fills print at the resting order's price, best level first, oldest
//     first within a level.
//   - FOK: if the crossing quantity on the other side is short of the order,
//     nothing trades.
//   - Only a GTC limit remainder rests; IOC, FOK and market remainders vanish.
//   - modify: a reduction at the same price keeps its place; anything else is
//     a cancel followed by a fresh GTC submit, which may trade.
//

#ifndef LIMIT_ORDER_BOOK_REFERENCE_BOOK_H
#define LIMIT_ORDER_BOOK_REFERENCE_BOOK_H

#include "harness/op.h"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

class ReferenceBook {
public:
    Outcome apply(const Op &op) {
        switch (op.kind) {
            case OpKind::Add: return submit(op.id, op.side, op.price, op.quantity, op.tif);
            case OpKind::Cancel: return {cancel(op.id) ? Status::Ok : Status::NotFound, {}};
            case OpKind::Modify: return modify(op.id, *op.price, op.quantity);
        }
        return {Status::Invalid, {}};
    }

    [[nodiscard]] bool contains(const OrderId id) const { return live.count(id) != 0; }
    [[nodiscard]] std::size_t restingOrders() const { return live.size(); }

    [[nodiscard]] std::optional<Price> priceOf(const OrderId id) const {
        const auto it = live.find(id);
        if (it == live.end()) return std::nullopt;
        for (const Resting &r: it->second == Side::BUY ? bids : asks) {
            if (r.id == id) return r.price;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Price> bestBid() const {
        return bids.empty() ? std::nullopt : std::optional<Price>(bids.front().price);
    }

    [[nodiscard]] std::optional<Price> bestAsk() const {
        return asks.empty() ? std::nullopt : std::optional<Price>(asks.front().price);
    }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot s;
        for (const Resting &r: bids) s.push_back({Side::BUY, r.price, r.id, r.quantity});
        for (const Resting &r: asks) s.push_back({Side::SELL, r.price, r.id, r.quantity});
        return s;
    }

private:
    struct Resting {
        OrderId id;
        Price price;
        Quantity quantity;
    };

    Outcome submit(const OrderId id, const Side side, const std::optional<Price> limit,
                   Quantity quantity, const TimeInForce tif) {
        if (quantity <= 0 || contains(id)) return {Status::Invalid, {}};
        std::vector<Resting> &other = side == Side::BUY ? asks : bids;
        const auto crosses = [&](const Resting &r) {
            if (!limit.has_value()) return true;
            return side == Side::BUY ? r.price <= *limit : r.price >= *limit;
        };

        Outcome out{Status::Ok, {}};
        if (tif == TimeInForce::FOK) {
            Quantity available = 0;
            for (const Resting &r: other) {
                if (!crosses(r)) break; // sorted best first, so the rest cannot cross either
                available += r.quantity;
            }
            if (available < quantity) return out;
        }

        while (quantity > 0 && !other.empty() && crosses(other.front())) {
            Resting &maker = other.front();
            const Quantity q = quantity < maker.quantity ? quantity : maker.quantity;
            out.fills.push_back(side == Side::BUY ? fast::Fill{id, maker.id, maker.price, q}
                                                  : fast::Fill{maker.id, id, maker.price, q});
            quantity -= q;
            maker.quantity -= q;
            if (maker.quantity == 0) {
                live.erase(maker.id);
                other.erase(other.begin());
            }
        }

        if (quantity > 0 && tif == TimeInForce::GTC && limit.has_value()) {
            std::vector<Resting> &own = side == Side::BUY ? bids : asks;
            // Behind every order at an equal or better price.
            std::size_t pos = 0;
            while (pos < own.size() && (side == Side::BUY ? own[pos].price >= *limit : own[pos].price <= *limit)) ++pos;
            own.insert(own.begin() + static_cast<std::ptrdiff_t>(pos), Resting{id, *limit, quantity});
            live[id] = side;
        }
        return out;
    }

    bool cancel(const OrderId id) {
        const auto it = live.find(id);
        if (it == live.end()) return false;
        std::vector<Resting> &own = it->second == Side::BUY ? bids : asks;
        for (std::size_t i = 0; i < own.size(); ++i) {
            if (own[i].id == id) {
                own.erase(own.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        live.erase(it);
        return true;
    }

    Outcome modify(const OrderId id, const Price price, const Quantity quantity) {
        const auto it = live.find(id);
        if (it == live.end()) return {Status::NotFound, {}};
        if (quantity <= 0) return {Status::Invalid, {}};
        const Side side = it->second;
        for (Resting &r: side == Side::BUY ? bids : asks) {
            if (r.id == id && r.price == price && quantity <= r.quantity) {
                r.quantity = quantity;
                return {Status::Ok, {}};
            }
        }
        cancel(id);
        return submit(id, side, price, quantity, TimeInForce::GTC);
    }

    std::vector<Resting> bids; // best (highest) first
    std::vector<Resting> asks; // best (lowest) first
    std::unordered_map<OrderId, Side> live; // id -> side, for contains/cancel
};

#endif //LIMIT_ORDER_BOOK_REFERENCE_BOOK_H
