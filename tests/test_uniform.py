"""
Tests for the Uniform call-auction pricing rule.

The key properties under test:
  - p* maximises min(cumulative_demand, cumulative_supply)
  - ties in p* break toward the highest price
  - all trades execute at p*, regardless of each order's limit
  - the long side is rationed by time priority (oldest fills first)
  - GTC leftovers roll to unfilled; IOC leftovers are dropped
"""
from datetime import datetime, timedelta

from python_engine.call_auction.rules.uniform import Uniform
from python_engine.orderbook.order import Order, Side, TimeInForce

TRADER = 1
T0 = datetime(2026, 1, 1, 9, 0, 0)


def gtc(side, price, qty, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.GTC,
                 timestamp=T0 + timedelta(seconds=t))


def ioc(side, price, qty, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.IOC,
                 timestamp=T0 + timedelta(seconds=t))


def mkt(side, qty, t=0):
    return Order(side=side, price=None, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.GTC,
                 timestamp=T0 + timedelta(seconds=t))


def fok(side, price, qty, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.FOK,
                 timestamp=T0 + timedelta(seconds=t))


class TestNoCross:

    def test_empty_buys_no_trades_gtc_sells_returned(self):
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([], [s])
        assert trades == []
        assert s in unfilled

    def test_empty_sells_no_trades_gtc_buys_returned(self):
        b = gtc(Side.BUY, 100, 50)
        trades, unfilled = Uniform().clear([b], [])
        assert trades == []
        assert b in unfilled

    def test_empty_both_returns_empty(self):
        trades, unfilled = Uniform().clear([], [])
        assert trades == []
        assert unfilled == []

    def test_no_price_overlap_no_trades(self):
        # best buy (95) < best sell (100) — spreads never cross
        trades, _ = Uniform().clear(
            [gtc(Side.BUY, 95, 50)],
            [gtc(Side.SELL, 100, 50)],
        )
        assert trades == []

    def test_no_price_overlap_gtc_orders_rolled_forward(self):
        b = gtc(Side.BUY, 95, 50)
        s = gtc(Side.SELL, 100, 50)
        _, unfilled = Uniform().clear([b], [s])
        assert b in unfilled
        assert s in unfilled

    def test_no_price_overlap_ioc_orders_dropped(self):
        _, unfilled = Uniform().clear(
            [ioc(Side.BUY, 95, 50)],
            [ioc(Side.SELL, 100, 50)],
        )
        assert unfilled == []


class TestClearingPrice:

    def test_tied_prices_highest_chosen(self):
        # buy@105 and sell@95: p=95 and p=105 both give tradeable=50.
        # Highest tied price wins → p*=105.
        b = gtc(Side.BUY, 105, 50)
        s = gtc(Side.SELL, 95, 50)
        trades, _ = Uniform().clear([b], [s])
        assert len(trades) == 1
        assert trades[0].price == 105

    def test_clearing_price_maximises_tradeable_volume(self):
        # Candidate prices and their tradeable volumes:
        #   p=100: demand=100 (both buys >= 100), supply=80 (s1 only) → 80
        #   p=102: demand=100 (both buys >= 102), supply=80 (s1 only)  → 80  ← tied winner
        #   p=103: demand=50  (b1 only),          supply=140            → 50
        #   p=105: demand=50  (b1 only),          supply=140            → 50
        # Tie at 100 and 102 → p*=102 (highest tied).
        b1 = gtc(Side.BUY, 105, 50, t=0)
        b2 = gtc(Side.BUY, 102, 50, t=1)
        s1 = gtc(Side.SELL, 100, 80, t=0)
        s2 = gtc(Side.SELL, 103, 60, t=1)
        trades, _ = Uniform().clear([b1, b2], [s1, s2])
        assert all(t.price == 102 for t in trades)
        assert sum(t.quantity for t in trades) == 80

    def test_three_way_tie_minimum_surplus_chosen(self):
        # buy@105 qty 30, sell@95 qty 30, sell@100 qty 30
        #   p=95:  demand=30, supply=30,  surplus=0  ← minimum surplus → p*=95
        #   p=100: demand=30, supply=60,  surplus=30
        #   p=105: demand=30, supply=60,  surplus=30
        b = gtc(Side.BUY, 105, 30)
        s1 = gtc(Side.SELL, 95, 30)
        s2 = gtc(Side.SELL, 100, 30)
        trades, _ = Uniform().clear([b], [s1, s2])
        assert all(t.price == 95 for t in trades)


class TestTradeGeneration:

    def test_single_exact_match_produces_one_trade(self):
        b = gtc(Side.BUY, 105, 50)
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert len(trades) == 1
        assert trades[0].quantity == 50
        assert unfilled == []

    def test_trade_order_ids_match_source_orders(self):
        b = gtc(Side.BUY, 105, 50)
        s = gtc(Side.SELL, 100, 50)
        trades, _ = Uniform().clear([b], [s])
        assert trades[0].buy_order_id == b.order_id
        assert trades[0].sell_order_id == s.order_id

    def test_one_buy_splits_across_two_sells(self):
        # buy@105 qty 100; sell@98 qty 60 (older), sell@102 qty 80 (newer)
        # p*=105, tradeable=100. Sell side long (140 > 100).
        # sell1 fills 60, sell2 fills 40 — two separate trade records.
        b  = gtc(Side.BUY,  105, 100, t=0)
        s1 = gtc(Side.SELL,  98,  60, t=1)
        s2 = gtc(Side.SELL, 102,  80, t=2)
        trades, _ = Uniform().clear([b], [s1, s2])
        assert len(trades) == 2
        assert trades[0].sell_order_id == s1.order_id
        assert trades[0].quantity == 60
        assert trades[1].sell_order_id == s2.order_id
        assert trades[1].quantity == 40

    def test_all_trades_execute_at_clearing_price_not_limit_prices(self):
        # Two sells at different limits (98 and 102), one buy at 105.
        # Uniform pricing: all trades at the same p*, not at each order's limit.
        b  = gtc(Side.BUY,  105, 100, t=0)
        s1 = gtc(Side.SELL,  98,  60, t=1)
        s2 = gtc(Side.SELL, 102,  40, t=2)
        trades, _ = Uniform().clear([b], [s1, s2])
        assert len(trades) == 2
        assert len({t.price for t in trades}) == 1  # single clearing price

    def test_total_traded_quantity_equals_tradeable_volume(self):
        # buy 100@105, sell 60@100 → tradeable = min(100, 60) = 60
        b = gtc(Side.BUY, 105, 100)
        s = gtc(Side.SELL, 100, 60)
        trades, _ = Uniform().clear([b], [s])
        assert sum(t.quantity for t in trades) == 60

    def test_ineligible_orders_do_not_contribute_to_volume(self):
        # b2@90 cannot meet p*=105 — only b1 trades (50 units, not 100)
        b1 = gtc(Side.BUY, 105, 50)
        b2 = gtc(Side.BUY,  90, 50)
        s  = gtc(Side.SELL, 100, 50)
        trades, _ = Uniform().clear([b1, b2], [s])
        assert sum(t.quantity for t in trades) == 50


class TestRationing:

    def test_buy_side_long_older_buy_fills_first(self):
        # sell qty=80; buy1 qty=60 (older, t=1); buy2 qty=50 (newer, t=2)
        # tradeable=80. buy1 fills 60, buy2 fills 20.
        b1 = gtc(Side.BUY, 105, 60, t=1)
        b2 = gtc(Side.BUY, 105, 50, t=2)
        s  = gtc(Side.SELL, 100, 80, t=0)
        trades, _ = Uniform().clear([b1, b2], [s])
        b1_qty = sum(t.quantity for t in trades if t.buy_order_id == b1.order_id)
        b2_qty = sum(t.quantity for t in trades if t.buy_order_id == b2.order_id)
        assert b1_qty == 60
        assert b2_qty == 20

    def test_sell_side_long_older_sell_fills_first(self):
        # buy qty=80; sell1 qty=60 (older, t=1); sell2 qty=50 (newer, t=2)
        # tradeable=80. sell1 fills 60, sell2 fills 20.
        s1 = gtc(Side.SELL, 100, 60, t=1)
        s2 = gtc(Side.SELL, 100, 50, t=2)
        b  = gtc(Side.BUY, 105, 80, t=0)
        trades, _ = Uniform().clear([b], [s1, s2])
        s1_qty = sum(t.quantity for t in trades if t.sell_order_id == s1.order_id)
        s2_qty = sum(t.quantity for t in trades if t.sell_order_id == s2.order_id)
        assert s1_qty == 60
        assert s2_qty == 20

    def test_partially_rationed_order_quantity_updated_in_unfilled(self):
        # b2 fills 20 of 50 → remaining 30 returned with updated quantity
        b1 = gtc(Side.BUY, 105, 60, t=1)
        b2 = gtc(Side.BUY, 105, 50, t=2)
        s  = gtc(Side.SELL, 100, 80, t=0)
        _, unfilled = Uniform().clear([b1, b2], [s])
        b2_unfilled = next(o for o in unfilled if o.order_id == b2.order_id)
        assert b2_unfilled.quantity == 30

    def test_fully_rationed_order_returned_in_unfilled(self):
        # b1 alone fills all sell volume → b2 gets nothing but is still GTC
        b1 = gtc(Side.BUY, 105, 80, t=1)
        b2 = gtc(Side.BUY, 105, 50, t=2)
        s  = gtc(Side.SELL, 100, 80, t=0)
        _, unfilled = Uniform().clear([b1, b2], [s])
        assert any(o.order_id == b2.order_id for o in unfilled)


class TestTifHandling:

    def test_ineligible_gtc_order_rolled_to_unfilled(self):
        # b_low@90 cannot cross at p*=105 — ineligible, GTC → rolled forward
        b_hi  = gtc(Side.BUY, 105, 50)
        b_low = gtc(Side.BUY,  90, 30)
        s     = gtc(Side.SELL, 100, 50)
        _, unfilled = Uniform().clear([b_hi, b_low], [s])
        assert any(o.order_id == b_low.order_id for o in unfilled)

    def test_ineligible_ioc_order_dropped(self):
        b_hi  = gtc(Side.BUY, 105, 50)
        b_low = ioc(Side.BUY,  90, 30)
        s     = gtc(Side.SELL, 100, 50)
        _, unfilled = Uniform().clear([b_hi, b_low], [s])
        assert not any(o.order_id == b_low.order_id for o in unfilled)

    def test_ineligible_sell_gtc_rolled_to_unfilled(self):
        # s_high@110 cannot cross at p*=105 — ineligible, GTC → rolled forward
        b      = gtc(Side.BUY, 105, 50)
        s_lo   = gtc(Side.SELL, 100, 50)
        s_high = gtc(Side.SELL, 110, 30)
        _, unfilled = Uniform().clear([b], [s_lo, s_high])
        assert any(o.order_id == s_high.order_id for o in unfilled)

    def test_rationed_gtc_order_in_unfilled_with_updated_quantity(self):
        # b2 eligible but partially rationed (GTC) → returned with remainder
        b1 = gtc(Side.BUY, 105, 60, t=1)
        b2 = gtc(Side.BUY, 105, 50, t=2)
        s  = gtc(Side.SELL, 100, 80, t=0)
        _, unfilled = Uniform().clear([b1, b2], [s])
        b2_u = next(o for o in unfilled if o.order_id == b2.order_id)
        assert b2_u.quantity == 30  # 50 - 20 filled

    def test_rationed_ioc_order_dropped(self):
        # b2 eligible but IOC and partially rationed → silently dropped
        b1 = gtc(Side.BUY, 105, 60, t=1)
        b2 = ioc(Side.BUY, 105, 50, t=2)
        s  = gtc(Side.SELL, 100, 80, t=0)
        _, unfilled = Uniform().clear([b1, b2], [s])
        assert not any(o.order_id == b2.order_id for o in unfilled)

    def test_fully_filled_orders_not_in_unfilled(self):
        b = gtc(Side.BUY, 105, 50)
        s = gtc(Side.SELL, 100, 50)
        _, unfilled = Uniform().clear([b], [s])
        assert unfilled == []

    def test_empty_buys_ioc_sell_excluded_from_unfilled(self):
        s = ioc(Side.SELL, 100, 50)
        _, unfilled = Uniform().clear([], [s])
        assert unfilled == []


class TestMarketOrders:

    def test_market_buy_trades_with_limit_sell(self):
        b = mkt(Side.BUY, 50)
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert len(trades) == 1
        assert trades[0].quantity == 50
        assert unfilled == []

    def test_market_sell_trades_with_limit_buy(self):
        b = gtc(Side.BUY, 105, 50)
        s = mkt(Side.SELL, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert len(trades) == 1
        assert trades[0].quantity == 50
        assert unfilled == []

    def test_market_order_clears_at_limit_order_price_level(self):
        # The only candidate price is the limit order's price → p* = that price
        b = mkt(Side.BUY, 50)
        s = gtc(Side.SELL, 100, 50)
        trades, _ = Uniform().clear([b], [s])
        assert trades[0].price == 100

    def test_all_market_orders_no_price_discovery_no_trades(self):
        # No limit prices → no candidate prices → no trades
        b = mkt(Side.BUY, 50)
        s = mkt(Side.SELL, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades == []
        assert b in unfilled
        assert s in unfilled


class TestFOKUniform:
    """
    FOK in a call auction: the order must fill completely within the batch
    simulation at the chosen price level, or it is cancelled entirely.

    Key properties:
      - A FOK that cannot fill is silently dropped (never in unfilled)
      - A FOK that fills appears in trades like any other order
      - A failed FOK does not block GTC orders on the same side
      - The presence of a FOK can shift which price level maximises volume,
        because levels where it cannot fill record zero contribution from it
      - Opposing FOKs check each other recursively; if neither can fill, both drop
    """

    # ------------------------------------------------------------------
    # Basic fill / fail
    # ------------------------------------------------------------------

    def test_fok_buy_fills_fully(self):
        # FOK buy qty exactly met by sell side → one trade, FOK not in unfilled
        b = fok(Side.BUY, 105, 50)
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id == b.order_id for t in trades)
        assert not any(o.order_id == b.order_id for o in unfilled)

    def test_fok_sell_fills_fully(self):
        b = gtc(Side.BUY, 105, 50)
        s = fok(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.sell_order_id == s.order_id for t in trades)
        assert not any(o.order_id == s.order_id for o in unfilled)

    def test_fok_buy_fails_insufficient_volume(self):
        # FOK needs 100, only 50 available → no trades, FOK dropped, GTC sell kept
        b = fok(Side.BUY, 105, 100)
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades == []
        assert not any(o.order_id == b.order_id for o in unfilled)
        assert any(o.order_id == s.order_id for o in unfilled)

    def test_fok_sell_fails_insufficient_volume(self):
        b = gtc(Side.BUY, 105, 50)
        s = fok(Side.SELL, 100, 100)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades == []
        assert not any(o.order_id == s.order_id for o in unfilled)
        assert any(o.order_id == b.order_id for o in unfilled)

    def test_failed_fok_is_never_in_unfilled(self):
        # Explicit: a FOK that cannot fill is cancelled, not rolled to next batch
        b = fok(Side.BUY, 105, 100)
        s = gtc(Side.SELL, 100, 30)
        _, unfilled = Uniform().clear([b], [s])
        assert not any(o.order_id == b.order_id for o in unfilled)

    # ------------------------------------------------------------------
    # FOK spanning multiple opposing orders
    # ------------------------------------------------------------------

    def test_fok_fills_across_multiple_opposing_orders(self):
        # FOK buy needs 100; two sells of 60 each → cumulative 120 ≥ 100 → fills
        # s1 (older) consumed fully; s2 (newer) partially consumed, 20 left in unfilled
        b  = fok(Side.BUY, 105, 100)
        s1 = gtc(Side.SELL, 100, 60, t=0)
        s2 = gtc(Side.SELL, 100, 60, t=1)
        trades, unfilled = Uniform().clear([b], [s1, s2])
        assert sum(t.quantity for t in trades) == 100
        assert any(t.buy_order_id == b.order_id for t in trades)
        assert not any(o.order_id == s1.order_id for o in unfilled)
        assert any(o.order_id == s2.order_id for o in unfilled)

    def test_fok_fails_cumulative_volume_insufficient(self):
        # FOK needs 100; two sells total 80 → can't fill → FOK dropped, sells kept
        b  = fok(Side.BUY, 105, 100)
        s1 = gtc(Side.SELL, 100, 40, t=0)
        s2 = gtc(Side.SELL, 100, 40, t=1)
        trades, unfilled = Uniform().clear([b], [s1, s2])
        assert trades == []
        assert not any(o.order_id == b.order_id for o in unfilled)
        assert any(o.order_id == s1.order_id for o in unfilled)
        assert any(o.order_id == s2.order_id for o in unfilled)

    # ------------------------------------------------------------------
    # FOK interaction with GTC orders on the same side
    # ------------------------------------------------------------------

    def test_gtc_consumes_volume_leaving_fok_unfillable(self):
        # GTC buy arrives first (t=0), consumes all 30 sell volume.
        # FOK buy arrives later (t=1), finds nothing left and is cancelled.
        b_gtc = gtc(Side.BUY, 105, 30, t=0)
        b_fok = fok(Side.BUY, 105, 50, t=1)
        s     = gtc(Side.SELL, 100, 30, t=0)
        trades, unfilled = Uniform().clear([b_gtc, b_fok], [s])
        assert sum(t.quantity for t in trades) == 30
        assert any(t.buy_order_id == b_gtc.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)

    def test_fok_fails_older_gtc_behind_it_still_fills(self):
        # FOK buy arrives first (t=0) with qty 100; sell has only 40.
        # FOK fails and is skipped. GTC buy (t=1, qty 40) then fills.
        b_fok = fok(Side.BUY, 105, 100, t=0)
        b_gtc = gtc(Side.BUY, 105,  40, t=1)
        s     = gtc(Side.SELL, 100,  40, t=0)
        trades, unfilled = Uniform().clear([b_fok, b_gtc], [s])
        assert sum(t.quantity for t in trades) == 40
        assert any(t.buy_order_id == b_gtc.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)

    def test_fok_on_short_side_fills_fully(self):
        # Buy side is short (30+20=50 vs sell 80). Both buys fill in full.
        # The FOK buy is not rationed because the buy side is the short side.
        b_gtc = gtc(Side.BUY, 105, 30, t=0)
        b_fok = fok(Side.BUY, 105, 20, t=1)
        s     = gtc(Side.SELL, 100, 80, t=0)
        trades, unfilled = Uniform().clear([b_gtc, b_fok], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id == b_fok.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)

    # ------------------------------------------------------------------
    # FOK shifts the winning clearing price
    # ------------------------------------------------------------------

    def test_fok_requirement_drives_clearing_price_up(self):
        # At p=95: only s1 (qty 50) is eligible on the sell side.
        #   FOK buy (qty 100) vs s1 (50) → can't fill → 0 trades at p=95.
        # At p=100: both s1 (50) and s2 (70) eligible; cumulative 120 ≥ 100
        #   → FOK fills → 100 trades at p=100.
        # p=100 is the only level with positive volume, so it wins.
        b  = fok(Side.BUY, 100, 100)
        s1 = gtc(Side.SELL,  95,  50, t=0)
        s2 = gtc(Side.SELL, 100,  70, t=1)
        trades, _ = Uniform().clear([b], [s1, s2])
        assert all(t.price == 100 for t in trades)
        assert sum(t.quantity for t in trades) == 100

    # ------------------------------------------------------------------
    # Opposing FOKs
    # ------------------------------------------------------------------

    def test_opposing_foks_equal_qty_both_fill(self):
        # FOK buy and FOK sell with matching quantities execute against each other
        b = fok(Side.BUY,  105, 50, t=0)
        s = fok(Side.SELL, 100, 50, t=1)
        trades, unfilled = Uniform().clear([b], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id  == b.order_id for t in trades)
        assert any(t.sell_order_id == s.order_id for t in trades)
        assert unfilled == []

    def test_opposing_foks_one_too_large_both_fail(self):
        # FOK sell (qty 100) needs more than FOK buy (qty 50) can provide.
        # The recursive walk finds no other buyers; neither side can fill → both dropped.
        b = fok(Side.BUY,  105,  50, t=0)
        s = fok(Side.SELL, 100, 100, t=1)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades   == []
        assert unfilled == []

    def test_fok_buy_skips_fok_sell_fills_with_gtc(self):
        # FOK buy (50) meets FOK sell (100) first; FOK sell is too large to
        # satisfy the FOK buy in isolation (100 > 50). The walk skips it and
        # finds GTC sell (50) instead, which covers the full 50 → FOK buy fills.
        # FOK sell can't fill (only FOK buy was on the other side, not enough)
        # → FOK sell is dropped.
        b_fok = fok(Side.BUY,  105,  50, t=0)
        s_fok = fok(Side.SELL, 100, 100, t=1)
        s_gtc = gtc(Side.SELL, 100,  50, t=2)
        trades, unfilled = Uniform().clear([b_fok], [s_fok, s_gtc])
        assert len(trades) == 1
        assert trades[0].quantity == 50
        assert trades[0].buy_order_id  == b_fok.order_id
        assert trades[0].sell_order_id == s_gtc.order_id
        assert not any(o.order_id == s_fok.order_id for o in unfilled)
