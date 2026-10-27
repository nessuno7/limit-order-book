//
// Port of python_engine/call_auction/fok.py
//
// Fill-or-kill feasibility checks shared by the call-auction pricing rules.
// In a batch, a FOK's opposing volume may itself come from another FOK that
// can only fill if *this* order fills it; fokWalk resolves that by recursing
// into the opposing FOK.
//

#ifndef LIMIT_ORDER_BOOK_FOK_H
#define LIMIT_ORDER_BOOK_FOK_H

#include "orderbook/Order.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fok {
    using IgnoreSet = std::unordered_set<OrderId>;
    using PartialFills = std::unordered_map<OrderId, Quantity>;

    // True if `order` can fill completely from `oppSortedList`. `sortedList`
    // is the rest of `order`'s own side, which an opposing FOK may draw on.
    bool walk(const Order *order,
              const std::vector<Order *> &sortedList,
              const std::vector<Order *> &oppSortedList,
              IgnoreSet &ordersToIgnore,
              PartialFills &partiallyFilledOrders,
              Quantity startingVol);

    // Drops FOKs that cannot fill from the front of the queue. Returns the
    // first order that can trade (any non-FOK, or a FOK that passes the walk),
    // or nullptr once `sortedList` is exhausted.
    Order *helper(Order *order,
                  std::vector<Order *> &sortedList,
                  const std::vector<Order *> &oppSortedList);

    // Python's list.pop(0): same result, same linear cost.
    Order *popFront(std::vector<Order *> &v);
}

#endif //LIMIT_ORDER_BOOK_FOK_H
