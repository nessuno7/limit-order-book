//
// Port of tests/test_continuous_auction.py and tests/test_fok_continuous.py,
// plus the continuous-auction cases of tests/test_edge_cases.py.
//

#include "helpers.h"
#include "test_framework.h"

#include "continuous_auction/continuous_auction.h"

#include <stdexcept>

namespace {
    Quantity restingQty(const OrderBook &book, OrderId id) { return book.index.at(id)->quantity; }
}

// ---- no match --------------------------------------------------------------

TEST(continuous_buy_with_no_asks_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    CHECK(e.submit(o.gtc(Side::BUY, 100, 10)).empty());
    CHECK_EQ(e.book.bestBid()->first, 100);
}

TEST(continuous_sell_with_no_bids_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    CHECK(e.submit(o.gtc(Side::SELL, 100, 10)).empty());
    CHECK_EQ(e.book.bestAsk()->first, 100);
}

TEST(continuous_buy_price_below_ask_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 105, 10));
    CHECK(e.submit(o.gtc(Side::BUY, 100, 10)).empty());
    CHECK_EQ(e.book.bestBid()->first, 100);
    CHECK_EQ(e.book.bestAsk()->first, 105);
}

TEST(continuous_sell_price_above_bid_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::BUY, 100, 10));
    CHECK(e.submit(o.gtc(Side::SELL, 105, 10)).empty());
}

// ---- matching --------------------------------------------------------------

TEST(continuous_exact_qty_and_price) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 10));
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 10);
    CHECK_EQ(trades[0].price, 100);
    CHECK(!e.book.bestBid().has_value());
    CHECK(!e.book.bestAsk().has_value());
}

TEST(continuous_sell_aggresses_exact_match) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::BUY, 100, 10));
    const auto trades = e.submit(o.gtc(Side::SELL, 100, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 10);
}

TEST(continuous_aggressor_smaller_resting_has_remainder) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 10));
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 4));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 4);
    CHECK_EQ(e.book.bestAsk()->first, 100);
    CHECK_EQ(restingQty(e.book, e.book.bestAsk()->second), 6);
}

TEST(continuous_resting_smaller_aggressor_rests_remainder) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 4));
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 4);
    CHECK_EQ(e.book.bestBid()->first, 100);
    CHECK_EQ(restingQty(e.book, e.book.bestBid()->second), 6);
}

TEST(continuous_trade_executes_at_resting_price) {
    Orders o;
    ContinuousDoubleAuction e1;
    e1.submit(o.gtc(Side::SELL, 99, 10));
    CHECK_EQ(e1.submit(o.gtc(Side::BUY, 105, 10))[0].price, 99);

    ContinuousDoubleAuction e2;
    e2.submit(o.gtc(Side::BUY, 105, 10));
    CHECK_EQ(e2.submit(o.gtc(Side::SELL, 99, 10))[0].price, 105);
}

TEST(continuous_earlier_resting_order_fills_first) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *s1 = o.gtc(Side::SELL, 100, 5);
    Order *s2 = o.gtc(Side::SELL, 100, 5);
    e.submit(s1);
    e.submit(s2);
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 5));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, s1->orderId);
    CHECK_EQ(e.book.asks.at(100).size(), 1u);
}

TEST(continuous_buy_sweeps_two_ask_levels) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 5));
    e.submit(o.gtc(Side::SELL, 101, 5));
    const auto trades = e.submit(o.gtc(Side::BUY, 101, 10));
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(totalQty(trades), 10);
    CHECK(!e.book.bestAsk().has_value());
}

TEST(continuous_sell_sweeps_two_bid_levels) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::BUY, 101, 5));
    e.submit(o.gtc(Side::BUY, 100, 5));
    const auto trades = e.submit(o.gtc(Side::SELL, 100, 10));
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(totalQty(trades), 10);
    CHECK(!e.book.bestBid().has_value());
}

TEST(continuous_trades_stored_on_engine) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 5));
    e.submit(o.gtc(Side::SELL, 101, 5));
    e.submit(o.gtc(Side::BUY, 101, 10));
    CHECK_EQ(e.trades.size(), 2u);
}

// ---- market orders ---------------------------------------------------------

TEST(continuous_market_orders_hit_best_price) {
    Orders o;
    ContinuousDoubleAuction e1;
    e1.submit(o.gtc(Side::SELL, 100, 10));
    auto trades = e1.submit(o.mkt(Side::BUY, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].price, 100);

    ContinuousDoubleAuction e2;
    e2.submit(o.gtc(Side::BUY, 100, 10));
    trades = e2.submit(o.mkt(Side::SELL, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].price, 100);
}

TEST(continuous_market_buy_sweeps_multiple_levels) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 5));
    e.submit(o.gtc(Side::SELL, 101, 5));
    const auto trades = e.submit(o.mkt(Side::BUY, 10));
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(totalQty(trades), 10);
}

TEST(continuous_gtc_market_remainder_never_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 4));
    Order *m = o.mkt(Side::BUY, 10);
    CHECK_EQ(totalQty(e.submit(m)), 4);
    CHECK(e.book.bids.empty() && e.book.asks.empty());
    CHECK(e.book.index.count(m->orderId) == 0);
}

TEST(continuous_market_fok_fills_when_liquidity_suffices) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 5));
    e.submit(o.gtc(Side::SELL, 101, 5));
    CHECK_EQ(totalQty(e.submit(o.mkt(Side::BUY, 8, TimeInForce::FOK))), 8);
}

TEST(continuous_market_fok_rejected_when_liquidity_short) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::BUY, 100, 3));
    CHECK(e.submit(o.mkt(Side::SELL, 5, TimeInForce::FOK)).empty());
    CHECK_EQ(restingQty(e.book, e.book.bestBid()->second), 3);
}

TEST(continuous_ioc_limit_remainder_does_not_rest) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 4));
    CHECK_EQ(totalQty(e.submit(o.ioc(Side::BUY, 100, 10))), 4);
    CHECK(e.book.bids.empty());
}

TEST(continuous_matching_after_cancelling_non_front_order) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *s1 = o.gtc(Side::SELL, 100, 5);
    Order *s2 = o.gtc(Side::SELL, 100, 5);
    e.submit(s1);
    e.submit(s2);
    e.book.cancel(s2->orderId);
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, s1->orderId);
    CHECK_EQ(e.book.bestBid()->first, 100);
}

TEST(continuous_resubmitting_a_resting_order_is_rejected) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *b = o.gtc(Side::BUY, 100, 5);
    e.submit(b);
    CHECK_THROWS(e.submit(b), std::invalid_argument);
    CHECK_EQ(e.book.bids.at(100).size(), 1u);
}

TEST(continuous_resubmitting_a_filled_order_is_rejected) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 5));
    Order *b = o.gtc(Side::BUY, 100, 5);
    e.submit(b);
    CHECK_EQ(b->quantity, 0);
    e.submit(o.gtc(Side::SELL, 100, 5));
    CHECK_THROWS(e.submit(b), std::invalid_argument);
    for (const Trade &t: e.trades) CHECK(t.quantity > 0);
    CHECK(e.book.index.count(b->orderId) == 0);
}

// ---- FOK -------------------------------------------------------------------

TEST(continuous_fok_rejected) {
    Orders o;
    {
        ContinuousDoubleAuction e; // no liquidity
        CHECK(e.submit(o.fok(Side::BUY, 100, 5)).empty());
        CHECK(e.submit(o.fok(Side::SELL, 100, 5)).empty());
    }
    {
        ContinuousDoubleAuction e; // insufficient volume
        e.submit(o.gtc(Side::SELL, 100, 3));
        CHECK(e.submit(o.fok(Side::BUY, 100, 5)).empty());
    }
    {
        ContinuousDoubleAuction e;
        e.submit(o.gtc(Side::BUY, 100, 3));
        CHECK(e.submit(o.fok(Side::SELL, 100, 5)).empty());
    }
    {
        ContinuousDoubleAuction e; // price does not cross
        e.submit(o.gtc(Side::SELL, 105, 10));
        CHECK(e.submit(o.fok(Side::BUY, 100, 5)).empty());
    }
}

TEST(continuous_fok_fills) {
    Orders o;
    {
        ContinuousDoubleAuction e;
        e.submit(o.gtc(Side::SELL, 100, 5));
        const auto trades = e.submit(o.fok(Side::BUY, 100, 5));
        CHECK_EQ(trades.size(), 1u);
        CHECK_EQ(trades[0].quantity, 5);
    }
    {
        ContinuousDoubleAuction e;
        e.submit(o.gtc(Side::SELL, 100, 10));
        CHECK_EQ(totalQty(e.submit(o.fok(Side::BUY, 100, 5))), 5);
    }
    {
        ContinuousDoubleAuction e;
        e.submit(o.gtc(Side::SELL, 100, 3));
        e.submit(o.gtc(Side::SELL, 101, 3));
        const auto trades = e.submit(o.fok(Side::BUY, 101, 6));
        CHECK_EQ(trades.size(), 2u);
        CHECK_EQ(totalQty(trades), 6);
    }
    {
        ContinuousDoubleAuction e;
        e.submit(o.gtc(Side::BUY, 100, 5));
        CHECK_EQ(totalQty(e.submit(o.fok(Side::SELL, 100, 5))), 5);
    }
}

TEST(continuous_rejected_fok_never_rests) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.fok(Side::BUY, 100, 5));
    CHECK(!e.book.bestBid().has_value());
    // ...so a later sell finds no buyer and rests itself.
    CHECK(e.submit(o.gtc(Side::SELL, 100, 5)).empty());
    CHECK_EQ(e.book.bestAsk()->first, 100);
}

TEST(continuous_partial_volume_fok_not_partially_filled) {
    Orders o;
    ContinuousDoubleAuction e;
    e.submit(o.gtc(Side::SELL, 100, 3));
    e.submit(o.fok(Side::BUY, 100, 5));
    CHECK_EQ(restingQty(e.book, e.book.bestAsk()->second), 3);
}

// ---- modify ----------------------------------------------------------------

TEST(continuous_modify_reduce_keeps_priority) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *first = o.gtc(Side::SELL, 100, 10);
    Order *second = o.gtc(Side::SELL, 100, 10);
    e.submit(first);
    e.submit(second);
    CHECK(e.modify(first->orderId, 100, 4).empty());
    CHECK_EQ(restingQty(e.book, first->orderId), 4);
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 4));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, first->orderId); // still at the front
}

TEST(continuous_modify_increase_loses_priority) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *first = o.gtc(Side::SELL, 100, 10);
    Order *second = o.gtc(Side::SELL, 100, 10);
    e.submit(first);
    e.submit(second);
    e.modify(first->orderId, 100, 11);
    const auto trades = e.submit(o.gtc(Side::BUY, 100, 10));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, second->orderId);
    CHECK_EQ(restingQty(e.book, first->orderId), 11);
}

TEST(continuous_modify_same_price_same_qty_is_a_noop) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *first = o.gtc(Side::BUY, 100, 5);
    e.submit(first);
    e.submit(o.gtc(Side::BUY, 100, 5));
    e.modify(first->orderId, 100, 5);
    CHECK_EQ(e.book.bestBid()->second, first->orderId);
}

TEST(continuous_modify_price_change_moves_level) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *b = o.gtc(Side::BUY, 100, 5);
    e.submit(b);
    CHECK(e.modify(b->orderId, 98, 5).empty());
    CHECK_EQ(e.book.bestBid()->first, 98);
    CHECK(e.book.bids.count(100) == 0);
}

TEST(continuous_modify_to_crossing_price_trades) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *ask = o.gtc(Side::SELL, 101, 3);
    Order *bid = o.gtc(Side::BUY, 99, 5);
    e.submit(ask);
    e.submit(bid);
    const auto trades = e.modify(bid->orderId, 102, 5);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].price, 101); // the resting ask's price
    CHECK_EQ(trades[0].quantity, 3);
    CHECK_EQ(restingQty(e.book, bid->orderId), 2);
    CHECK_EQ(e.book.bestBid()->first, 102);
    CHECK_EQ(e.trades.size(), 1u);
}

TEST(continuous_modify_rejects_unknown_id_and_bad_qty) {
    Orders o;
    ContinuousDoubleAuction e;
    Order *b = o.gtc(Side::BUY, 100, 5);
    CHECK_THROWS(e.modify(b->orderId, 100, 1), std::out_of_range);
    e.submit(b);
    CHECK_THROWS(e.modify(b->orderId, 100, 0), std::invalid_argument);
    CHECK_THROWS(e.modify(b->orderId, 100, -3), std::invalid_argument);
    CHECK_EQ(restingQty(e.book, b->orderId), 5);
}
