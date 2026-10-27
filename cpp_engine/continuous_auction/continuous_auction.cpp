//
// Port of python_engine/continuous_auction/continuous_auction.py
//

#include "continuous_auction.h"

#include <stdexcept>
#include <string>

bool ContinuousDoubleAuction::gtcHelper(const Price bestPrice,
                                        Order *ctr,
                                        Order *order,
                                        std::vector<Trade> &newTrades) {
    const Quantity quantity = order->quantity <= ctr->quantity ? order->quantity : ctr->quantity;

    if (order->side == Side::BUY) {
        newTrades.emplace_back(bestPrice, quantity, order->orderId, ctr->orderId);
    } else { // sell side: just flip the buy and sell counterparties
        newTrades.emplace_back(bestPrice, quantity, ctr->orderId, order->orderId);
    }

    order->quantity = order->quantity - quantity;
    ctr->quantity = ctr->quantity - quantity;

    if (ctr->quantity == 0) { // resting order exhausted, take it off the book
        book.cancel(ctr->orderId);
    }

    return order->quantity != 0; // false once the order has been satisfied
}

bool ContinuousDoubleAuction::fokWalk(const Order *order) {
    Quantity filledQty = 0;

    // A market FOK has no limit: every resting order on the other side counts.
    if (order->side == Side::BUY) {
        for (const auto &[price, queue]: book.asks) { // ascending, as sorted(asks.items())
            if (order->price.has_value() && price > *order->price) break;
            for (const OrderId uid: queue) {
                filledQty += book.index.at(uid)->quantity;
                if (filledQty >= order->quantity) return true;
            }
        }
    } else { // SELL: descending, as sorted(bids.items(), reverse=True)
        for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it) {
            if (order->price.has_value() && it->first < *order->price) break;
            for (const OrderId uid: it->second) {
                filledQty += book.index.at(uid)->quantity;
                if (filledQty >= order->quantity) return true;
            }
        }
    }

    return false;
}

void ContinuousDoubleAuction::rest(Order *order) {
    // Only a GTC limit order rests. A market order has no price level to rest
    // on, so its unfilled remainder is cancelled like an IOC's.
    if (order->tif == TimeInForce::GTC && order->price.has_value()) book.add(order);
}

std::vector<Trade> ContinuousDoubleAuction::submit(Order *order) {
    if (book.index.count(order->orderId) != 0) {
        throw std::invalid_argument("order " + std::to_string(order->orderId) + " is already resting in the book");
    }
    if (order->quantity <= 0) { // e.g. resubmitting an order that already filled
        throw std::invalid_argument("order " + std::to_string(order->orderId) + " has no quantity left to fill");
    }
    std::vector<Trade> newTrades;

    // Precedence of the order: a market order keeps walking the book, a limit
    // order stops at its price. Exactly one of these is live at a time.
    bool marketContinue = !order->price.has_value();
    bool limitContinue = !marketContinue;

    if (order->tif == TimeInForce::FOK) {
        if (!fokWalk(order)) {
            return {}; // FOK gets cancelled, never rests
        }
    }

    if (order->side == Side::BUY) {
        while (marketContinue || limitContinue) {
            const auto best = book.bestAsk();
            Order *sellCtr = nullptr;
            if (best.has_value()) {
                const auto it = book.index.find(best->second);
                if (it != book.index.end()) sellCtr = it->second;
            }

            if (!best.has_value()) { // no seller, so rest the order
                rest(order);
                break;
            }
            if (!order->price.has_value()) { // market order takes the best price
                marketContinue = gtcHelper(best->first, sellCtr, order, newTrades);
            } else if (best->first <= *order->price) {
                // someone is selling for no more than we are bidding, take it
                limitContinue = gtcHelper(best->first, sellCtr, order, newTrades);
            } else { // limit order and best ask is above our bid
                rest(order);
                // IOC and FOK remainders are simply dropped
                break;
            }
        }
    } else { // Side::SELL
        while (marketContinue || limitContinue) {
            const auto best = book.bestBid();
            Order *buyCtr = nullptr;
            if (best.has_value()) {
                const auto it = book.index.find(best->second);
                if (it != book.index.end()) buyCtr = it->second;
            }

            if (!best.has_value()) { // no buyer, so rest the order
                rest(order);
                break;
            }
            if (!order->price.has_value()) { // market order takes the best price
                marketContinue = gtcHelper(best->first, buyCtr, order, newTrades);
            } else if (best->first >= *order->price) {
                // someone is bidding at least what we are asking, take it
                limitContinue = gtcHelper(best->first, buyCtr, order, newTrades);
            } else { // limit order and best bid is below our ask
                rest(order);
                break;
            }
        }
    }

    this->trades.insert(this->trades.end(), newTrades.begin(), newTrades.end());
    return newTrades;
}

std::vector<Trade> ContinuousDoubleAuction::modify(const OrderId orderId,
                                                   const Price newPrice,
                                                   const Quantity newQuantity,
                                                   std::optional<Datetime> timestamp) {
    Order *order = book.index.at(orderId); // throws if not resting
    if (newQuantity <= 0) {
        throw std::invalid_argument("modify needs a positive quantity, got " + std::to_string(newQuantity)
                                    + "; use cancel to remove an order");
    }
    if (newPrice == *order->price && newQuantity <= order->quantity) {
        order->quantity = newQuantity; // a reduction keeps priority
        return {};
    }
    book.cancel(orderId);
    order->price = newPrice;
    order->quantity = newQuantity;
    order->renew(timestamp);
    return submit(order);
}
