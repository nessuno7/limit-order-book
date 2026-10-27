//
// Owns Orders so that everything else can hold non-owning Order*.
//
// Python keeps orders alive by reference counting: the book's index, a call
// auction's pending list and the caller's own variable all point at one
// object, and mutations through any of them are seen by all. Raw pointers into
// a deque reproduce that with no control block and no per-order allocation --
// deque never invalidates references to existing elements on push_back.
//
// Nothing is reclaimed until the arena dies. That is deliberate for now: a
// matching engine wants a slab plus a free list, not per-order delete.
//

#ifndef LIMIT_ORDER_BOOK_ORDER_ARENA_H
#define LIMIT_ORDER_BOOK_ORDER_ARENA_H

#include "Order.h"

#include <deque>
#include <utility>

class OrderArena {
public:
    template<typename... Args>
    Order *create(Args &&... args) {
        storage.emplace_back(std::forward<Args>(args)...);
        return &storage.back();
    }

    // The `copy.copy(o)` of the Python rules: same order_id and seq,
    // independent quantity.
    Order *copy(const Order &other) {
        storage.push_back(other);
        return &storage.back();
    }

    [[nodiscard]] std::size_t size() const { return storage.size(); }

private:
    std::deque<Order> storage;
};

#endif //LIMIT_ORDER_BOOK_ORDER_ARENA_H
