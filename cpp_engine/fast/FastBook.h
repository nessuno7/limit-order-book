//
// A latency-oriented continuous order book with the same matching rules as
// ContinuousDoubleAuction (price-time priority, trades at the resting price,
// GTC/IOC/FOK, market orders, cancel, modify), built for a flat latency
// distribution instead of readability:
//
//   - Price ladder: a flat array of levels indexed by price tick, plus a
//     bitmap of non-empty levels, so finding the next best level after one
//     empties is a word scan (countr_zero/countl_zero), not a tree walk.
//     MapLadder keeps the same interface on a std::map, for comparison.
//   - Intrusive lists: each level is a doubly linked list threaded through
//     the order nodes themselves, so cancelling from the middle of a queue is
//     O(1) and a level owns no container.
//   - Object pool: every order node is preallocated; a LIFO free list hands
//     out slots, so recently freed (cache-warm) slots are reused first.
//   - Id index: open addressing with linear probing and backward-shift
//     deletion (no tombstones), preallocated at twice the pool size.
//   - Fills go into a buffer reserved up front and are returned as a span.
//
// After construction the array version allocates nothing on submit, cancel
// or modify, unless one call produces more fills than `fillCapacity`.
//
// Differences from ContinuousDoubleAuction, by design:
//   - The caller supplies order ids; there is no Order object or arena.
//   - Prices must lie in [minPrice, maxPrice] (std::out_of_range otherwise).
//   - Fills carry no timestamp, and the book keeps no trade history.
//

#ifndef LIMIT_ORDER_BOOK_FAST_BOOK_H
#define LIMIT_ORDER_BOOK_FAST_BOOK_H

#include "Types.h"
#include "orderbook/Order.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace fast {
    struct Fill {
        OrderId buyOrderId;
        OrderId sellOrderId;
        Price price;
        Quantity quantity;

        bool operator==(const Fill &) const = default;
    };

    using Slot = std::uint32_t;
    inline constexpr Slot NIL = std::numeric_limits<Slot>::max();

    struct Level {
        Slot head = NIL;
        Slot tail = NIL;
        Quantity quantity = 0; // total resting quantity; makes the FOK check O(levels)
    };

    struct Node {
        OrderId id = 0;
        Price price = 0;
        Quantity quantity = 0;
        Slot prev = NIL;
        Slot next = NIL; // also the free-list link while the slot is unused
        Side side = Side::BUY;
    };

    // ── object pool ──────────────────────────────────────────────────────────

    class OrderPool {
    public:
        explicit OrderPool(const std::size_t capacity) : nodes(capacity) {
            if (capacity >= NIL) throw std::length_error("order pool capacity too large");
            for (std::size_t i = 0; i < capacity; ++i) {
                nodes[i].next = i + 1 < capacity ? static_cast<Slot>(i + 1) : NIL;
            }
            freeHead = capacity > 0 ? 0 : NIL;
        }

        Slot acquire() {
            if (freeHead == NIL) throw std::length_error("order pool exhausted");
            const Slot s = freeHead;
            freeHead = nodes[s].next;
            ++live;
            return s;
        }

        void release(const Slot s) {
            nodes[s].next = freeHead;
            freeHead = s;
            --live;
        }

        // Adds slots [capacity(), newCapacity) to the free list. Existing slots keep their indices.
        void grow(const std::size_t newCapacity) {
            if (newCapacity >= NIL) throw std::length_error("order pool capacity too large");
            const std::size_t old = nodes.size();
            nodes.resize(newCapacity);
            for (std::size_t i = old; i < newCapacity; ++i) {
                nodes[i].next = i + 1 < newCapacity ? static_cast<Slot>(i + 1) : freeHead;
            }
            if (newCapacity > old) freeHead = static_cast<Slot>(old);
        }

        Node &operator[](const Slot s) { return nodes[s]; }
        const Node &operator[](const Slot s) const { return nodes[s]; }
        [[nodiscard]] std::size_t size() const { return live; }
        [[nodiscard]] std::size_t capacity() const { return nodes.size(); }

    private:
        std::vector<Node> nodes;
        Slot freeHead = NIL;
        std::size_t live = 0;
    };

    // ── id index: OrderId -> Slot ────────────────────────────────────────────

    class IdIndex {
    public:
        explicit IdIndex(const std::size_t maxEntries) { allocate(maxEntries); }

        // Re-sizes the table for `maxEntries`, keeping every entry.
        void rehash(const std::size_t maxEntries) {
            const std::vector<OrderId> oldKeys = std::move(keys);
            const std::vector<Slot> oldSlots = std::move(slots);
            allocate(maxEntries);
            for (std::size_t i = 0; i < oldSlots.size(); ++i) {
                if (oldSlots[i] != NIL) insert(oldKeys[i], oldSlots[i]);
            }
        }

        [[nodiscard]] Slot find(const OrderId id) const {
            for (std::size_t i = home(id);; i = (i + 1) & mask) {
                if (slots[i] == NIL) return NIL;
                if (keys[i] == id) return slots[i];
            }
        }

        // The caller guarantees `id` is absent.
        void insert(const OrderId id, const Slot s) {
            std::size_t i = home(id);
            while (slots[i] != NIL) i = (i + 1) & mask;
            keys[i] = id;
            slots[i] = s;
        }

        void erase(const OrderId id) {
            std::size_t i = home(id);
            for (;; i = (i + 1) & mask) {
                if (slots[i] == NIL) return;
                if (keys[i] == id) break;
            }
            // Backward-shift deletion: pull later entries of the probe run
            // into the hole unless their home lies cyclically in (i, j].
            for (std::size_t j = i;;) {
                j = (j + 1) & mask;
                if (slots[j] == NIL) break;
                const std::size_t k = home(keys[j]);
                const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
                if (!stays) {
                    keys[i] = keys[j];
                    slots[i] = slots[j];
                    i = j;
                }
            }
            slots[i] = NIL;
        }

    private:
        void allocate(const std::size_t maxEntries) {
            std::size_t capacity = 16;
            while (capacity < maxEntries * 2) capacity <<= 1; // load factor <= 0.5
            keys.assign(capacity, 0);
            slots.assign(capacity, NIL);
            mask = capacity - 1;
        }

        [[nodiscard]] std::size_t home(OrderId x) const {
            // splitmix64 finaliser: sequential ids spread over the table
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return static_cast<std::size_t>(x) & mask;
        }

        std::vector<OrderId> keys;
        std::vector<Slot> slots;
        std::size_t mask = 0;
    };

    // ── price ladders ────────────────────────────────────────────────────────
    //
    // One side of the book. `Bid` makes higher prices better. forEach visits
    // non-empty levels from the best outwards until the callback returns false.

    template<bool Bid>
    class ArrayLadder {
    public:
        ArrayLadder(const Price minPrice, const Price maxPrice)
            : lo(minPrice),
              levels(static_cast<std::size_t>(maxPrice - minPrice + 1)),
              bits((levels.size() + 63) / 64, 0) {}

        Level *find(const Price p) {
            const std::size_t i = index(p);
            return test(i) ? &levels[i] : nullptr;
        }

        [[nodiscard]] const Level *find(const Price p) const {
            const std::size_t i = index(p);
            return test(i) ? &levels[i] : nullptr;
        }

        Level &getOrCreate(const Price p) {
            const std::size_t i = index(p);
            if (!test(i)) {
                bits[i >> 6] |= bit(i);
                if (best == NONE || (Bid ? i > best : i < best)) best = i;
            }
            return levels[i];
        }

        void erase(const Price p) {
            const std::size_t i = index(p);
            bits[i >> 6] &= ~bit(i);
            levels[i] = Level{};
            if (i == best) best = after(i);
        }

        [[nodiscard]] bool empty() const { return best == NONE; }
        [[nodiscard]] Price bestPrice() const { return lo + static_cast<Price>(best); }
        Level &bestLevel() { return levels[best]; }

        template<class F>
        void forEach(F &&f) const {
            for (std::size_t i = best; i != NONE; i = after(i)) {
                if (!f(lo + static_cast<Price>(i), levels[i])) return;
            }
        }

    private:
        static constexpr std::size_t NONE = std::numeric_limits<std::size_t>::max();

        [[nodiscard]] std::size_t index(const Price p) const { return static_cast<std::size_t>(p - lo); }
        static std::uint64_t bit(const std::size_t i) { return std::uint64_t{1} << (i & 63); }
        [[nodiscard]] bool test(const std::size_t i) const { return (bits[i >> 6] & bit(i)) != 0; }

        // The next non-empty level worse than i.
        [[nodiscard]] std::size_t after(const std::size_t i) const {
            if constexpr (Bid) {
                return i == 0 ? NONE : lastAtOrBelow(i - 1);
            } else {
                return firstAtOrAbove(i + 1);
            }
        }

        [[nodiscard]] std::size_t firstAtOrAbove(const std::size_t from) const {
            if (from >= levels.size()) return NONE;
            std::size_t w = from >> 6;
            std::uint64_t word = bits[w] & (~std::uint64_t{0} << (from & 63));
            for (;;) {
                if (word != 0) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
                if (++w == bits.size()) return NONE;
                word = bits[w];
            }
        }

        [[nodiscard]] std::size_t lastAtOrBelow(const std::size_t from) const {
            std::size_t w = from >> 6;
            std::uint64_t word = bits[w] & (~std::uint64_t{0} >> (63 - (from & 63)));
            for (;;) {
                if (word != 0) return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word));
                if (w == 0) return NONE;
                word = bits[--w];
            }
        }

        Price lo;
        std::vector<Level> levels;
        std::vector<std::uint64_t> bits; // bit i set <=> levels[i] is non-empty
        std::size_t best = NONE;
    };

    template<bool Bid>
    class MapLadder {
    public:
        MapLadder(Price, Price) {}

        Level *find(const Price p) {
            const auto it = levels.find(p);
            return it == levels.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const Level *find(const Price p) const {
            const auto it = levels.find(p);
            return it == levels.end() ? nullptr : &it->second;
        }

        Level &getOrCreate(const Price p) { return levels[p]; }
        void erase(const Price p) { levels.erase(p); }
        [[nodiscard]] bool empty() const { return levels.empty(); }
        [[nodiscard]] Price bestPrice() const { return levels.begin()->first; }
        Level &bestLevel() { return levels.begin()->second; }

        template<class F>
        void forEach(F &&f) const {
            for (const auto &[price, level]: levels) {
                if (!f(price, level)) return;
            }
        }

    private:
        // Best level first on both sides.
        std::map<Price, Level, std::conditional_t<Bid, std::greater<>, std::less<>>> levels;
    };

    // ── the book ─────────────────────────────────────────────────────────────

    template<template<bool> class Ladder>
    class BasicFastBook {
    public:
        BasicFastBook(const Price minPrice, const Price maxPrice, const std::size_t maxOrders,
                      const std::size_t fillCapacity = 1 << 16)
            : lo(minPrice), hi(maxPrice),
              bids(checkRange(minPrice, maxPrice), maxPrice), asks(minPrice, maxPrice),
              pool(maxOrders), index(maxOrders) {
            fills.reserve(fillCapacity);
        }

        // By default a full book throws std::length_error. With growth on,
        // the pool and id table double instead, so `maxOrders` is only a
        // starting size: small enough to stay in cache when the live book
        // is small. Allocates only while growing.
        void growWhenFull(const bool on = true) { growable = on; }

        // Matches `quantity` against the other side; a GTC limit remainder
        // rests. Returns the fills, valid until the next call. Throws
        // std::invalid_argument for a non-positive quantity or an id that is
        // already resting, std::out_of_range for a price outside the ladder.
        std::span<const Fill> submit(const OrderId id, const Side side, const std::optional<Price> price,
                                     Quantity quantity, const TimeInForce tif) {
            if (quantity <= 0) throw std::invalid_argument("quantity must be positive");
            if (price.has_value()) checkPrice(*price);
            if (index.find(id) != NIL) throw std::invalid_argument("order " + std::to_string(id) + " is already resting");
            fills.clear();

            if (side == Side::BUY) {
                match<true>(id, price, quantity, tif, asks);
                if (quantity > 0 && tif == TimeInForce::GTC && price.has_value()) rest(id, side, *price, quantity, bids);
            } else {
                match<false>(id, price, quantity, tif, bids);
                if (quantity > 0 && tif == TimeInForce::GTC && price.has_value()) rest(id, side, *price, quantity, asks);
            }
            return fills;
        }

        // False if `id` is not resting.
        bool cancel(const OrderId id) {
            const Slot s = index.find(id);
            if (s == NIL) return false;
            remove(s);
            return true;
        }

        // Same rules as ContinuousDoubleAuction::modify: a reduction at the
        // same price keeps priority, anything else re-enters (and may trade).
        std::span<const Fill> modify(const OrderId id, const Price newPrice, const Quantity newQuantity) {
            const Slot s = index.find(id);
            if (s == NIL) throw std::out_of_range("order " + std::to_string(id) + " is not resting");
            if (newQuantity <= 0) throw std::invalid_argument("modify needs a positive quantity");
            checkPrice(newPrice);
            fills.clear();

            Node &n = pool[s];
            if (newPrice == n.price && newQuantity <= n.quantity) {
                withSide(n.side, [&](auto &ladder) { ladder.find(n.price)->quantity -= n.quantity - newQuantity; });
                n.quantity = newQuantity;
                return fills;
            }
            const Side side = n.side;
            remove(s);
            return submit(id, side, newPrice, newQuantity, TimeInForce::GTC);
        }

        [[nodiscard]] std::optional<Price> bestBid() const {
            return bids.empty() ? std::nullopt : std::optional<Price>(bids.bestPrice());
        }

        [[nodiscard]] std::optional<Price> bestAsk() const {
            return asks.empty() ? std::nullopt : std::optional<Price>(asks.bestPrice());
        }

        [[nodiscard]] std::size_t restingOrders() const { return pool.size(); }

        [[nodiscard]] bool contains(const OrderId id) const { return index.find(id) != NIL; }

        // Visits one side's resting orders best level first, FIFO within a
        // level: f(price, id, quantity).
        template<class F>
        void forEachOrder(const Side side, F &&f) const {
            const auto visit = [&](const Price price, const Level &level) {
                for (Slot s = level.head; s != NIL; s = pool[s].next) f(price, pool[s].id, pool[s].quantity);
                return true;
            };
            if (side == Side::BUY) bids.forEach(visit);
            else asks.forEach(visit);
        }

        // Visits the orders resting at one price, FIFO: f(id, quantity).
        template<class F>
        void forEachOrderAt(const Side side, const Price price, F &&f) const {
            if (price < lo || price > hi) return;
            const Level *level = side == Side::BUY ? bids.find(price) : asks.find(price);
            if (level == nullptr) return;
            for (Slot s = level->head; s != NIL; s = pool[s].next) f(pool[s].id, pool[s].quantity);
        }

        // ── passive mode: rebuilding a book from an exchange feed ────────────
        //
        // A feed reports what the exchange already did, so these never match:
        // restPassive() queues an order at the back of its level even if it
        // would cross, and reduce() applies an execution or a partial cancel
        // to the order the message names.

        // Throws std::invalid_argument for a non-positive quantity or an id
        // that is already resting, std::out_of_range for a price outside the
        // ladder.
        void restPassive(const OrderId id, const Side side, const Price price, const Quantity quantity) {
            if (quantity <= 0) throw std::invalid_argument("quantity must be positive");
            checkPrice(price);
            if (index.find(id) != NIL) throw std::invalid_argument("order " + std::to_string(id) + " is already resting");
            if (side == Side::BUY) rest(id, side, price, quantity, bids);
            else rest(id, side, price, quantity, asks);
        }

        // Takes `quantity` off a resting order, keeping its place in the
        // queue; at zero the order leaves the book. False if `id` is not
        // resting. Throws std::invalid_argument if `quantity` is not in
        // [1, resting quantity].
        bool reduce(const OrderId id, const Quantity quantity) {
            const Slot s = index.find(id);
            if (s == NIL) return false;
            Node &n = pool[s];
            if (quantity <= 0 || quantity > n.quantity) {
                throw std::invalid_argument("cannot reduce order " + std::to_string(id) + " by " +
                                            std::to_string(quantity));
            }
            if (quantity == n.quantity) {
                remove(s);
                return true;
            }
            withSide(n.side, [&](auto &ladder) { ladder.find(n.price)->quantity -= quantity; });
            n.quantity -= quantity;
            return true;
        }

        struct RestingInfo {
            Side side;
            Price price;
            Quantity quantity;
        };

        [[nodiscard]] std::optional<RestingInfo> find(const OrderId id) const {
            const Slot s = index.find(id);
            if (s == NIL) return std::nullopt;
            const Node &n = pool[s];
            return RestingInfo{n.side, n.price, n.quantity};
        }

        // Total resting quantity at one price, 0 for an empty level or a
        // price outside the ladder.
        [[nodiscard]] Quantity levelQuantity(const Side side, const Price price) const {
            if (price < lo || price > hi) return 0;
            const Level *level = side == Side::BUY ? bids.find(price) : asks.find(price);
            return level != nullptr ? level->quantity : 0;
        }

        // Visits one side's non-empty levels best first until f(price,
        // quantity) returns false.
        template<class F>
        void forEachLevel(const Side side, F &&f) const {
            const auto visit = [&](const Price price, const Level &level) { return f(price, level.quantity); };
            if (side == Side::BUY) bids.forEach(visit);
            else asks.forEach(visit);
        }

    private:
        static Price checkRange(const Price minPrice, const Price maxPrice) {
            if (minPrice > maxPrice) throw std::invalid_argument("minPrice > maxPrice");
            return minPrice;
        }

        void checkPrice(const Price p) const {
            if (p < lo || p > hi) throw std::out_of_range("price " + std::to_string(p) + " outside the ladder");
        }

        template<class F>
        decltype(auto) withSide(const Side side, F &&f) {
            return side == Side::BUY ? f(bids) : f(asks);
        }

        template<bool Buy, class Opposite>
        void match(const OrderId id, const std::optional<Price> limit, Quantity &quantity,
                   const TimeInForce tif, Opposite &opp) {
            const auto crosses = [&](const Price levelPrice) {
                if (!limit.has_value()) return true; // a market order takes any price
                return Buy ? levelPrice <= *limit : levelPrice >= *limit;
            };

            if (tif == TimeInForce::FOK) {
                Quantity available = 0;
                opp.forEach([&](const Price p, const Level &level) {
                    if (!crosses(p)) return false;
                    available += level.quantity;
                    return available < quantity;
                });
                if (available < quantity) {
                    quantity = 0; // killed: nothing trades, nothing rests
                    return;
                }
            }

            while (quantity > 0 && !opp.empty()) {
                const Price p = opp.bestPrice();
                if (!crosses(p)) break;
                Level &level = opp.bestLevel();
                while (quantity > 0 && level.head != NIL) {
                    const Slot s = level.head;
                    Node &maker = pool[s];
                    const Quantity q = std::min(quantity, maker.quantity);
                    fills.push_back(Buy ? Fill{id, maker.id, p, q} : Fill{maker.id, id, p, q});
                    quantity -= q;
                    maker.quantity -= q;
                    level.quantity -= q;
                    if (maker.quantity == 0) {
                        level.head = maker.next;
                        if (level.head != NIL) pool[level.head].prev = NIL;
                        else level.tail = NIL;
                        index.erase(maker.id);
                        pool.release(s);
                    }
                }
                if (level.head == NIL) opp.erase(p);
            }
        }

        template<class Own>
        void rest(const OrderId id, const Side side, const Price price, const Quantity quantity, Own &own) {
            if (growable && pool.size() == pool.capacity()) {
                const std::size_t capacity = std::max<std::size_t>(16, pool.capacity() * 2);
                pool.grow(capacity);
                index.rehash(capacity);
            }
            const Slot s = pool.acquire();
            Level &level = own.getOrCreate(price);
            pool[s] = Node{id, price, quantity, level.tail, NIL, side};
            if (level.tail != NIL) pool[level.tail].next = s;
            else level.head = s;
            level.tail = s;
            level.quantity += quantity;
            index.insert(id, s);
        }

        void remove(const Slot s) {
            Node &n = pool[s];
            withSide(n.side, [&](auto &ladder) {
                Level &level = *ladder.find(n.price);
                if (n.prev != NIL) pool[n.prev].next = n.next;
                else level.head = n.next;
                if (n.next != NIL) pool[n.next].prev = n.prev;
                else level.tail = n.prev;
                level.quantity -= n.quantity;
                if (level.head == NIL) ladder.erase(n.price);
            });
            index.erase(n.id);
            pool.release(s);
        }

        Price lo, hi;
        Ladder<true> bids;
        Ladder<false> asks;
        OrderPool pool;
        IdIndex index;
        std::vector<Fill> fills;
        bool growable = false;
    };

    // Flat array + bitmap ladder: the zero-allocation book.
    using FastBook = BasicFastBook<ArrayLadder>;
    // Same pool, intrusive lists and id index, but std::map levels: isolates
    // what the flat ladder alone is worth.
    using MapBook = BasicFastBook<MapLadder>;
}

#endif //LIMIT_ORDER_BOOK_FAST_BOOK_H
