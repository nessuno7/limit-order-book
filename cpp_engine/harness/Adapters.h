//
// One interface over every continuous-book implementation, so the fuzzer
// and the latency benchmarks drive them all with the same op stream:
//
//   prepare(op)  work the caller would do before calling the book, kept out
//                of timed regions (the engine needs an Order object built)
//   run(op)      the book call itself; returns the number of fills, or
//                NOT_RESTING for a cancel of an id that is not in the book
//                (routine: the order may have just filled), without throwing
//   apply(op)    prepare + run, reporting the full Outcome (for the fuzzer)
//   bestBid(), bestAsk(), restingOrders(), contains(id), snapshot()
//
// Other rejections are thrown by the books and translated to a Status in
// apply(), off the hot path.
//

#ifndef LIMIT_ORDER_BOOK_ADAPTERS_H
#define LIMIT_ORDER_BOOK_ADAPTERS_H

#include "continuous_auction/ContinuousDoubleAuction.h"
#include "fast/FastBook.h"
#include "harness/Op.h"
#include "orderbook/OrderArena.h"

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

inline constexpr std::size_t NOT_RESTING = std::numeric_limits<std::size_t>::max();

// The original engine: std::map levels, deque<OrderId> queues, an
// unordered_map index, Order objects in an arena, std::vector<Trade> results.
class EngineAdapter {
public:
    static constexpr const char *NAME = "engine (std::map)";

    EngineAdapter(Price, Price, std::size_t) {}

    void prepare(const Op &op) {
        if (op.kind != OpKind::Add) return;
        staged = arena.create(op.side, op.price, op.quantity, TraderId{1}, op.tif, Datetime{});
        staged->orderId = op.id; // the stream's id, so fills compare across books
    }

    std::size_t run(const Op &op) {
        switch (op.kind) {
            case OpKind::Add: return (last = engine.submit(staged)).size();
            case OpKind::Cancel:
                if (engine.book.index.count(op.id) == 0) return NOT_RESTING;
                engine.book.cancel(op.id);
                return 0;
            case OpKind::Modify: return (last = engine.modify(op.id, *op.price, op.quantity, Datetime{})).size();
        }
        return 0;
    }

    Outcome apply(const Op &op) {
        Outcome out;
        try {
            prepare(op);
            last.clear();
            if (run(op) == NOT_RESTING) out.status = Status::NotFound;
        } catch (const std::out_of_range &) {
            out.status = Status::NotFound;
        } catch (const std::invalid_argument &) {
            out.status = Status::Invalid;
        }
        for (const Trade &t: last) out.fills.push_back({t.buyOrderId, t.sellOrderId, t.price, t.quantity});
        return out;
    }

    [[nodiscard]] std::optional<Price> bestBid() const {
        const auto b = engine.book.bestBid();
        return b ? std::optional<Price>(b->first) : std::nullopt;
    }

    [[nodiscard]] std::optional<Price> bestAsk() const {
        const auto a = engine.book.bestAsk();
        return a ? std::optional<Price>(a->first) : std::nullopt;
    }

    [[nodiscard]] std::size_t restingOrders() const { return engine.book.index.size(); }
    [[nodiscard]] bool contains(const OrderId id) const { return engine.book.index.count(id) != 0; }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot s;
        const auto &book = engine.book;
        for (auto it = book.bids.rbegin(); it != book.bids.rend(); ++it) {
            for (const OrderId id: it->second) s.push_back({Side::BUY, it->first, id, book.index.at(id)->quantity});
        }
        for (const auto &[price, queue]: book.asks) {
            for (const OrderId id: queue) s.push_back({Side::SELL, price, id, book.index.at(id)->quantity});
        }
        return s;
    }

private:
    ContinuousDoubleAuction engine;
    OrderArena arena;
    Order *staged = nullptr;
    std::vector<Trade> last;
};

// fast::FastBook (flat array ladder) or fast::MapBook (std::map ladder).
template<class Book>
class FastAdapter {
public:
    static constexpr const char *NAME = std::is_same_v<Book, fast::FastBook>
                                            ? "FastBook (flat array)"
                                            : "MapBook (std::map + pool)";

    FastAdapter(const Price minPrice, const Price maxPrice, const std::size_t maxOrders)
        : book(minPrice, maxPrice, maxOrders) {}

    void prepare(const Op &) {}

    std::size_t run(const Op &op) {
        switch (op.kind) {
            case OpKind::Add: return (last = book.submit(op.id, op.side, op.price, op.quantity, op.tif)).size();
            case OpKind::Cancel:
                return book.cancel(op.id) ? 0 : NOT_RESTING;
            case OpKind::Modify: return (last = book.modify(op.id, *op.price, op.quantity)).size();
        }
        return 0;
    }

    Outcome apply(const Op &op) {
        Outcome out;
        last = {};
        try {
            if (run(op) == NOT_RESTING) out.status = Status::NotFound;
        } catch (const std::out_of_range &) {
            out.status = Status::NotFound;
        } catch (const std::invalid_argument &) {
            out.status = Status::Invalid;
        }
        out.fills.assign(last.begin(), last.end());
        return out;
    }

    [[nodiscard]] std::optional<Price> bestBid() const { return book.bestBid(); }
    [[nodiscard]] std::optional<Price> bestAsk() const { return book.bestAsk(); }
    [[nodiscard]] std::size_t restingOrders() const { return book.restingOrders(); }
    [[nodiscard]] bool contains(const OrderId id) const { return book.contains(id); }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot s;
        for (const Side side: {Side::BUY, Side::SELL}) {
            book.forEachOrder(side, [&](const Price p, const OrderId id, const Quantity q) {
                s.push_back({side, p, id, q});
            });
        }
        return s;
    }

private:
    Book book;
    std::span<const fast::Fill> last;
};

using FastBookAdapter = FastAdapter<fast::FastBook>;
using MapBookAdapter = FastAdapter<fast::MapBook>;

#endif //LIMIT_ORDER_BOOK_ADAPTERS_H
