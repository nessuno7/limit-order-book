//
// Tests for the backtester (backtest/): FastBook's passive replay mode, the
// feeds, and each rule that decides when a simulated order fills.
//

#include "test_framework.h"

#include "backtest/Backtester.h"
#include "backtest/DayCache.h"
#include "backtest/Feeds.h"
#include "backtest/Strategies.h"
#include "harness/op.h"

#include <filesystem>
#include <optional>
#include <sstream>
#include <vector>

using bt::EventType;
using bt::MarketEvent;

namespace {
    bt::Strategy idle; // the tests drive the backtester by hand

    bt::Config config(const bt::Nanos latency = 0) {
        bt::Config c;
        c.minPrice = 1;
        c.maxPrice = 1'000;
        c.maxOrders = 1'024;
        c.orderLatency = latency;
        c.markoutHorizon = 100;
        return c;
    }

    MarketEvent ev(const bt::Nanos t, const EventType type, const OrderId id, const Side side, const Price price,
                   const Quantity qty) {
        return {t, type, id, side, price, qty};
    }

    Snapshot snapshot(const fast::FastBook &b) {
        Snapshot s;
        for (const Side side: {Side::BUY, Side::SELL}) {
            b.forEachOrder(side, [&](const Price p, const OrderId id, const Quantity q) { s.push_back({side, p, id, q}); });
        }
        return s;
    }
}

// ---- FastBook passive mode ------------------------------------------------------

TEST(passive_rest_never_matches) {
    fast::FastBook b(1, 100, 16);
    b.restPassive(1, Side::SELL, 50, 5);
    b.restPassive(2, Side::BUY, 55, 3); // crosses, but a feed says it rested
    CHECK_EQ(*b.bestBid(), 55);
    CHECK_EQ(*b.bestAsk(), 50);
    CHECK_EQ(b.restingOrders(), 2u);
    CHECK_THROWS(b.restPassive(2, Side::BUY, 40, 1), std::invalid_argument);
    CHECK_THROWS(b.restPassive(3, Side::BUY, 101, 1), std::out_of_range);
    CHECK_THROWS(b.restPassive(3, Side::BUY, 40, 0), std::invalid_argument);
}

TEST(passive_reduce_keeps_priority_and_removes_at_zero) {
    fast::FastBook b(1, 100, 16);
    b.restPassive(1, Side::BUY, 50, 5);
    b.restPassive(2, Side::BUY, 50, 7);
    CHECK(b.reduce(1, 2));
    CHECK_EQ(b.levelQuantity(Side::BUY, 50), 10);
    CHECK((snapshot(b) == Snapshot{{Side::BUY, 50, 1, 3}, {Side::BUY, 50, 2, 7}}));
    CHECK(b.reduce(1, 3));
    CHECK(!b.contains(1));
    CHECK_EQ(b.levelQuantity(Side::BUY, 50), 7);
    CHECK(!b.reduce(1, 1));
    CHECK_THROWS(b.reduce(2, 8), std::invalid_argument);
    CHECK_THROWS(b.reduce(2, 0), std::invalid_argument);
    CHECK(b.reduce(2, 7));
    CHECK(!b.bestBid().has_value());
    CHECK_EQ(b.levelQuantity(Side::BUY, 50), 0);
    CHECK_EQ(b.levelQuantity(Side::BUY, 5'000), 0);
}

// ---- feeds -------------------------------------------------------------------

TEST(synthetic_feed_replays_to_the_book_that_produced_it) {
    // Replaying the feed passively must rebuild exactly the matching book that
    // generated it: same orders, same queue order, same quantities.
    for (int profile = 0; profile < 3; ++profile) {
        const FlowParams params = flowProfile(profile);
        const std::size_t ops = 20'000;
        const auto feed = bt::syntheticFeed(static_cast<std::uint64_t>(profile) + 11, ops, params);

        fast::FastBook matching(params.minPrice, params.maxPrice, ops + 16);
        OpGenerator gen(static_cast<std::uint64_t>(profile) + 11, params);
        for (std::size_t i = 0; i < ops; ++i) {
            const Op op = gen.next(matching);
            try {
                if (op.kind == OpKind::Add) matching.submit(op.id, op.side, op.price, op.quantity, op.tif);
                else if (op.kind == OpKind::Cancel) matching.cancel(op.id);
                else matching.modify(op.id, *op.price, op.quantity);
            } catch (const std::exception &) {} // rejected requests, as in the feed
        }

        fast::FastBook replay(params.minPrice, params.maxPrice, ops + 16);
        bt::Nanos last = 0;
        for (const MarketEvent &e: feed) {
            CHECK(e.time >= last);
            last = e.time;
            if (e.type == EventType::Add) replay.restPassive(e.id, e.side, e.price, e.quantity);
            else if (e.type == EventType::Delete) CHECK(replay.cancel(e.id));
            else CHECK(replay.reduce(e.id, e.quantity));
        }
        CHECK(snapshot(replay) == snapshot(matching));
    }
}

TEST(lobster_lines_parse) {
    const auto add = bt::parseLobsterLine("34200.004241176,1,16113575,18,5853300,1\r");
    CHECK(add.has_value());
    CHECK_EQ(add->time, 34'200'004'241'176);
    CHECK(add->type == EventType::Add);
    CHECK_EQ(add->id, 16113575u);
    CHECK_EQ(add->quantity, 18);
    CHECK_EQ(add->price, 5853300);
    CHECK(add->side == Side::BUY);

    const auto exec = bt::parseLobsterLine("34200.5,4,16113575,10,5853300,-1", 100);
    CHECK(exec.has_value());
    CHECK_EQ(exec->time, 34'200'500'000'000);
    CHECK(exec->type == EventType::Execute);
    CHECK(exec->side == Side::SELL);
    CHECK_EQ(exec->price, 58533);

    CHECK(!bt::parseLobsterLine("34200.5,5,0,10,5853300,1").has_value()); // hidden execution
    CHECK(!bt::parseLobsterLine("34200.5,1,7,10").has_value());
    CHECK(!bt::parseLobsterLine("34200.5,1,7,10,5853300,0").has_value());

    std::istringstream file("34200.1,1,1,5,100,1\n34200.2,7,0,0,-1,-1\n34200.3,3,1,5,100,1\n");
    CHECK_EQ(bt::readLobster(file).size(), 2u);
}

// ---- queue position and fills ------------------------------------------------------

TEST(backtest_joins_behind_resting_volume) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 6));
    b.step(ev(2, EventType::Add, 2, Side::BUY, 100, 4));
    const OrderId me = b.place(Side::BUY, 100, 5);
    b.step(ev(3, EventType::Add, 3, Side::BUY, 100, 8)); // behind us
    CHECK(b.order(me)->live);
    CHECK_EQ(b.order(me)->ahead, 10);

    b.step(ev(4, EventType::Execute, 1, Side::BUY, 100, 6)); // ahead of us: no fill
    CHECK_EQ(b.order(me)->ahead, 4);
    CHECK_EQ(b.order(me)->remaining, 5);
    b.step(ev(5, EventType::Delete, 3, Side::BUY, 100, 8)); // behind us: no change
    CHECK_EQ(b.order(me)->ahead, 4);
    b.step(ev(6, EventType::Cancel, 2, Side::BUY, 100, 1)); // ahead of us
    CHECK_EQ(b.order(me)->ahead, 3);
    CHECK_EQ(b.stats().fills, 0u);
}

TEST(backtest_fills_when_an_order_behind_us_executes) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::SELL, 105, 5));
    const OrderId me = b.place(Side::SELL, 105, 4);
    b.step(ev(2, EventType::Add, 2, Side::SELL, 105, 10));
    b.step(ev(3, EventType::Execute, 1, Side::SELL, 105, 5));
    CHECK_EQ(b.stats().fills, 0u);
    CHECK_EQ(b.order(me)->ahead, 0);
    b.step(ev(4, EventType::Execute, 2, Side::SELL, 105, 3)); // the aggressor reached past us
    CHECK_EQ(b.order(me)->remaining, 1);
    CHECK_EQ(b.stats().makerVolume, 3);
    CHECK_EQ(b.stats().position, -3);
    CHECK_EQ(b.stats().cash, 315);
    b.step(ev(5, EventType::Execute, 2, Side::SELL, 105, 7));
    CHECK(b.order(me) == nullptr);
    CHECK_EQ(b.stats().soldVolume, 4);
}

TEST(backtest_better_price_fills_on_execution_through_it) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 5));
    b.step(ev(2, EventType::Add, 2, Side::SELL, 103, 5));
    const OrderId me = b.place(Side::BUY, 101, 2); // improves the bid
    b.step(ev(3, EventType::Execute, 1, Side::BUY, 100, 5));
    CHECK(b.order(me) == nullptr);
    CHECK_EQ(b.stats().boughtVolume, 2);
    CHECK_EQ(b.stats().cash, -202);
}

TEST(backtest_orders_resting_before_the_feed_count_as_ahead) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 4));
    const OrderId me = b.place(Side::BUY, 100, 3);
    b.step(ev(2, EventType::Add, 2, Side::SELL, 110, 1));
    // #99 was never added: it rested before the feed began, so it was ahead.
    b.step(ev(3, EventType::Execute, 99, Side::BUY, 100, 2));
    CHECK_EQ(b.stats().fills, 0u);
    CHECK_EQ(b.stats().unknownOrders, 1u);
    CHECK_EQ(b.order(me)->remaining, 3);
}

TEST(backtest_crossing_add_trades_with_us) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 5));
    b.step(ev(2, EventType::Add, 2, Side::SELL, 104, 5));
    const OrderId me = b.place(Side::BUY, 102, 6);
    b.step(ev(3, EventType::Add, 3, Side::SELL, 101, 4)); // would have hit our 102 bid
    CHECK_EQ(b.order(me)->remaining, 2);
    CHECK_EQ(b.stats().cash, -408); // at our price, as the maker
    CHECK_EQ(b.stats().makerVolume, 4);
}

TEST(backtest_marketable_order_takes_then_rests) {
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::SELL, 101, 3));
    b.step(ev(2, EventType::Add, 2, Side::SELL, 102, 4));
    b.step(ev(3, EventType::Add, 3, Side::BUY, 99, 2));
    const OrderId me = b.place(Side::BUY, 102, 10);
    b.step(ev(4, EventType::Add, 4, Side::BUY, 98, 1));
    CHECK_EQ(b.stats().takerVolume, 7);
    CHECK_EQ(b.stats().cash, -(3 * 101 + 4 * 102));
    CHECK_EQ(b.order(me)->remaining, 3);
    CHECK_EQ(b.order(me)->ahead, 0);
}

TEST(backtest_latency_delays_arrival_and_cancel) {
    bt::Backtester b(config(100), idle);
    b.step(ev(0, EventType::Add, 1, Side::BUY, 100, 5));
    const OrderId me = b.place(Side::BUY, 100, 2);
    b.step(ev(50, EventType::Add, 2, Side::BUY, 100, 7)); // arrives before us
    CHECK(!b.order(me)->live);
    b.step(ev(150, EventType::Add, 3, Side::SELL, 120, 1));
    CHECK(b.order(me)->live);
    CHECK_EQ(b.order(me)->ahead, 12);

    CHECK(b.cancel(me));
    CHECK(!b.cancel(me)); // already in flight
    b.step(ev(160, EventType::Execute, 1, Side::BUY, 100, 5));
    b.step(ev(170, EventType::Execute, 2, Side::BUY, 100, 7));
    b.step(ev(180, EventType::Add, 4, Side::SELL, 100, 1)); // crosses us while the cancel is in flight
    CHECK_EQ(b.stats().boughtVolume, 1);
    b.step(ev(250, EventType::Add, 5, Side::BUY, 90, 1)); // the cancel lands
    CHECK(b.order(me) == nullptr);
    CHECK_EQ(b.stats().boughtVolume, 1);
}

TEST(backtest_markout_and_pnl) {
    bt::Config c = config();
    c.makerFee = -0.5; // rebate
    bt::Backtester b(c, idle);
    b.step(ev(0, EventType::Add, 1, Side::BUY, 100, 5));
    b.step(ev(0, EventType::Add, 2, Side::SELL, 102, 5));
    b.place(Side::BUY, 101, 2); // improves the bid, nothing ahead
    b.step(ev(1, EventType::Add, 3, Side::BUY, 99, 1));
    b.step(ev(10, EventType::Add, 4, Side::SELL, 101, 2)); // fills us at 101
    CHECK_EQ(b.stats().position, 2);
    CHECK_EQ(b.stats().markoutVolume, 0); // not due until t = 110
    b.step(ev(20, EventType::Delete, 4, Side::SELL, 101, 2));
    b.step(ev(20, EventType::Delete, 1, Side::BUY, 100, 5)); // bid 99, ask 102: mid 100.5
    b.step(ev(200, EventType::Add, 5, Side::BUY, 90, 1));    // past the horizon
    CHECK_EQ(b.stats().markoutVolume, 2);
    CHECK(b.stats().markout == -1.0); // (100.5 - 101) x 2: bought, then the price fell
    CHECK(b.stats().fees == -1.0);
    CHECK(b.stats().pnl(b.mid()) == -202.0 + 2 * 100.5 + 1.0);
}

TEST(backtest_crossing_add_trades_after_volume_ahead) {
    // Not something a consistent feed produces, but the queue must still hold.
    bt::Backtester b(config(), idle);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 5));
    const OrderId me = b.place(Side::BUY, 100, 4);
    b.step(ev(2, EventType::Add, 2, Side::SELL, 100, 7));
    CHECK_EQ(b.order(me)->remaining, 2);
}

TEST(backtest_quote_at_touch_on_synthetic_flow) {
    const FlowParams params = flowProfile(0);
    const auto feed = bt::syntheticFeed(5, 50'000, params);
    bt::Config c;
    c.minPrice = params.minPrice;
    c.maxPrice = params.maxPrice;
    c.orderLatency = 50'000;
    bt::QuoteAtTouch strategy(5, 50);
    bt::Backtester b(c, strategy);
    b.run(feed);
    const bt::Stats &s = b.stats();
    CHECK(s.fills > 0);
    CHECK_EQ(s.position, s.boughtVolume - s.soldVolume);
    CHECK_EQ(s.makerVolume + s.takerVolume, s.boughtVolume + s.soldVolume);
    // Orders still in flight (new quotes, cancelled ones not yet gone) can
    // overshoot the limit, but only by a few quotes.
    CHECK(s.maxAbsPosition <= 50 + 4 * 5);
    CHECK_EQ(s.markoutVolume, s.boughtVolume + s.soldVolume);
    CHECK_EQ(s.unknownOrders, 0u);
}

// ---- LOBSTER snapshots and placeholder volume ----------------------------------------

TEST(lobster_book_line_parses_and_skips_empty_levels) {
    const auto t = bt::parseLobsterBookLine("2239500,100,2231800,50,9999999999,0,-9999999999,0\r", 100);
    CHECK(t.has_value());
    CHECK((t->asks == std::vector<std::pair<Price, Quantity>>{{22395, 100}}));
    CHECK((t->bids == std::vector<std::pair<Price, Quantity>>{{22318, 50}}));
    CHECK(!bt::parseLobsterBookLine("2239500,100,2231800").has_value());
}

TEST(lobster_day_initial_book_undoes_the_first_message) {
    std::istringstream messages("34200.1,1,7,30,1000,1\n34200.2,4,7,10,1000,1\n");
    std::istringstream books("1010,5,1000,30\n1010,5,1000,20\n");
    const bt::LobsterDay day = bt::loadLobster(messages, books);
    CHECK_EQ(day.messages.size(), 2u);
    CHECK(day.initial.bids.empty()); // the add of #7 created the only bid level
    CHECK((day.initial.asks == std::vector<std::pair<Price, Quantity>>{{1010, 5}}));
    CHECK_EQ(day.minPrice, 1000);
    CHECK_EQ(day.maxPrice, 1010);

    std::istringstream shortBooks("1010,5,1000,30\n");
    std::istringstream again("34200.1,1,7,30,1000,1\n34200.2,4,7,10,1000,1\n");
    CHECK_THROWS(bt::loadLobster(again, shortBooks), std::runtime_error);
}

TEST(replay_book_placeholders_absorb_unseen_orders) {
    bt::ReplayBook b(1, 1'000, 64);
    b.resyncTo({{{100, 50}, {99, 20}}, {{101, 30}}}); // seeded: nothing the feed introduced
    b.apply(ev(1, EventType::Add, 1, Side::BUY, 100, 5));
    CHECK((b.top(5).bids == std::vector<std::pair<Price, Quantity>>{{100, 55}, {99, 20}}));

    CHECK(b.apply(ev(2, EventType::Execute, 77, Side::BUY, 100, 10)) == bt::ReplayBook::Applied::Placeholder);
    CHECK(b.apply(ev(3, EventType::Delete, 78, Side::SELL, 101, 30)) == bt::ReplayBook::Applied::Placeholder);
    CHECK(b.apply(ev(4, EventType::Delete, 79, Side::SELL, 105, 3)) == bt::ReplayBook::Applied::Unexplained);
    CHECK(b.apply(ev(5, EventType::Execute, 1, Side::BUY, 100, 5)) == bt::ReplayBook::Applied::Known);
    const bt::TopOfBook t = b.top(5);
    CHECK((t.bids == std::vector<std::pair<Price, Quantity>>{{100, 40}, {99, 20}}));
    CHECK(t.asks.empty());
    CHECK_EQ(b.stats().placeholderHits, 2u);
    CHECK_EQ(b.stats().unexplained, 1u);
}

TEST(replay_book_resync_matches_the_snapshot) {
    bt::ReplayBook b(1, 1'000, 64);
    b.apply(ev(1, EventType::Add, 1, Side::SELL, 102, 10));
    b.apply(ev(1, EventType::Add, 2, Side::SELL, 103, 10));
    b.resyncTo({{{100, 50}}, {{101, 7}, {102, 15}, {103, 10}}});
    CHECK((b.top(5) == bt::TopOfBook{{{100, 50}}, {{101, 7}, {102, 15}, {103, 10}}}));
    b.resyncTo({{{100, 20}}, {{102, 10}, {103, 10}}}); // 101 vanished, 102 back to the known order
    CHECK((b.top(5) == bt::TopOfBook{{{100, 20}}, {{102, 10}, {103, 10}}}));
    CHECK(b.fastBook().contains(1));
}

TEST(backtest_seeded_levels_count_as_ahead) {
    bt::Backtester b(config(), idle);
    b.seed({{{100, 40}}, {{105, 10}}});
    const OrderId me = b.place(Side::BUY, 100, 5);
    b.step(ev(1, EventType::Add, 1, Side::BUY, 100, 3));
    CHECK_EQ(b.order(me)->ahead, 40);
    b.step(ev(2, EventType::Execute, 50, Side::BUY, 100, 30)); // an order from before the feed
    CHECK_EQ(b.order(me)->ahead, 10);
    CHECK_EQ(b.stats().fills, 0u);
    CHECK_EQ(b.levelQuantity(Side::BUY, 100), 13);
}

TEST(replay_book_resync_drops_orders_that_left_out_of_view) {
    // #1 left while its level was hidden, so the feed never said so; #2 was
    // added later and is still there. The snapshot shows only #2's volume.
    bt::ReplayBook b(1, 1'000, 64);
    b.apply(ev(1, EventType::Add, 1, Side::SELL, 102, 100));
    b.apply(ev(2, EventType::Add, 2, Side::SELL, 102, 30));
    b.resyncTo({{}, {{102, 30}}});
    CHECK(!b.fastBook().contains(1));
    CHECK(b.fastBook().contains(2));
    CHECK_EQ(b.stats().droppedKnown, 100);
}

TEST(replay_book_folds_levels_that_leave_view) {
    // Two-level snapshots. #2 at 102 drops out of view when 100 arrives, is
    // cancelled while hidden (the feed never says so), and the level comes
    // back with other volume. Only placeholder volume is corrected.
    bt::ReplayBook b(1, 1'000, 64, 2);
    b.apply(ev(1, EventType::Add, 1, Side::SELL, 101, 10));
    b.apply(ev(1, EventType::Add, 2, Side::SELL, 102, 20));
    b.resyncTo({{}, {{101, 10}, {102, 20}}});
    b.apply(ev(2, EventType::Add, 3, Side::SELL, 100, 5));
    b.resyncTo({{}, {{100, 5}, {101, 10}}});
    CHECK(!b.fastBook().contains(2));
    CHECK_EQ(b.stats().foldedOrders, 1u);
    CHECK_EQ(b.fastBook().levelQuantity(Side::SELL, 102), 20); // still there, as placeholder volume

    b.apply(ev(3, EventType::Delete, 3, Side::SELL, 100, 5));
    b.resyncTo({{}, {{101, 10}, {102, 8}}});
    CHECK((b.top(5).asks == std::vector<std::pair<Price, Quantity>>{{101, 10}, {102, 8}}));
    CHECK_EQ(b.stats().droppedKnown, 0);
    CHECK(b.fastBook().contains(1));
}

// ---- compiled days (backtest/DayCache.h) ------------------------------------------

namespace {
    // A LOBSTER-like day from the synthetic feed: snapshots show the top
    // `depth` levels of the true book, and every 7th add is withheld (its
    // later cancels and executions are not), so re-syncs have placeholder
    // volume to create, shrink and fold.
    bt::LobsterDay hiddenAddDay(const std::uint64_t seed, const std::size_t depth) {
        const FlowParams params = flowProfile(0);
        const std::vector<MarketEvent> feed = bt::syntheticFeed(seed, 20'000, params);
        bt::ReplayBook truth(params.minPrice, params.maxPrice, 1'024);
        bt::LobsterDay day;
        day.depth = depth;
        day.minPrice = params.minPrice + 1; // Backtester's book spans minPrice - 1 .. maxPrice + 1
        day.maxPrice = params.maxPrice - 1;
        std::size_t adds = 0;
        for (const MarketEvent &e: feed) {
            truth.apply(e);
            const bool hidden = e.type == EventType::Add && ++adds % 7 == 0;
            day.messages.push_back(hidden ? std::nullopt : std::optional<MarketEvent>(e));
            day.books.push_back(truth.top(depth));
        }
        return day;
    }
}

TEST(compiled_day_replays_exactly_like_online_resync) {
    for (const std::uint64_t seed: {1u, 2u, 3u}) {
        const bt::LobsterDay day = hiddenAddDay(seed, 3);
        const bt::CompiledDay compiled = bt::compileDay(day);
        CHECK(!compiled.fixes.empty());

        bt::ReplayBook online(day.minPrice - 1, day.maxPrice + 1, 64, day.depth);
        bt::ReplayBook replayed(day.minPrice - 1, day.maxPrice + 1, 64, day.depth);
        online.resyncTo(day.initial);
        replayed.replay(compiled.seedFixes());
        std::size_t k = 0;
        for (std::size_t i = 0; i < day.messages.size(); ++i) {
            if (!day.messages[i]) continue;
            CHECK(compiled.events[k] == *day.messages[i]);
            online.apply(*day.messages[i]);
            replayed.apply(compiled.events[k]);
            CHECK_EQ(online.resyncTo(day.books[i]), replayed.replay(compiled.fixesAfter(k)));
            ++k;
            if (k % 500 == 0) CHECK(snapshot(online.fastBook()) == snapshot(replayed.fastBook()));
        }
        CHECK_EQ(k, compiled.events.size());
        CHECK(snapshot(online.fastBook()) == snapshot(replayed.fastBook()));
        CHECK_EQ(online.stats().resyncedVolume, replayed.stats().resyncedVolume);
        CHECK_EQ(online.stats().foldedOrders, replayed.stats().foldedOrders);
        CHECK(online.stats().foldedOrders > 0);

        // And through the backtester, with a strategy that trades.
        bt::Config c = config(50'000);
        c.minPrice = day.minPrice - 1;
        c.maxPrice = day.maxPrice + 1;
        c.snapshotDepth = day.depth;
        bt::QuoteAtTouch s1(10, 100), s2(10, 100);
        bt::Backtester a(c, s1), b(c, s2);
        a.seed(day.initial);
        b.seed(compiled.seedFixes());
        for (std::size_t i = 0; i < day.messages.size(); ++i) {
            if (day.messages[i]) a.step(*day.messages[i], &day.books[i]);
        }
        for (std::size_t i = 0; i < compiled.events.size(); ++i) b.step(compiled.events[i], compiled.fixesAfter(i));
        a.finish();
        b.finish();
        CHECK(a.stats().fills > 0);
        CHECK_EQ(a.stats().fills, b.stats().fills);
        CHECK_EQ(a.stats().cash, b.stats().cash);
        CHECK_EQ(a.stats().position, b.stats().position);
        CHECK_EQ(a.stats().markout, b.stats().markout);
    }
}

TEST(day_cache_round_trips_and_rejects_stale_or_damaged_files) {
    const bt::CompiledDay day = bt::compileDay(hiddenAddDay(4, 2));
    const std::string path = (std::filesystem::temp_directory_path() / "lob_test_day.lobday").string();
    const bt::DaySource source{100, 1, 200, 2, 100};
    bt::saveDay(day, source, path);

    const auto back = bt::loadDay(path, source);
    CHECK(back.has_value());
    CHECK(back->events == day.events);
    CHECK(back->fixes == day.fixes);
    CHECK(back->fixEnd == day.fixEnd);
    CHECK_EQ(back->rows, day.rows);
    CHECK_EQ(back->depth, day.depth);
    CHECK_EQ(back->minPrice, day.minPrice);

    bt::DaySource touched = source;
    touched.bookTime = 3; // the orderbook file changed since
    CHECK(!bt::loadDay(path, touched).has_value());
    bt::DaySource otherDivisor = source;
    otherDivisor.priceDivisor = 1;
    CHECK(!bt::loadDay(path, otherDivisor).has_value());

    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 5);
    CHECK(!bt::loadDay(path, source).has_value());
    std::filesystem::remove(path);
    CHECK(!bt::loadDay(path, source).has_value());
}
