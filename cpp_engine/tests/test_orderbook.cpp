//
// Port of tests/test_orderbook.py, plus TestCancelAnyOrder from
// tests/test_edge_cases.py.
//

#include "helpers.h"
#include "test_framework.h"

#include "orderbook/Orderbook.h"

#include <deque>
#include <stdexcept>

// ---- add -------------------------------------------------------------------

TEST(orderbook_bid_appears_in_bids) {
    Orders o;
    OrderBook book("TEST");
    Order *b = o.gtc(Side::BUY, 100, 10);
    book.add(b);
    CHECK(book.bids.count(100) == 1);
    CHECK(book.index.at(b->orderId) == b);
}

TEST(orderbook_ask_appears_in_asks) {
    Orders o;
    OrderBook book("TEST");
    Order *s = o.gtc(Side::SELL, 105, 5);
    book.add(s);
    CHECK(book.asks.count(105) == 1);
    CHECK(book.index.at(s->orderId) == s);
}

TEST(orderbook_multiple_orders_same_level_queued) {
    Orders o;
    OrderBook book("TEST");
    book.add(o.gtc(Side::BUY, 100, 10));
    book.add(o.gtc(Side::BUY, 100, 5));
    CHECK_EQ(book.bids.at(100).size(), 2u);
}

TEST(orderbook_orders_at_different_levels) {
    Orders o;
    OrderBook book("TEST");
    book.add(o.gtc(Side::BUY, 100, 10));
    book.add(o.gtc(Side::BUY, 101, 5));
    CHECK(book.bids.count(100) == 1 && book.bids.count(101) == 1);
}

TEST(orderbook_market_order_not_added) {
    Orders o;
    OrderBook book("TEST");
    book.add(o.mkt(Side::BUY, 10));
    CHECK(book.bids.empty());
    CHECK(book.index.empty());
}

TEST(orderbook_duplicate_add_rejected) {
    Orders o;
    OrderBook book("TEST");
    Order *b = o.gtc(Side::BUY, 100, 10);
    book.add(b);
    CHECK_THROWS(book.add(b), std::invalid_argument);
    CHECK_EQ(book.bids.at(100).size(), 1u);
}

// ---- best prices -----------------------------------------------------------

TEST(orderbook_best_bid_and_ask_empty) {
    OrderBook book("TEST");
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
}

TEST(orderbook_best_bid_single) {
    Orders o;
    OrderBook book("TEST");
    Order *b = o.gtc(Side::BUY, 100, 10);
    book.add(b);
    CHECK_EQ(book.bestBid()->first, 100);
    CHECK_EQ(book.bestBid()->second, b->orderId);
}

TEST(orderbook_best_ask_single) {
    Orders o;
    OrderBook book("TEST");
    Order *s = o.gtc(Side::SELL, 105, 5);
    book.add(s);
    CHECK_EQ(book.bestAsk()->first, 105);
    CHECK_EQ(book.bestAsk()->second, s->orderId);
}

TEST(orderbook_best_bid_is_highest_price) {
    Orders o;
    OrderBook book("TEST");
    book.add(o.gtc(Side::BUY, 99, 3));
    book.add(o.gtc(Side::BUY, 100, 10));
    book.add(o.gtc(Side::BUY, 101, 5));
    CHECK_EQ(book.bestBid()->first, 101);
}

TEST(orderbook_best_ask_is_lowest_price) {
    Orders o;
    OrderBook book("TEST");
    book.add(o.gtc(Side::SELL, 107, 3));
    book.add(o.gtc(Side::SELL, 105, 10));
    book.add(o.gtc(Side::SELL, 103, 5));
    CHECK_EQ(book.bestAsk()->first, 103);
}

// ---- cancel ----------------------------------------------------------------

TEST(orderbook_cancel_removes_from_index_and_empty_level) {
    Orders o;
    OrderBook book("TEST");
    Order *b = o.gtc(Side::BUY, 100, 10);
    book.add(b);
    CHECK(book.cancel(b->orderId) == b);
    CHECK(book.index.empty());
    CHECK(book.bids.empty());
}

TEST(orderbook_cancel_front_leaves_level_intact) {
    Orders o;
    OrderBook book("TEST");
    Order *o1 = o.gtc(Side::BUY, 100, 10);
    Order *o2 = o.gtc(Side::BUY, 100, 5);
    book.add(o1);
    book.add(o2);
    book.cancel(o1->orderId);
    CHECK_EQ(book.bids.at(100).size(), 1u);
    CHECK_EQ(book.bids.at(100).front(), o2->orderId);
}

TEST(orderbook_cancel_middle_of_level_removes_that_order) {
    Orders o;
    OrderBook book("TEST");
    Order *o1 = o.gtc(Side::BUY, 100, 1);
    Order *o2 = o.gtc(Side::BUY, 100, 2);
    Order *o3 = o.gtc(Side::BUY, 100, 3);
    for (Order *x: {o1, o2, o3}) book.add(x);
    book.cancel(o2->orderId);
    CHECK(book.bids.at(100) == (std::deque<OrderId>{o1->orderId, o3->orderId}));
    CHECK_EQ(book.index.size(), 2u);
    CHECK(book.index.count(o2->orderId) == 0);
}

TEST(orderbook_cancel_back_of_level_keeps_front_as_best) {
    Orders o;
    OrderBook book("TEST");
    Order *o1 = o.gtc(Side::SELL, 100, 1);
    Order *o2 = o.gtc(Side::SELL, 100, 2);
    book.add(o1);
    book.add(o2);
    book.cancel(o2->orderId);
    CHECK_EQ(book.bestAsk()->second, o1->orderId);
}

TEST(orderbook_cancel_unknown_id_throws) {
    OrderBook book("TEST");
    CHECK_THROWS(book.cancel(987654321), std::out_of_range);
}

// ---- validation ------------------------------------------------------------

TEST(order_non_positive_quantity_rejected) {
    Orders o;
    CHECK_THROWS(o.gtc(Side::BUY, 100, 0), std::invalid_argument);
    CHECK_THROWS(o.gtc(Side::BUY, 100, -5), std::invalid_argument);
}

TEST(order_priority_breaks_timestamp_ties_by_arrival) {
    Orders o;
    Order *first = o.gtc(Side::BUY, 100, 1, 0);
    Order *second = o.gtc(Side::BUY, 100, 1, 0);
    CHECK(first->priority() < second->priority());
}
