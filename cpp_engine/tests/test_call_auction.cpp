//
// Port of tests/test_call_auction.py, plus TestCallAuctionRoundTrip and
// TestDuplicateSubmission from tests/test_edge_cases.py.
//

#include "helpers.h"
#include "test_framework.h"

#include "call_auction/call_auction.h"
#include "call_auction/rules/discriminatory.h"
#include "call_auction/rules/uniform.h"

#include <stdexcept>
#include <utility>

namespace {
    struct NoopPricing : PricingRule {
        ClearResult clear(const std::vector<Order *> &, const std::vector<Order *> &) override { return {}; }
    };

    struct CapturePricing : PricingRule {
        std::vector<std::pair<std::vector<Order *>, std::vector<Order *>>> calls;

        ClearResult clear(const std::vector<Order *> &buys, const std::vector<Order *> &sells) override {
            calls.emplace_back(buys, sells);
            return {};
        }
    };

    struct FixedPricing : PricingRule {
        Trade dummy{100, 5, 1, 2};

        ClearResult clear(const std::vector<Order *> &, const std::vector<Order *> &) override {
            return ClearResult{{dummy}, {}};
        }
    };
}

TEST(call_auction_submit_collects_without_matching) {
    Orders o;
    NoopPricing pricing;
    CallAuction e(pricing);
    CHECK(e.submit(o.gtc(Side::SELL, 99, 10)).empty());
    CHECK(e.submit(o.gtc(Side::BUY, 105, 10)).empty());
    CHECK_EQ(e.getPending().size(), 2u);
    CHECK(e.trades.empty());
}

TEST(call_auction_run_clears_pending) {
    Orders o;
    NoopPricing pricing;
    CallAuction e(pricing);
    e.submit(o.gtc(Side::BUY, 100, 10));
    e.run();
    CHECK(e.getPending().empty());
}

TEST(call_auction_run_passes_correct_sides_to_pricing) {
    Orders o;
    CapturePricing pricing;
    CallAuction e(pricing);
    Order *b = o.gtc(Side::BUY, 100, 10);
    Order *s = o.gtc(Side::SELL, 99, 10);
    e.submit(b);
    e.submit(s);
    e.run();
    CHECK_EQ(pricing.calls.size(), 1u);
    CHECK(contains(pricing.calls[0].first, b));
    CHECK(contains(pricing.calls[0].second, s));
}

TEST(call_auction_run_returns_trades_from_pricing) {
    Orders o;
    FixedPricing pricing;
    CallAuction e(pricing);
    e.submit(o.gtc(Side::BUY, 100, 5));
    e.submit(o.gtc(Side::SELL, 99, 5));
    const auto result = e.run();
    CHECK_EQ(result.size(), 1u);
    CHECK_EQ(result[0].price, 100);
    CHECK_EQ(e.trades.size(), 1u);
}

TEST(call_auction_run_on_empty_book_and_repeated_runs) {
    Orders o;
    CapturePricing pricing;
    CallAuction e(pricing);
    CHECK(e.run().empty());
    e.submit(o.gtc(Side::BUY, 100, 5));
    e.run();
    e.submit(o.gtc(Side::SELL, 99, 5));
    e.run();
    CHECK_EQ(pricing.calls.size(), 3u);
    CHECK(e.getPending().empty());
}

TEST(call_auction_rejects_duplicate_pending_order) {
    Orders o;
    Uniform pricing;
    CallAuction e(pricing);
    Order *b = o.gtc(Side::BUY, 100, 5);
    e.submit(b);
    CHECK_THROWS(e.submit(b), std::invalid_argument);
}

TEST(call_auction_rejects_filled_order) {
    Orders o;
    Uniform pricing;
    CallAuction e(pricing);
    Order *b = o.gtc(Side::BUY, 105, 5, 0);
    e.submit(b);
    e.submit(o.gtc(Side::SELL, 100, 5, 1));
    e.run();
    CHECK_THROWS(e.submit(b), std::invalid_argument);
}

TEST(call_auction_rejects_market_order_for_discriminatory) {
    Orders o;
    Discriminatory pricing;
    CallAuction e(pricing);
    CHECK_THROWS(e.submit(o.mkt(Side::BUY, 5)), std::invalid_argument);
    CHECK(e.getPending().empty());
}

TEST(call_auction_uniform_rolls_original_with_updated_quantity) {
    Orders o;
    Uniform pricing;
    CallAuction e(pricing);
    Order *b = o.gtc(Side::BUY, 105, 50, 0);
    e.submit(b);
    e.submit(o.gtc(Side::SELL, 100, 30, 1));
    e.run();
    CHECK_EQ(e.getPending().size(), 1u);
    CHECK(e.getPending()[0] == b);
    CHECK_EQ(b->quantity, 20);
}

TEST(call_auction_uniform_remainder_trades_in_next_batch) {
    Orders o;
    Uniform pricing;
    CallAuction e(pricing);
    Order *b = o.gtc(Side::BUY, 105, 50, 0);
    e.submit(b);
    e.submit(o.gtc(Side::SELL, 100, 30, 1));
    e.run();
    e.submit(o.gtc(Side::SELL, 100, 20, 2));
    const auto trades = e.run();
    CHECK_EQ(totalQty(trades), 20);
    for (const Trade &t: trades) CHECK_EQ(t.buyOrderId, b->orderId);
    CHECK(e.getPending().empty());
}

TEST(call_auction_discriminatory_ioc_does_not_survive) {
    Orders o;
    Discriminatory pricing;
    CallAuction e(pricing);
    e.submit(o.gtc(Side::BUY, 105, 5, 0));
    e.submit(o.ioc(Side::BUY, 104, 5, 1));
    e.submit(o.gtc(Side::SELL, 100, 5, 2));
    e.run();
    CHECK(e.getPending().empty());
}
