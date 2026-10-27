//
// Port of python_engine/call_auction/fok.py
//

#include "fok.h"

namespace {
    // A market order on either side always crosses.
    bool crosses(const Order *o1, const Order *o2) {
        if (!o1->price.has_value() || !o2->price.has_value()) return true;
        return o1->side == Side::BUY
                   ? (*o1->price >= *o2->price)
                   : (*o2->price >= *o1->price);
    }
}

namespace fok {
    Order *popFront(std::vector<Order *> &v) {
        Order *front = v.front();
        v.erase(v.begin());
        return front;
    }

    bool walk(const Order *order,
              const std::vector<Order *> &sortedList,
              const std::vector<Order *> &oppSortedList,
              IgnoreSet &ordersToIgnore,
              PartialFills &partiallyFilledOrders,
              const Quantity startingVol) {
        Quantity volume = startingVol;

        for (const Order *oppOrder: oppSortedList) {
            if (ordersToIgnore.count(oppOrder->orderId) != 0) continue;

            if (oppOrder->tif == TimeInForce::FOK && oppOrder->quantity > order->quantity - volume) {
                // The opposing FOK is bigger than what we still need, so it
                // can only fill by also taking volume from our side. Try that
                // speculatively on copies: a failed attempt must not leave
                // orders marked as consumed for the next opposing FOK we try.
                IgnoreSet trialIgnore = ordersToIgnore;
                trialIgnore.insert(order->orderId);
                PartialFills trialPartial = partiallyFilledOrders;
                if (walk(oppOrder, oppSortedList, sortedList, trialIgnore, trialPartial,
                         order->quantity - volume)) {
                    return true;
                }
                ordersToIgnore.insert(oppOrder->orderId); // cannot be filled, skip it from now on
                continue;
            }

            // Beyond the limit, and the list is in priority order.
            if (!crosses(order, oppOrder)) return false;

            const auto partial = partiallyFilledOrders.find(oppOrder->orderId);
            const Quantity available = partial != partiallyFilledOrders.end() ? partial->second : oppOrder->quantity;
            const Quantity needed = order->quantity - volume;
            if (needed < available) {
                partiallyFilledOrders[oppOrder->orderId] = available - needed; // leftover volume
            } else {
                if (partial != partiallyFilledOrders.end()) partiallyFilledOrders.erase(partial);
                ordersToIgnore.insert(oppOrder->orderId);
            }
            volume += available;
            if (volume >= order->quantity) return true;
        }
        return false;
    }

    Order *helper(Order *order,
                  std::vector<Order *> &sortedList,
                  const std::vector<Order *> &oppSortedList) {
        while (order != nullptr && order->tif == TimeInForce::FOK) {
            // Python's `=None` defaults: a fresh pair of maps on every entry.
            IgnoreSet ordersToIgnore;
            PartialFills partiallyFilledOrders;
            if (walk(order, sortedList, oppSortedList, ordersToIgnore, partiallyFilledOrders, 0)) {
                return order;
            }
            order = sortedList.empty() ? nullptr : popFront(sortedList);
        }
        return order;
    }
}
