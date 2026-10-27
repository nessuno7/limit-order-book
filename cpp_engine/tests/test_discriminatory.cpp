//
// Port of tests/test_fok_discriminatory.py, plus the discriminatory cases of
// tests/test_edge_cases.py.
//

#include "helpers.h"
#include "test_framework.h"

#include "call_auction/rules/discriminatory.h"

#include <stdexcept>

namespace {
    Quantity tradedBy(const std::vector<Trade> &trades, const Order *o) {
        Quantity q = 0;
        for (const Trade &t: trades) {
            if (t.buyOrderId == o->orderId || t.sellOrderId == o->orderId) q += t.quantity;
        }
        return q;
    }
}

// ---- baseline --------------------------------------------------------------

TEST(discriminatory_fok_baseline) {
    Orders o;
    {
        const auto r = Discriminatory().clear({o.fok(Side::BUY, 105, 5)}, {o.gtc(Side::SELL, 100, 5)});
        CHECK_EQ(r.trades.size(), 1u);
        CHECK_EQ(r.trades[0].quantity, 5);
    }
    CHECK(Discriminatory().clear({o.fok(Side::BUY, 105, 5)}, {o.gtc(Side::SELL, 100, 3)}).trades.empty());
    {
        const auto r = Discriminatory().clear({o.gtc(Side::BUY, 105, 5)}, {o.fok(Side::SELL, 100, 5)});
        CHECK_EQ(r.trades.size(), 1u);
        CHECK_EQ(r.trades[0].quantity, 5);
    }
    CHECK(Discriminatory().clear({o.gtc(Side::BUY, 105, 3)}, {o.fok(Side::SELL, 100, 5)}).trades.empty());
}

// ---- chained FOKs ----------------------------------------------------------

TEST(discriminatory_fok_buy_uses_fok_sell_that_can_fill) {
    Orders o;
    Order *a = o.fok(Side::BUY, 105, 4, 2);
    Order *b = o.fok(Side::SELL, 100, 10, 1);
    Order *c = o.gtc(Side::BUY, 105, 6, 3);
    const auto r = Discriminatory().clear({a, c}, {b});
    CHECK_EQ(totalQty(r.trades), 10);
    CHECK(r.unfilled.empty());
}

TEST(discriminatory_fok_sell_uses_fok_buy_that_can_fill) {
    Orders o;
    Order *a = o.fok(Side::SELL, 100, 4, 2);
    Order *b = o.fok(Side::BUY, 105, 10, 1);
    Order *c = o.gtc(Side::SELL, 100, 6, 3);
    const auto r = Discriminatory().clear({b}, {a, c});
    CHECK_EQ(totalQty(r.trades), 10);
    CHECK(r.unfilled.empty());
}

TEST(discriminatory_fok_rejected_when_chained_fok_cannot_fill) {
    Orders o;
    Order *a = o.fok(Side::BUY, 105, 4, 2);
    Order *b = o.fok(Side::SELL, 100, 10, 1);
    Order *d = o.gtc(Side::SELL, 100, 2, 0);
    Order *c = o.gtc(Side::BUY, 105, 5, 3);
    const auto r = Discriminatory().clear({a, c}, {d, b});
    CHECK_EQ(tradedBy(r.trades, a), 0);
    CHECK(totalQty(r.trades) > 0); // the GTCs still cross
}

TEST(discriminatory_fok_sell_rejected_when_chained_fok_cannot_fill) {
    Orders o;
    Order *a = o.fok(Side::SELL, 100, 4, 2);
    Order *b = o.fok(Side::BUY, 105, 10, 1);
    Order *d = o.gtc(Side::BUY, 105, 2, 0);
    Order *c = o.gtc(Side::SELL, 100, 5, 3);
    const auto r = Discriminatory().clear({b, d}, {a, c});
    CHECK_EQ(tradedBy(r.trades, a), 0);
}

TEST(discriminatory_failed_fok_branch_does_not_poison_the_next) {
    Orders o;
    Order *a = o.fok(Side::BUY, 100, 4, 0);
    Order *d = o.gtc(Side::BUY, 100, 5, 3);
    Order *b = o.fok(Side::SELL, 100, 10, 1);
    Order *c = o.fok(Side::SELL, 100, 6, 2);
    const auto r = Discriminatory().clear({a, d}, {b, c});
    CHECK_EQ(tradedBy(r.trades, a), 4);
    CHECK_EQ(tradedBy(r.trades, c), 6);
    CHECK_EQ(tradedBy(r.trades, b), 0);
    CHECK_EQ(r.unfilled.size(), 1u);
    CHECK(r.unfilled[0] == d);
    CHECK_EQ(d->quantity, 3);
}

// ---- time in force and validation -----------------------------------------

TEST(discriminatory_ioc_left_in_queue_is_not_rolled_forward) {
    Orders o;
    Order *b1 = o.gtc(Side::BUY, 105, 5, 0);
    Order *b2 = o.ioc(Side::BUY, 104, 5, 1);
    Order *s = o.gtc(Side::SELL, 100, 5, 2);
    const auto r = Discriminatory().clear({b1, b2}, {s});
    CHECK_EQ(totalQty(r.trades), 5);
    CHECK(!contains(r.unfilled, b2));
}

TEST(discriminatory_non_crossing_ioc_and_fok_are_dropped) {
    Orders o;
    Order *bGtc = o.gtc(Side::BUY, 88, 5, 2);
    Order *sGtc = o.gtc(Side::SELL, 111, 5, 4);
    const auto r = Discriminatory().clear(
            {o.ioc(Side::BUY, 90, 5, 0), o.fok(Side::BUY, 89, 5, 1), bGtc},
            {o.ioc(Side::SELL, 110, 5, 3), sGtc});
    CHECK(r.trades.empty());
    CHECK_EQ(r.unfilled.size(), 2u);
    CHECK(contains(r.unfilled, bGtc) && contains(r.unfilled, sGtc));
}

TEST(discriminatory_market_order_rejected_cleanly) {
    Orders o;
    CHECK_THROWS(Discriminatory().clear({o.mkt(Side::BUY, 5)}, {o.gtc(Side::SELL, 100, 5)}),
                 std::invalid_argument);
}

TEST(discriminatory_price_is_older_orders_limit) {
    Orders o;
    // Distinct timestamps: the older sell is the maker.
    auto r = Discriminatory().clear({o.gtc(Side::BUY, 105, 5, 1)}, {o.gtc(Side::SELL, 100, 5, 0)});
    CHECK_EQ(r.trades[0].price, 100);
    // Equal timestamps: the buy was created first, so it is the maker.
    Order *b = o.gtc(Side::BUY, 105, 5, 0);
    Order *s = o.gtc(Side::SELL, 100, 5, 0);
    r = Discriminatory().clear({b}, {s});
    CHECK_EQ(r.trades[0].price, 105);
}
