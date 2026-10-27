//
// Two small allocation-light containers for the backtester's hot path, where
// std::unordered_map and std::deque spent most of their time in the heap.
//
//   IdMap<V>: OrderId -> V, open addressing with linear probing and
//     backward-shift deletion (as FastBook's IdIndex), doubling at half full.
//   Fifo<T>: a queue on one vector; popped slots are reclaimed when the
//     queue empties or the dead prefix outgrows the live part.
//

#ifndef LIMIT_ORDER_BOOK_BACKTEST_FLAT_CONTAINERS_H
#define LIMIT_ORDER_BOOK_BACKTEST_FLAT_CONTAINERS_H

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bt {
    template<class V>
    class IdMap {
    public:
        explicit IdMap(const std::size_t initialCapacity = 1'024) { allocate(initialCapacity); }

        // The value for `id`, or `absent` if it has none.
        [[nodiscard]] V get(const OrderId id, const V absent = V{}) const {
            for (std::size_t i = home(id);; i = (i + 1) & mask) {
                if (!used[i]) return absent;
                if (keys[i] == id) return values[i];
            }
        }

        void set(const OrderId id, const V value) {
            std::size_t i = home(id);
            for (; used[i]; i = (i + 1) & mask) {
                if (keys[i] == id) {
                    values[i] = value;
                    return;
                }
            }
            if ((count + 1) * 2 > keys.size()) {
                grow();
                set(id, value);
                return;
            }
            used[i] = 1;
            keys[i] = id;
            values[i] = value;
            ++count;
        }

        void erase(const OrderId id) {
            std::size_t i = home(id);
            for (;; i = (i + 1) & mask) {
                if (!used[i]) return;
                if (keys[i] == id) break;
            }
            for (std::size_t j = i;;) {
                j = (j + 1) & mask;
                if (!used[j]) break;
                const std::size_t k = home(keys[j]);
                const bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
                if (!stays) {
                    keys[i] = keys[j];
                    values[i] = values[j];
                    i = j;
                }
            }
            used[i] = 0;
            --count;
        }

        [[nodiscard]] std::size_t size() const { return count; }

    private:
        void allocate(const std::size_t entries) {
            std::size_t capacity = 16;
            while (capacity < entries * 2) capacity <<= 1;
            keys.assign(capacity, 0);
            values.assign(capacity, V{});
            used.assign(capacity, 0);
            mask = capacity - 1;
            count = 0;
        }

        void grow() {
            const std::vector<OrderId> oldKeys = std::move(keys);
            const std::vector<V> oldValues = std::move(values);
            const std::vector<std::uint8_t> oldUsed = std::move(used);
            allocate(oldKeys.size());
            for (std::size_t i = 0; i < oldKeys.size(); ++i) {
                if (oldUsed[i]) set(oldKeys[i], oldValues[i]);
            }
        }

        [[nodiscard]] std::size_t home(OrderId x) const {
            x ^= x >> 33; // splitmix64 finaliser, as IdIndex
            x *= 0xff51afd7ed558ccdULL;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ULL;
            x ^= x >> 33;
            return static_cast<std::size_t>(x) & mask;
        }

        std::vector<OrderId> keys;
        std::vector<V> values;
        std::vector<std::uint8_t> used;
        std::size_t mask = 0;
        std::size_t count = 0;
    };

    template<class T>
    class Fifo {
    public:
        [[nodiscard]] bool empty() const { return head == items.size(); }
        [[nodiscard]] const T &front() const { return items[head]; }
        void push_back(const T &x) { items.push_back(x); }

        void pop_front() {
            if (++head == items.size()) {
                items.clear();
                head = 0;
            } else if (head >= 1'024 && head * 2 >= items.size()) {
                items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(head));
                head = 0;
            }
        }

    private:
        std::vector<T> items;
        std::size_t head = 0;
    };
}

#endif //LIMIT_ORDER_BOOK_BACKTEST_FLAT_CONTAINERS_H
