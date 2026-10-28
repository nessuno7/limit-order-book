//
// Port of python_engine/call_auction/rules/uniform.py
//

#include "Uniform.h"

#include "call_auction/FillOrKill.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

using fok::popFront;

namespace {
    Quantity totalQuantity(const std::vector<Order *> &orders) {
        Quantity total = 0;
        for (const Order *o: orders) total += o->quantity;
        return total;
    }

    // One candidate price's simulation.
    struct LevelResult {
        std::vector<Trade> trades;
        std::vector<Order *> unfilled;  // copies
        std::vector<Order *> simulated; // every copy, for write-back
        Quantity surplus = 0;
    };
}

void Uniform::populateLevels(const std::vector<Order *> &orders,
                             std::map<Price, std::vector<Order *>> &levels) {
    for (Order *order: orders) {
        if (!order->price.has_value()) continue;
        levels[*order->price].push_back(order);
    }
}

std::vector<Order *> Uniform::createOrderList(const Price priceLevel,
                                              const std::map<Price, std::vector<Order *>> &orders,
                                              const Side side,
                                              const std::vector<Order *> &marketOrders,
                                              OrderArena &pool) {
    std::vector<Order *> r;

    for (const Order *o: marketOrders) r.push_back(pool.copy(*o));

    if (side == Side::BUY) {
        // sorted(orders.keys(), reverse=True)
        for (auto it = orders.rbegin(); it != orders.rend(); ++it) {
            if (it->first >= priceLevel) {
                for (const Order *o: it->second) r.push_back(pool.copy(*o));
            }
        }
    } else {
        for (const auto &[price, level]: orders) { // ascending
            if (price <= priceLevel) {
                for (const Order *o: level) r.push_back(pool.copy(*o));
            }
        }
    }

    std::stable_sort(r.begin(), r.end(), [](const Order *a, const Order *b) {
        return a->priority() < b->priority();
    });
    return r;
}

Price Uniform::chooseTieBreak(const std::vector<Price> &tiedPrices) {
    return *std::max_element(tiedPrices.begin(), tiedPrices.end());
}

ClearResult Uniform::clear(const std::vector<Order *> &buys,
                           const std::vector<Order *> &sells) {
    OrderArena pool; // the copies live only as long as this call
    std::map<Price, std::vector<Order *>> buyLevels;
    std::map<Price, std::vector<Order *>> sellLevels;
    std::map<Price, Quantity> levelVolume;
    std::map<Price, LevelResult> levelResults;

    std::vector<Order *> mktBuys;
    std::vector<Order *> mktSells;
    for (Order *o: buys) {
        if (o->isMarket()) mktBuys.push_back(o);
    }
    for (Order *o: sells) {
        if (o->isMarket()) mktSells.push_back(o);
    }
    populateLevels(buys, buyLevels);
    populateLevels(sells, sellLevels);

    // Market orders add no candidate price of their own: a batch of only
    // market orders has no price discovery and does not trade.
    std::set<Price> candidatePrices;
    for (const auto &[price, level]: buyLevels) candidatePrices.insert(price);
    for (const auto &[price, level]: sellLevels) candidatePrices.insert(price);

    for (const Price p: candidatePrices) { // ascending
        std::vector<Order *> sortedSells = createOrderList(p, sellLevels, Side::SELL, mktSells, pool);
        std::vector<Order *> sortedBuys = createOrderList(p, buyLevels, Side::BUY, mktBuys, pool);

        if (sortedBuys.empty() || sortedSells.empty()) continue;

        LevelResult level;
        level.simulated = sortedBuys;
        level.simulated.insert(level.simulated.end(), sortedSells.begin(), sortedSells.end());
        level.surplus = std::abs(totalQuantity(sortedBuys) - totalQuantity(sortedSells));

        Order *buyOrder = popFront(sortedBuys);
        Order *sellOrder = popFront(sortedSells);

        while (buyOrder != nullptr && sellOrder != nullptr) {
            {
                std::vector<Order *> opp{sellOrder};
                opp.insert(opp.end(), sortedSells.begin(), sortedSells.end());
                buyOrder = fok::helper(buyOrder, sortedBuys, opp);
            }
            if (buyOrder == nullptr) break;
            {
                std::vector<Order *> opp{buyOrder};
                opp.insert(opp.end(), sortedBuys.begin(), sortedBuys.end());
                sellOrder = fok::helper(sellOrder, sortedSells, opp);
            }
            if (sellOrder == nullptr) break;

            // Every order here is eligible at p (buy >= p >= sell), so all
            // pairs cross and every trade prints at p.
            const OrderId bId = buyOrder->orderId;
            const OrderId sId = sellOrder->orderId;

            Quantity restingQuantity;
            if (buyOrder->quantity == sellOrder->quantity) {
                restingQuantity = sellOrder->quantity;
                buyOrder->quantity = sellOrder->quantity = 0;
                buyOrder = sortedBuys.empty() ? nullptr : popFront(sortedBuys);
                sellOrder = sortedSells.empty() ? nullptr : popFront(sortedSells);
            } else if (buyOrder->quantity < sellOrder->quantity) {
                restingQuantity = buyOrder->quantity;
                sellOrder->quantity -= restingQuantity;
                buyOrder->quantity = 0;
                buyOrder = sortedBuys.empty() ? nullptr : popFront(sortedBuys);
            } else {
                restingQuantity = sellOrder->quantity;
                buyOrder->quantity -= restingQuantity;
                sellOrder->quantity = 0;
                sellOrder = sortedSells.empty() ? nullptr : popFront(sortedSells);
            }

            // Only written when a trade actually prints, so a price that
            // crosses nothing never becomes a candidate for p*.
            levelVolume[p] += restingQuantity;
            level.trades.emplace_back(p, restingQuantity, bId, sId);
        }

        for (Order *o: sortedSells) {
            if (o->tif == TimeInForce::GTC) level.unfilled.push_back(o);
        }
        for (Order *o: sortedBuys) {
            if (o->tif == TimeInForce::GTC) level.unfilled.push_back(o);
        }
        if (buyOrder != nullptr && buyOrder->quantity > 0 && buyOrder->tif == TimeInForce::GTC) {
            level.unfilled.push_back(buyOrder);
        }
        if (sellOrder != nullptr && sellOrder->quantity > 0 && sellOrder->tif == TimeInForce::GTC) {
            level.unfilled.push_back(sellOrder);
        }

        levelResults[p] = std::move(level);
    }

    if (levelVolume.empty()) {
        std::vector<Order *> rolled;
        for (Order *o: buys) {
            if (o->tif == TimeInForce::GTC) rolled.push_back(o);
        }
        for (Order *o: sells) {
            if (o->tif == TimeInForce::GTC) rolled.push_back(o);
        }
        return ClearResult{{}, std::move(rolled)};
    }

    Quantity maxVal = std::numeric_limits<Quantity>::min();
    for (const auto &[price, volume]: levelVolume) maxVal = std::max(maxVal, volume);

    std::vector<Price> maxKeys; // ascending
    for (const auto &[price, volume]: levelVolume) {
        if (volume == maxVal) maxKeys.push_back(price);
    }

    Price bestP;
    if (maxKeys.size() > 1) {
        Quantity minSurplus = std::numeric_limits<Quantity>::max();
        for (const Price k: maxKeys) minSurplus = std::min(minSurplus, levelResults.at(k).surplus);

        std::vector<Price> tradeKeys;
        for (const Price k: maxKeys) {
            if (levelResults.at(k).surplus == minSurplus) tradeKeys.push_back(k);
        }
        bestP = chooseTieBreak(tradeKeys);
    } else {
        bestP = maxKeys.front();
    }

    LevelResult &winner = levelResults.at(bestP);

    // Write the winning simulation back to the caller's orders, so what rolls
    // into the next batch is the original, not a copy that dies with `pool`.
    std::unordered_map<OrderId, Order *> originals;
    for (Order *o: buys) originals[o->orderId] = o;
    for (Order *o: sells) originals[o->orderId] = o;
    for (const Order *clone: winner.simulated) originals.at(clone->orderId)->quantity = clone->quantity;

    std::vector<Order *> unfilled;
    for (const Order *clone: winner.unfilled) unfilled.push_back(originals.at(clone->orderId));

    // Orders that weren't eligible at bestP never entered the simulation;
    // return any GTC ones so they roll into the next batch.
    const auto eligible = [bestP](const Order *o) {
        if (!o->price.has_value()) return true;
        return o->side == Side::BUY ? (*o->price >= bestP) : (*o->price <= bestP);
    };
    for (Order *o: buys) {
        if (!eligible(o) && o->tif == TimeInForce::GTC) unfilled.push_back(o);
    }
    for (Order *o: sells) {
        if (!eligible(o) && o->tif == TimeInForce::GTC) unfilled.push_back(o);
    }

    return ClearResult{std::move(winner.trades), std::move(unfilled)};
}
