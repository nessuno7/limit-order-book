//
// Directed tests for fast/FastBook.h. The differential fuzzer (fuzz/fuzz.cpp)
// covers the matching rules against a reference book; these pin down the
// data structures underneath, where a bug may take thousands of random ops
// to surface: the id hash table, bitmap scans across 64-level words, the
// free list, and the price range.
//

#include "test_framework.h"

#include "fast/FastBook.h"

#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using fast::Fill;
using fast::FastBook;
using fast::MapBook;

namespace {
    template<class Book>
    std::vector<Fill> submit(Book &b, OrderId id, Side side, std::optional<Price> price, Quantity qty,
                             TimeInForce tif = TimeInForce::GTC) {
        const auto fills = b.submit(id, side, price, qty, tif);
        return {fills.begin(), fills.end()};
    }
}

// ---- id index ----------------------------------------------------------------

TEST(fast_id_index_matches_unordered_map_under_churn) {
    // Random inserts and erases over a small key range force long probe runs
    // and wrap-around, which is where backward-shift deletion goes wrong.
    fast::IdIndex index(64);
    std::unordered_map<OrderId, fast::Slot> model;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        const OrderId id = rng() % 200;
        if (model.count(id) != 0) {
            CHECK_EQ(index.find(id), model[id]);
            index.erase(id);
            model.erase(id);
        } else if (model.size() < 64) {
            const auto slot = static_cast<fast::Slot>(rng() % 1000);
            index.insert(id, slot);
            model[id] = slot;
        }
        const OrderId probe = rng() % 200;
        const auto it = model.find(probe);
        CHECK_EQ(index.find(probe), it == model.end() ? fast::NIL : it->second);
    }
}

TEST(fast_id_index_erase_of_missing_id_is_harmless) {
    fast::IdIndex index(8);
    index.insert(1, 10);
    index.erase(2);
    CHECK_EQ(index.find(1), 10u);
}

// ---- pool --------------------------------------------------------------------

TEST(fast_pool_reuses_the_most_recently_freed_slot) {
    fast::OrderPool pool(4);
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    pool.release(a);
    CHECK_EQ(pool.acquire(), a); // LIFO: the cache-warm slot comes back first
    CHECK(b != a);
    CHECK_EQ(pool.size(), 2u);
}

TEST(fast_pool_exhaustion_throws) {
    FastBook book(1, 100, 2);
    submit(book, 1, Side::BUY, 10, 1);
    submit(book, 2, Side::BUY, 11, 1);
    CHECK_THROWS(book.submit(3, Side::BUY, 12, 1, TimeInForce::GTC), std::length_error);
    CHECK(book.cancel(1));
    submit(book, 3, Side::BUY, 12, 1); // the freed slot is usable again
    CHECK_EQ(book.restingOrders(), 2u);
}

// ---- ladder ------------------------------------------------------------------

TEST(fast_best_bid_falls_back_across_bitmap_words) {
    FastBook book(0, 1'000, 16);
    submit(book, 1, Side::BUY, 3, 1);   // word 0
    submit(book, 2, Side::BUY, 64, 1);  // first bit of word 1
    submit(book, 3, Side::BUY, 700, 1); // word 10
    CHECK_EQ(*book.bestBid(), 700);
    book.cancel(3);
    CHECK_EQ(*book.bestBid(), 64);
    book.cancel(2);
    CHECK_EQ(*book.bestBid(), 3);
    book.cancel(1);
    CHECK(!book.bestBid().has_value());
}

TEST(fast_best_ask_advances_across_bitmap_words) {
    FastBook book(0, 1'000, 16);
    submit(book, 1, Side::SELL, 63, 1);  // last bit of word 0
    submit(book, 2, Side::SELL, 128, 1); // first bit of word 2
    submit(book, 3, Side::SELL, 1'000, 1); // top of the ladder
    CHECK_EQ(*book.bestAsk(), 63);
    book.cancel(1);
    CHECK_EQ(*book.bestAsk(), 128);
    book.cancel(2);
    CHECK_EQ(*book.bestAsk(), 1'000);
}

TEST(fast_sweep_across_levels_and_words) {
    FastBook book(0, 1'000, 64);
    for (Price p = 60; p < 200; p += 10) submit(book, static_cast<OrderId>(p), Side::SELL, p, 2);
    const auto fills = submit(book, 1'000, Side::BUY, 135, 100);
    CHECK_EQ(fills.size(), 8u); // 60, 70, ..., 130
    CHECK_EQ(fills.front().price, 60);
    CHECK_EQ(fills.back().price, 130);
    CHECK_EQ(*book.bestAsk(), 140);
    CHECK_EQ(*book.bestBid(), 135); // remainder 84 rests at its limit
}

TEST(fast_prices_outside_the_ladder_throw) {
    FastBook book(100, 200, 4);
    CHECK_THROWS(book.submit(1, Side::BUY, 99, 1, TimeInForce::GTC), std::out_of_range);
    CHECK_THROWS(book.submit(1, Side::SELL, 201, 1, TimeInForce::GTC), std::out_of_range);
    submit(book, 1, Side::BUY, 100, 1);
    submit(book, 2, Side::SELL, 200, 1);
    CHECK_THROWS(book.modify(1, 250, 1), std::out_of_range);
    CHECK_THROWS(FastBook(10, 5, 4), std::invalid_argument);
}

// ---- matching rules, on both ladders ----------------------------------------

template<class Book>
void checkPriceTimePriorityAndPartialFill() {
    Book book(1, 1'000, 16);
    submit(book, 1, Side::SELL, 101, 5);
    submit(book, 2, Side::SELL, 100, 5);
    submit(book, 3, Side::SELL, 100, 5);
    const auto fills = submit(book, 4, Side::BUY, 101, 12);
    CHECK_EQ(fills.size(), 3u);
    CHECK(fills[0] == (Fill{4, 2, 100, 5}));
    CHECK(fills[1] == (Fill{4, 3, 100, 5}));
    CHECK(fills[2] == (Fill{4, 1, 101, 2}));
    CHECK_EQ(book.restingOrders(), 1u);
}

TEST(fast_price_time_priority_and_partial_fill) {
    checkPriceTimePriorityAndPartialFill<FastBook>();
    checkPriceTimePriorityAndPartialFill<MapBook>();
}

template<class Book>
void checkTimeInForce() {
    Book book(1, 1'000, 16);
    submit(book, 1, Side::SELL, 100, 5);
    CHECK(submit(book, 2, Side::BUY, 100, 6, TimeInForce::FOK).empty()); // 5 < 6: killed
    CHECK_EQ(book.restingOrders(), 1u);
    CHECK_EQ(submit(book, 3, Side::BUY, 100, 5, TimeInForce::FOK).size(), 1u); // exactly enough
    submit(book, 4, Side::SELL, 100, 5);
    CHECK_EQ(submit(book, 5, Side::BUY, 100, 8, TimeInForce::IOC).size(), 1u);
    CHECK(!book.contains(5)); // IOC remainder dropped
    submit(book, 6, Side::SELL, 105, 3);
    CHECK_EQ(submit(book, 7, Side::BUY, std::nullopt, 10).size(), 1u); // market
    CHECK(!book.contains(7)); // market remainder never rests
    CHECK_EQ(book.restingOrders(), 0u);
}

TEST(fast_ioc_fok_and_market_orders) {
    checkTimeInForce<FastBook>();
    checkTimeInForce<MapBook>();
}

template<class Book>
void checkModify() {
    Book book(1, 1'000, 16);
    submit(book, 1, Side::BUY, 100, 10);
    submit(book, 2, Side::BUY, 100, 10);
    CHECK(book.modify(1, 100, 4).empty()); // reduce: keeps the front
    auto fills = submit(book, 3, Side::SELL, 100, 4);
    CHECK(fills.size() == 1 && fills[0].buyOrderId == 1);
    book.modify(2, 100, 20); // increase: requeued, still alone at 100
    submit(book, 4, Side::BUY, 100, 1);
    fills = submit(book, 5, Side::SELL, 100, 21);
    CHECK_EQ(fills.size(), 2u);
    CHECK_EQ(fills[0].buyOrderId, 2u);
    CHECK_EQ(fills[1].buyOrderId, 4u);
    submit(book, 6, Side::SELL, 105, 3);
    submit(book, 7, Side::BUY, 99, 5);
    const auto crossed = book.modify(7, 105, 5); // new price crosses
    CHECK(crossed.size() == 1 && crossed[0] == (Fill{7, 6, 105, 3}));
    CHECK_EQ(*book.bestBid(), 105);
    CHECK_THROWS(book.modify(99, 100, 1), std::out_of_range);
    CHECK_THROWS(book.modify(7, 105, 0), std::invalid_argument);
}

TEST(fast_modify_rules) {
    checkModify<FastBook>();
    checkModify<MapBook>();
}

TEST(fast_rejects_duplicates_and_bad_quantities) {
    FastBook book(1, 1'000, 16);
    submit(book, 1, Side::BUY, 100, 1);
    CHECK_THROWS(book.submit(1, Side::BUY, 100, 1, TimeInForce::GTC), std::invalid_argument);
    CHECK_THROWS(book.submit(2, Side::BUY, 100, 0, TimeInForce::GTC), std::invalid_argument);
    CHECK(!book.cancel(42));
}
