//
// Port of tests/test_uniform.py and tests/test_fok_uniform.py, plus the
// uniform cases of tests/test_edge_cases.py.
//

#include "helpers.h"
#include "test_framework.h"

#include "call_auction/rules/Uniform.h"

#include <set>

namespace {
    Quantity tradedBy(const std::vector<Trade> &trades, const Order *o) {
        Quantity q = 0;
        for (const Trade &t: trades) {
            if (t.buyOrderId == o->orderId || t.sellOrderId == o->orderId) q += t.quantity;
        }
        return q;
    }

    bool allAtPrice(const std::vector<Trade> &trades, Price p) {
        for (const Trade &t: trades) {
            if (t.price != p) return false;
        }
        return true;
    }
}

// ---- no cross --------------------------------------------------------------

TEST(uniform_one_sided_batches_roll_gtc_forward) {
    Orders o;
    Order *s = o.gtc(Side::SELL, 100, 50);
    auto r = Uniform().clear({}, {s});
    CHECK(r.trades.empty());
    CHECK(contains(r.unfilled, s));

    Order *b = o.gtc(Side::BUY, 100, 50);
    r = Uniform().clear({b}, {});
    CHECK(r.trades.empty());
    CHECK(contains(r.unfilled, b));

    r = Uniform().clear({}, {});
    CHECK(r.trades.empty() && r.unfilled.empty());
}

TEST(uniform_no_price_overlap) {
    Orders o;
    Order *b = o.gtc(Side::BUY, 95, 50);
    Order *s = o.gtc(Side::SELL, 100, 50);
    auto r = Uniform().clear({b}, {s});
    CHECK(r.trades.empty());
    CHECK(contains(r.unfilled, b) && contains(r.unfilled, s));

    r = Uniform().clear({o.ioc(Side::BUY, 95, 50)}, {o.ioc(Side::SELL, 100, 50)});
    CHECK(r.unfilled.empty());
}

// ---- clearing price --------------------------------------------------------

TEST(uniform_tied_prices_highest_chosen) {
    Orders o;
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 50)}, {o.gtc(Side::SELL, 95, 50)});
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 105);
}

TEST(uniform_clearing_price_maximises_tradeable_volume) {
    Orders o;
    const auto r = Uniform().clear(
            {o.gtc(Side::BUY, 105, 50, 0), o.gtc(Side::BUY, 102, 50, 1)},
            {o.gtc(Side::SELL, 100, 80, 0), o.gtc(Side::SELL, 103, 60, 1)});
    CHECK(allAtPrice(r.trades, 102));
    CHECK_EQ(totalQty(r.trades), 80);
}

TEST(uniform_three_way_tie_minimum_surplus_chosen) {
    Orders o;
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 30)},
                                   {o.gtc(Side::SELL, 95, 30), o.gtc(Side::SELL, 100, 30)});
    CHECK(allAtPrice(r.trades, 95));
}

// ---- trade generation ------------------------------------------------------

TEST(uniform_single_exact_match) {
    Orders o;
    Order *b = o.gtc(Side::BUY, 105, 50);
    Order *s = o.gtc(Side::SELL, 100, 50);
    const auto r = Uniform().clear({b}, {s});
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 50);
    CHECK_EQ(r.trades[0].buyOrderId, b->orderId);
    CHECK_EQ(r.trades[0].sellOrderId, s->orderId);
    CHECK(r.unfilled.empty());
}

TEST(uniform_one_buy_splits_across_two_sells) {
    Orders o;
    Order *s1 = o.gtc(Side::SELL, 98, 60, 1);
    Order *s2 = o.gtc(Side::SELL, 102, 80, 2);
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 100, 0)}, {s1, s2});
    CHECK_EQ(r.trades.size(), 2u);
    CHECK_EQ(r.trades[0].sellOrderId, s1->orderId);
    CHECK_EQ(r.trades[0].quantity, 60);
    CHECK_EQ(r.trades[1].sellOrderId, s2->orderId);
    CHECK_EQ(r.trades[1].quantity, 40);
}

TEST(uniform_all_trades_execute_at_one_price) {
    Orders o;
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 100, 0)},
                                   {o.gtc(Side::SELL, 98, 60, 1), o.gtc(Side::SELL, 102, 40, 2)});
    CHECK_EQ(r.trades.size(), 2u);
    std::set<Price> prices;
    for (const Trade &t: r.trades) prices.insert(t.price);
    CHECK_EQ(prices.size(), 1u);
}

TEST(uniform_total_traded_equals_tradeable_volume) {
    Orders o;
    CHECK_EQ(totalQty(Uniform().clear({o.gtc(Side::BUY, 105, 100)}, {o.gtc(Side::SELL, 100, 60)}).trades), 60);
    CHECK_EQ(totalQty(Uniform().clear({o.gtc(Side::BUY, 105, 50), o.gtc(Side::BUY, 90, 50)},
                                      {o.gtc(Side::SELL, 100, 50)}).trades), 50);
}

// ---- rationing -------------------------------------------------------------

TEST(uniform_long_side_rationed_by_time) {
    Orders o;
    Order *b1 = o.gtc(Side::BUY, 105, 60, 1);
    Order *b2 = o.gtc(Side::BUY, 105, 50, 2);
    const auto r = Uniform().clear({b1, b2}, {o.gtc(Side::SELL, 100, 80, 0)});
    CHECK_EQ(tradedBy(r.trades, b1), 60);
    CHECK_EQ(tradedBy(r.trades, b2), 20);
    const Order *b2u = findById(r.unfilled, b2->orderId);
    CHECK(b2u != nullptr);
    CHECK_EQ(b2u->quantity, 30);

    Order *s1 = o.gtc(Side::SELL, 100, 60, 1);
    Order *s2 = o.gtc(Side::SELL, 100, 50, 2);
    const auto r2 = Uniform().clear({o.gtc(Side::BUY, 105, 80, 0)}, {s1, s2});
    CHECK_EQ(tradedBy(r2.trades, s1), 60);
    CHECK_EQ(tradedBy(r2.trades, s2), 20);
}

TEST(uniform_fully_rationed_order_returned_in_unfilled) {
    Orders o;
    Order *b2 = o.gtc(Side::BUY, 105, 50, 2);
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 80, 1), b2}, {o.gtc(Side::SELL, 100, 80, 0)});
    CHECK(contains(r.unfilled, b2));
}

// ---- time in force ---------------------------------------------------------

TEST(uniform_ineligible_orders_by_tif) {
    Orders o;
    Order *bLow = o.gtc(Side::BUY, 90, 30);
    auto r = Uniform().clear({o.gtc(Side::BUY, 105, 50), bLow}, {o.gtc(Side::SELL, 100, 50)});
    CHECK(contains(r.unfilled, bLow));

    Order *bLowIoc = o.ioc(Side::BUY, 90, 30);
    r = Uniform().clear({o.gtc(Side::BUY, 105, 50), bLowIoc}, {o.gtc(Side::SELL, 100, 50)});
    CHECK(!contains(r.unfilled, bLowIoc));

    Order *sHigh = o.gtc(Side::SELL, 110, 30);
    r = Uniform().clear({o.gtc(Side::BUY, 105, 50)}, {o.gtc(Side::SELL, 100, 50), sHigh});
    CHECK(contains(r.unfilled, sHigh));
}

TEST(uniform_rationed_ioc_order_dropped) {
    Orders o;
    Order *b2 = o.ioc(Side::BUY, 105, 50, 2);
    const auto r = Uniform().clear({o.gtc(Side::BUY, 105, 60, 1), b2}, {o.gtc(Side::SELL, 100, 80, 0)});
    CHECK(!contains(r.unfilled, b2));
}

TEST(uniform_one_sided_ioc_dropped) {
    Orders o;
    CHECK(Uniform().clear({}, {o.ioc(Side::SELL, 100, 50)}).unfilled.empty());
}

// ---- market orders ---------------------------------------------------------

TEST(uniform_market_orders_trade_with_limits) {
    Orders o;
    auto r = Uniform().clear({o.mkt(Side::BUY, 50)}, {o.gtc(Side::SELL, 100, 50)});
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 50);
    CHECK_EQ(r.trades[0].price, 100); // the only candidate is the limit price
    CHECK(r.unfilled.empty());

    r = Uniform().clear({o.gtc(Side::BUY, 105, 50)}, {o.mkt(Side::SELL, 50)});
    CHECK_EQ(totalQty(r.trades), 50);
    CHECK(r.unfilled.empty());
}

TEST(uniform_all_market_orders_no_price_discovery_no_trades) {
    Orders o;
    Order *b = o.mkt(Side::BUY, 50);
    Order *s = o.mkt(Side::SELL, 50);
    const auto r = Uniform().clear({b}, {s});
    CHECK(r.trades.empty());
    CHECK(contains(r.unfilled, b) && contains(r.unfilled, s));
}

// ---- FOK -------------------------------------------------------------------

TEST(uniform_fok_fills_fully_or_not_at_all) {
    Orders o;
    Order *b = o.fok(Side::BUY, 105, 50);
    auto r = Uniform().clear({b}, {o.gtc(Side::SELL, 100, 50)});
    CHECK_EQ(tradedBy(r.trades, b), 50);
    CHECK(!contains(r.unfilled, b));

    Order *s = o.fok(Side::SELL, 100, 50);
    r = Uniform().clear({o.gtc(Side::BUY, 105, 50)}, {s});
    CHECK_EQ(tradedBy(r.trades, s), 50);

    Order *big = o.fok(Side::BUY, 105, 100);
    Order *gs = o.gtc(Side::SELL, 100, 50);
    r = Uniform().clear({big}, {gs});
    CHECK(r.trades.empty());
    CHECK(!contains(r.unfilled, big));
    CHECK(contains(r.unfilled, gs));

    Order *bigS = o.fok(Side::SELL, 100, 100);
    Order *gb = o.gtc(Side::BUY, 105, 50);
    r = Uniform().clear({gb}, {bigS});
    CHECK(r.trades.empty());
    CHECK(!contains(r.unfilled, bigS));
    CHECK(contains(r.unfilled, gb));
}

TEST(uniform_fok_across_multiple_opposing_orders) {
    Orders o;
    Order *b = o.fok(Side::BUY, 105, 100);
    Order *s1 = o.gtc(Side::SELL, 100, 60, 0);
    Order *s2 = o.gtc(Side::SELL, 100, 60, 1);
    auto r = Uniform().clear({b}, {s1, s2});
    CHECK_EQ(totalQty(r.trades), 100);
    CHECK(!contains(r.unfilled, s1));
    CHECK(contains(r.unfilled, s2));

    Order *b2 = o.fok(Side::BUY, 105, 100);
    Order *t1 = o.gtc(Side::SELL, 100, 40, 0);
    Order *t2 = o.gtc(Side::SELL, 100, 40, 1);
    r = Uniform().clear({b2}, {t1, t2});
    CHECK(r.trades.empty());
    CHECK(contains(r.unfilled, t1) && contains(r.unfilled, t2));
}

TEST(uniform_fok_and_gtc_same_side) {
    Orders o;
    Order *bGtc = o.gtc(Side::BUY, 105, 30, 0);
    Order *bFok = o.fok(Side::BUY, 105, 50, 1);
    auto r = Uniform().clear({bGtc, bFok}, {o.gtc(Side::SELL, 100, 30, 0)});
    CHECK_EQ(totalQty(r.trades), 30);
    CHECK_EQ(tradedBy(r.trades, bGtc), 30);
    CHECK(!contains(r.unfilled, bFok));

    Order *fFirst = o.fok(Side::BUY, 105, 100, 0);
    Order *gAfter = o.gtc(Side::BUY, 105, 40, 1);
    r = Uniform().clear({fFirst, gAfter}, {o.gtc(Side::SELL, 100, 40, 0)});
    CHECK_EQ(tradedBy(r.trades, gAfter), 40);
    CHECK(!contains(r.unfilled, fFirst));

    Order *shortFok = o.fok(Side::BUY, 105, 20, 1);
    r = Uniform().clear({o.gtc(Side::BUY, 105, 30, 0), shortFok}, {o.gtc(Side::SELL, 100, 80, 0)});
    CHECK_EQ(totalQty(r.trades), 50);
    CHECK_EQ(tradedBy(r.trades, shortFok), 20);
}

TEST(uniform_fok_requirement_drives_clearing_price_up) {
    Orders o;
    const auto r = Uniform().clear({o.fok(Side::BUY, 100, 100)},
                                   {o.gtc(Side::SELL, 95, 50, 0), o.gtc(Side::SELL, 100, 70, 1)});
    CHECK(allAtPrice(r.trades, 100));
    CHECK_EQ(totalQty(r.trades), 100);
}

TEST(uniform_opposing_foks) {
    Orders o;
    Order *b = o.fok(Side::BUY, 105, 50, 0);
    Order *s = o.fok(Side::SELL, 100, 50, 1);
    auto r = Uniform().clear({b}, {s});
    CHECK_EQ(totalQty(r.trades), 50);
    CHECK(r.unfilled.empty());

    r = Uniform().clear({o.fok(Side::BUY, 105, 50, 0)}, {o.fok(Side::SELL, 100, 100, 1)});
    CHECK(r.trades.empty() && r.unfilled.empty());

    Order *bFok = o.fok(Side::BUY, 105, 50, 0);
    Order *sFok = o.fok(Side::SELL, 100, 100, 1);
    Order *sGtc = o.gtc(Side::SELL, 100, 50, 2);
    r = Uniform().clear({bFok}, {sFok, sGtc});
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].quantity, 50);
    CHECK_EQ(r.trades[0].buyOrderId, bFok->orderId);
    CHECK_EQ(r.trades[0].sellOrderId, sGtc->orderId);
    CHECK(!contains(r.unfilled, sFok));
}

TEST(uniform_chained_fok) {
    Orders o;
    {
        Order *a = o.fok(Side::BUY, 105, 4, 2);
        Order *c = o.gtc(Side::BUY, 105, 6, 3);
        Order *b = o.fok(Side::SELL, 100, 10, 1);
        const auto r = Uniform().clear({a, c}, {b});
        CHECK_EQ(totalQty(r.trades), 10);
        CHECK(r.unfilled.empty());
    }
    {
        Order *a = o.fok(Side::BUY, 105, 4, 2);
        Order *c = o.gtc(Side::BUY, 105, 5, 3);
        Order *b = o.fok(Side::SELL, 100, 10, 1);
        Order *d = o.gtc(Side::SELL, 100, 2, 0);
        const auto r = Uniform().clear({a, c}, {b, d});
        CHECK_EQ(tradedBy(r.trades, a), 0);
        CHECK(tradedBy(r.trades, c) > 0 && tradedBy(r.trades, d) > 0);
    }
}

TEST(uniform_failed_fok_branch_does_not_poison_the_next) {
    Orders o;
    Order *a = o.fok(Side::BUY, 100, 4, 0);
    Order *d = o.gtc(Side::BUY, 100, 5, 3);
    Order *b = o.fok(Side::SELL, 100, 10, 1);
    Order *c = o.fok(Side::SELL, 100, 6, 2);
    const auto r = Uniform().clear({a, d}, {b, c});
    CHECK_EQ(tradedBy(r.trades, a), 4);
    CHECK_EQ(tradedBy(r.trades, c), 6);
    CHECK_EQ(r.unfilled.size(), 1u);
    CHECK(r.unfilled[0] == d);
    CHECK_EQ(d->quantity, 3);
}

TEST(uniform_unfilled_are_the_callers_orders) {
    Orders o;
    Order *b = o.gtc(Side::BUY, 105, 50, 0);
    Order *s = o.gtc(Side::SELL, 100, 30, 1);
    const auto r = Uniform().clear({b}, {s});
    CHECK_EQ(r.unfilled.size(), 1u);
    CHECK(r.unfilled[0] == b);
    CHECK_EQ(b->quantity, 20);
    CHECK_EQ(s->quantity, 0);
}
