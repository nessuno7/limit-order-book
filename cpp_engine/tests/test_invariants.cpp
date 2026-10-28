//
// Port of tests/test_invariants.py: many small seeded scenarios, checked
// against properties every correct engine must keep rather than specific
// outputs. On failure the message names the seed.
//

#include "helpers.h"
#include "test_framework.h"

#include "call_auction/rules/Discriminatory.h"
#include "call_auction/rules/Uniform.h"
#include "continuous_auction/ContinuousDoubleAuction.h"

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
    constexpr int CALL_SEEDS = 2000;
    constexpr int CONTINUOUS_SEEDS = 500;

    int randInt(std::mt19937 &rng, int lo, int hi) { // inclusive, like random.randint
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    }

    Order *randomOrder(std::mt19937 &rng, Orders &o, Side side, bool allowMarket) {
        const bool market = allowMarket && randInt(rng, 0, 99) < 15;
        const auto tif = static_cast<TimeInForce>(randInt(rng, 0, 2));
        const Quantity qty = randInt(rng, 1, 10);
        const int t = randInt(rng, 0, 5); // coarse timestamps on purpose: ties must be handled too
        if (market) return o.make(side, std::nullopt, qty, tif, t);
        return o.make(side, randInt(rng, 95, 105), qty, tif, t);
    }

    struct Batch {
        std::vector<Order *> buys;
        std::vector<Order *> sells;
        std::unordered_map<OrderId, Quantity> originalQty;
    };

    Batch randomBatch(std::mt19937 &rng, Orders &o, bool allowMarket) {
        Batch b;
        const int nb = randInt(rng, 0, 6);
        const int ns = randInt(rng, 0, 6);
        for (int i = 0; i < nb; ++i) b.buys.push_back(randomOrder(rng, o, Side::BUY, allowMarket));
        for (int i = 0; i < ns; ++i) b.sells.push_back(randomOrder(rng, o, Side::SELL, allowMarket));
        for (const Order *x: b.buys) b.originalQty[x->orderId] = x->quantity;
        for (const Order *x: b.sells) b.originalQty[x->orderId] = x->quantity;
        return b;
    }

    std::unordered_map<OrderId, Quantity> tradedByOrder(const std::vector<Trade> &trades) {
        std::unordered_map<OrderId, Quantity> traded;
        for (const Trade &t: trades) {
            traded[t.buyOrderId] += t.quantity;
            traded[t.sellOrderId] += t.quantity;
        }
        return traded;
    }

#define CHECK_SEED(cond, seed) \
    do { if (!(cond)) testing::fail(__FILE__, __LINE__, std::string(#cond) + " failed for seed " + std::to_string(seed)); } while (0)

    void checkBatch(const Batch &b, const ClearResult &r, int seed) {
        std::unordered_map<OrderId, const Order *> byId;
        for (const Order *x: b.buys) byId[x->orderId] = x;
        for (const Order *x: b.sells) byId[x->orderId] = x;

        std::unordered_map<OrderId, Quantity> remaining;
        for (const Order *x: r.unfilled) {
            CHECK_SEED(remaining.count(x->orderId) == 0, seed); // returned twice
            CHECK_SEED(byId.count(x->orderId) == 1 && byId.at(x->orderId) == x, seed); // caller's own object
            remaining[x->orderId] = x->quantity;
        }

        for (const Trade &t: r.trades) {
            CHECK_SEED(t.quantity > 0, seed);
            const Order *buy = byId.at(t.buyOrderId);
            const Order *sell = byId.at(t.sellOrderId);
            CHECK_SEED(buy->side == Side::BUY && sell->side == Side::SELL, seed);
            CHECK_SEED(!buy->price.has_value() || t.price <= *buy->price, seed);
            CHECK_SEED(!sell->price.has_value() || t.price >= *sell->price, seed);
        }

        auto traded = tradedByOrder(r.trades);
        for (const auto &[id, o]: byId) {
            const Quantity q0 = b.originalQty.at(id);
            const Quantity done = traded[id];
            CHECK_SEED(done <= q0, seed);
            if (o->tif == TimeInForce::FOK) CHECK_SEED(done == 0 || done == q0, seed);
            if (o->tif != TimeInForce::GTC) {
                CHECK_SEED(remaining.count(id) == 0, seed);
            } else {
                const auto it = remaining.find(id);
                CHECK_SEED(done + (it == remaining.end() ? 0 : it->second) == q0, seed);
            }
        }
    }

    Quantity tradeableVolume(const Batch &b, Price p) {
        Quantity demand = 0;
        Quantity supply = 0;
        for (const Order *x: b.buys) {
            if (!x->price.has_value() || *x->price >= p) demand += b.originalQty.at(x->orderId);
        }
        for (const Order *x: b.sells) {
            if (!x->price.has_value() || *x->price <= p) supply += b.originalQty.at(x->orderId);
        }
        return std::min(demand, supply);
    }
}

TEST(invariants_uniform) {
    for (int seed = 0; seed < CALL_SEEDS; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        Orders o;
        Batch b = randomBatch(rng, o, true);
        const ClearResult r = Uniform().clear(b.buys, b.sells);
        checkBatch(b, r, seed);

        std::set<Price> prices;
        for (const Trade &t: r.trades) prices.insert(t.price);
        CHECK_SEED(prices.size() <= 1, seed);

        // Without FOKs, p* must achieve the best tradeable volume.
        bool anyFok = false;
        std::set<Price> limitPrices;
        for (const auto *side: {&b.buys, &b.sells}) {
            for (const Order *x: *side) {
                anyFok = anyFok || x->tif == TimeInForce::FOK;
                if (x->price.has_value()) limitPrices.insert(*x->price);
            }
        }
        if (!anyFok) {
            Quantity best = 0;
            for (const Price p: limitPrices) best = std::max(best, tradeableVolume(b, p));
            CHECK_SEED(totalQty(r.trades) == best, seed);
        }
    }
}

TEST(invariants_discriminatory) {
    for (int seed = 0; seed < CALL_SEEDS; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        Orders o;
        Batch b = randomBatch(rng, o, false);
        checkBatch(b, Discriminatory().clear(b.buys, b.sells), seed);
    }
}

TEST(invariants_continuous) {
    for (int seed = 0; seed < CONTINUOUS_SEEDS; ++seed) {
        std::mt19937 rng(static_cast<unsigned>(seed));
        Orders o;
        ContinuousDoubleAuction e;
        std::unordered_map<OrderId, Quantity> originalQty;

        for (int step = 0; step < 40; ++step) {
            Order *order = randomOrder(rng, o, randInt(rng, 0, 1) == 0 ? Side::BUY : Side::SELL, true);
            originalQty[order->orderId] = order->quantity;
            std::unordered_map<OrderId, Price> restingPrice;
            for (const auto &[id, r]: e.book.index) restingPrice[id] = *r->price;

            const auto trades = e.submit(order);

            const Quantity filled = totalQty(trades);
            if (order->tif == TimeInForce::FOK) CHECK_SEED(filled == 0 || filled == originalQty[order->orderId], seed);
            for (const Trade &t: trades) {
                const OrderId maker = order->side == Side::BUY ? t.sellOrderId : t.buyOrderId;
                CHECK_SEED(t.price == restingPrice.at(maker), seed);
                if (order->price.has_value()) {
                    CHECK_SEED(order->side == Side::BUY ? t.price <= *order->price : t.price >= *order->price, seed);
                }
            }
            if (e.book.index.count(order->orderId) != 0) {
                CHECK_SEED(order->tif == TimeInForce::GTC && order->price.has_value(), seed);
                CHECK_SEED(order->quantity == originalQty[order->orderId] - filled, seed);
            }

            // Book consistency: levels and index agree, nothing empty, nothing crossed.
            std::unordered_set<OrderId> queued;
            for (const auto *side: {&e.book.bids, &e.book.asks}) {
                for (const auto &[price, level]: *side) {
                    CHECK_SEED(!level.empty(), seed);
                    for (const OrderId id: level) {
                        const Order *r = e.book.index.at(id);
                        CHECK_SEED(*r->price == price && r->quantity > 0 && r->tif == TimeInForce::GTC, seed);
                        CHECK_SEED(queued.insert(id).second, seed);
                    }
                }
            }
            CHECK_SEED(queued.size() == e.book.index.size(), seed);
            if (!e.book.bids.empty() && !e.book.asks.empty()) {
                CHECK_SEED(e.book.bids.rbegin()->first < e.book.asks.begin()->first, seed);
            }
        }

        for (const auto &[id, q]: tradedByOrder(e.trades)) CHECK_SEED(q <= originalQty.at(id), seed);
    }
}
