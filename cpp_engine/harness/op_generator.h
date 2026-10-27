//
// Seeded random order flow for the fuzzer and the latency benchmarks.
//
// The flow is shaped like a real book rather than uniform noise: a mid price
// that random-walks, passive orders a few ticks behind it, a minority that
// cross (and sometimes sweep several levels), cancels and modifies aimed at
// orders that are actually resting, and a small share of invalid requests
// (unknown ids, zero quantities) so the error paths are exercised too.
//
// The generator needs to know which ids are still resting. It asks a book
// (any type with contains(OrderId)) that the caller keeps in step by applying
// each op it is handed.
//

#ifndef LIMIT_ORDER_BOOK_OP_GENERATOR_H
#define LIMIT_ORDER_BOOK_OP_GENERATOR_H

#include "harness/op.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

struct FlowParams {
    Price minPrice = 1;
    Price maxPrice = 20'000;
    Price startMid = 10'000;
    int addPct = 55;     // the rest is split between cancel and modify
    int cancelPct = 25;
    int marketPct = 4;   // share of adds with no limit
    int iocPct = 15;     // share of adds
    int fokPct = 15;
    int crossTicks = 3;  // a limit can sit up to this many ticks through the mid
    int depthTicks = 15; // ... and up to this many behind it
    int badPct = 2;      // cancels/modifies aimed at unknown ids, zero quantities
    int midEvery = 64;   // the mid moves on average once per this many ops ...
    int midStep = 1;     // ... by up to this many ticks
    std::size_t targetDepth = 1'000'000; // above this many resting orders, adds turn into cancels
};

// Three shapes of flow; the fuzzer rotates through them by seed.
//   tight:    the default; a shallow book that trades often
//   deep:     wide quotes and few cancels; thousands of resting orders over
//             hundreds of levels, so queues are long and level scans cross
//             many bitmap words
//   volatile: a fast-moving mid and deep crossing limits; frequent sweeps
inline FlowParams flowProfile(const int which) {
    FlowParams p;
    if (which == 1) {
        p.addPct = 62;
        p.cancelPct = 14;
        p.depthTicks = 400;
        p.targetDepth = 5'000;
    } else if (which == 2) {
        p.crossTicks = 40;
        p.depthTicks = 40;
        p.midEvery = 8;
        p.midStep = 6;
    }
    return p;
}

inline const char *flowName(const int which) { return which == 1 ? "deep" : which == 2 ? "volatile" : "tight"; }

class OpGenerator {
public:
    explicit OpGenerator(const std::uint64_t seed, FlowParams params = {})
        : p(params), rng(seed), mid(params.startMid) {}

    template<class Book>
    Op next(const Book &book) {
        if (rng() % static_cast<std::uint64_t>(p.midEvery) == 0) walkMid();
        const int roll = pct();
        if (roll < p.addPct && book.restingOrders() <= p.targetDepth) return add();
        if (roll < p.addPct) return Op::of(OpKind::Cancel, target(book));
        if (roll < p.addPct + p.cancelPct) return Op::of(OpKind::Cancel, target(book));
        return modify(book);
    }

    [[nodiscard]] OrderId idsIssued() const { return nextId - 1; }

private:
    int pct() { return static_cast<int>(rng() % 100); }

    std::int64_t uniform(const std::int64_t lo, const std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    }

    void walkMid() {
        mid += uniform(-p.midStep, p.midStep);
        const Price margin = p.depthTicks + p.crossTicks + 10;
        mid = std::clamp(mid, p.minPrice + margin, p.maxPrice - margin);
    }

    Quantity quantity() {
        // Mostly small, sometimes large enough to sweep several levels.
        const int roll = pct();
        if (roll < 80) return uniform(1, 20);
        if (roll < 97) return uniform(21, 100);
        return uniform(101, 600);
    }

    Price limitFor(const Side side) {
        const Price offset = uniform(-p.crossTicks, p.depthTicks); // negative = through the mid
        return side == Side::BUY ? mid - offset : mid + offset;
    }

    Op add() {
        Op op = Op::of(OpKind::Add, nextId++);
        op.side = rng() % 2 == 0 ? Side::BUY : Side::SELL;
        op.quantity = quantity();
        if (pct() >= p.marketPct) op.price = limitFor(op.side);
        const int t = pct();
        op.tif = t < p.iocPct ? TimeInForce::IOC : t < p.iocPct + p.fokPct ? TimeInForce::FOK : TimeInForce::GTC;
        recent.push_back(op.id);
        return op;
    }

    template<class Book>
    Op modify(const Book &book) {
        Op op = Op::of(OpKind::Modify, target(book));
        const Side side = rng() % 2 == 0 ? Side::BUY : Side::SELL; // only shapes the new price
        const int roll = pct();
        if (roll < 40) { // reduce in place: keeps priority if the price is unchanged
            op.price = lastPrice(book, op.id, side);
            op.quantity = uniform(1, 10);
        } else if (roll < 70) { // new price, may cross
            op.price = limitFor(side);
            op.quantity = quantity();
        } else { // same price, maybe more quantity
            op.price = lastPrice(book, op.id, side);
            op.quantity = quantity();
        }
        if (pct() < p.badPct) op.quantity = 0;
        return op;
    }

    // An id that is resting, most of the time; occasionally a dead or never
    // issued one. Dead ids are dropped from `recent` as they are found.
    template<class Book>
    OrderId target(const Book &book) {
        if (pct() < p.badPct || recent.empty()) {
            return rng() % 2 == 0 ? nextId + 1'000'000 : static_cast<OrderId>(uniform(1, static_cast<std::int64_t>(nextId)));
        }
        for (int tries = 0; tries < 8 && !recent.empty(); ++tries) {
            const std::size_t i = static_cast<std::size_t>(rng() % recent.size());
            const OrderId id = recent[i];
            if (book.contains(id)) return id;
            recent[i] = recent.back();
            recent.pop_back();
        }
        return recent.empty() ? nextId + 1'000'000 : recent[static_cast<std::size_t>(rng() % recent.size())];
    }

    // The price a modify keeps when it is not meant to move the order: the
    // order's current price if the book can tell us, else near the mid.
    template<class Book>
    Price lastPrice(const Book &book, const OrderId id, const Side side) {
        if constexpr (requires { book.priceOf(id); }) {
            if (const auto price = book.priceOf(id)) return *price;
        }
        return limitFor(side);
    }

    FlowParams p;
    std::mt19937_64 rng;
    Price mid;
    OrderId nextId = 1;
    std::vector<OrderId> recent; // ids that may still be resting
};

#endif //LIMIT_ORDER_BOOK_OP_GENERATOR_H
