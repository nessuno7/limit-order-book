"""
FOK tests for the Uniform call-auction pricing rule (old_uniform.py).

All-or-nothing semantics in a call auction:
  - A FOK that cannot fill at any candidate price level is cancelled (not in unfilled)
  - A failed FOK does not block GTC orders on the same side
  - The presence of a FOK can change which price level wins (a level where the
    FOK cannot fill records zero volume from it)
  - Opposing FOKs walk each other recursively; if neither can fill, both drop

NOTE: tests marked with (*) will fail until the no-trade return path on
line 175 of old_uniform.py is fixed:
    if not level_volume:
        return [], []           # current — drops all GTC orders
        # should be:
        return [], [o for o in buys + sells if o.tif is TimeInForce.GTC]
"""
from datetime import datetime, timedelta
from uuid import uuid4

from python_engine.call_auction.rules.old_uniform import Uniform
from python_engine.orderbook.order import Order, Side, TimeInForce

TRADER = uuid4()
T0 = datetime(2026, 1, 1, 9, 0, 0)


def gtc(side, price, qty, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.GTC,
                 timestamp=T0 + timedelta(seconds=t))


def fok(side, price, qty, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.FOK,
                 timestamp=T0 + timedelta(seconds=t))


class TestFOKBasic:
    """Single FOK order against GTC orders — fill and fail cases."""

    def test_fok_buy_fills_fully(self):
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

    def test_failed_fok_is_never_in_unfilled(self):
        b = fok(Side.BUY, 105, 100)
        s = gtc(Side.SELL, 100, 30)
        _, unfilled = Uniform().clear([b], [s])
        assert not any(o.order_id == b.order_id for o in unfilled)

    # (*) these fail until line 175 returns GTC leftovers instead of [], []

    def test_fok_buy_fails_gtc_sell_preserved(self):
        # FOK needs 100, only 50 available → no trades; GTC sell should roll forward
        b = fok(Side.BUY, 105, 100)
        s = gtc(Side.SELL, 100, 50)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades == []
        assert not any(o.order_id == b.order_id for o in unfilled)
        assert any(o.order_id == s.order_id for o in unfilled)  # (*) fails

    def test_fok_sell_fails_gtc_buy_preserved(self):
        b = gtc(Side.BUY, 105, 50)
        s = fok(Side.SELL, 100, 100)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades == []
        assert not any(o.order_id == s.order_id for o in unfilled)
        assert any(o.order_id == b.order_id for o in unfilled)  # (*) fails

    def test_fok_fills_across_multiple_opposing_orders(self):
        # FOK needs 100; two sells of 60 each → cumulative 120 ≥ 100 → fills
        # s1 (older) consumed fully; s2 partially consumed, 20 left in unfilled
        b  = fok(Side.BUY, 105, 100)
        s1 = gtc(Side.SELL, 100, 60, t=0)
        s2 = gtc(Side.SELL, 100, 60, t=1)
        trades, unfilled = Uniform().clear([b], [s1, s2])
        assert sum(t.quantity for t in trades) == 100
        assert any(t.buy_order_id == b.order_id for t in trades)
        assert not any(o.order_id == s1.order_id for o in unfilled)
        assert any(o.order_id == s2.order_id for o in unfilled)

    def test_fok_fails_cumulative_volume_insufficient(self):
        # Two sells total 80; FOK needs 100 → fails; both GTC sells should roll forward
        b  = fok(Side.BUY, 105, 100)
        s1 = gtc(Side.SELL, 100, 40, t=0)
        s2 = gtc(Side.SELL, 100, 40, t=1)
        trades, unfilled = Uniform().clear([b], [s1, s2])
        assert trades == []
        assert not any(o.order_id == b.order_id for o in unfilled)
        assert any(o.order_id == s1.order_id for o in unfilled)  # (*) fails
        assert any(o.order_id == s2.order_id for o in unfilled)  # (*) fails


class TestFOKAndGTC:
    """FOK interacting with GTC orders on the same side."""

    def test_gtc_consumes_volume_leaving_fok_unfillable(self):
        # GTC buy (t=0) is older; takes all 30 sell volume.
        # FOK buy (t=1) finds nothing left and is cancelled.
        b_gtc = gtc(Side.BUY, 105, 30, t=0)
        b_fok = fok(Side.BUY, 105, 50, t=1)
        s     = gtc(Side.SELL, 100, 30, t=0)
        trades, unfilled = Uniform().clear([b_gtc, b_fok], [s])
        assert sum(t.quantity for t in trades) == 30
        assert any(t.buy_order_id == b_gtc.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)

    def test_fok_fails_gtc_behind_it_still_fills(self):
        # FOK buy arrives first (t=0), qty 100 — sell has only 40 → FOK skipped.
        # GTC buy (t=1), qty 40 fills normally.
        b_fok = fok(Side.BUY, 105, 100, t=0)
        b_gtc = gtc(Side.BUY, 105,  40, t=1)
        s     = gtc(Side.SELL, 100,  40, t=0)
        trades, unfilled = Uniform().clear([b_fok, b_gtc], [s])
        assert sum(t.quantity for t in trades) == 40
        assert any(t.buy_order_id == b_gtc.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)

    def test_fok_on_short_side_fills_fully(self):
        # Buy side is short (30+20=50 vs sell 80): the FOK is not rationed,
        # it fills in full because the short side always clears completely.
        b_gtc = gtc(Side.BUY, 105, 30, t=0)
        b_fok = fok(Side.BUY, 105, 20, t=1)
        s     = gtc(Side.SELL, 100, 80, t=0)
        trades, unfilled = Uniform().clear([b_gtc, b_fok], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id == b_fok.order_id for t in trades)
        assert not any(o.order_id == b_fok.order_id for o in unfilled)


class TestFOKClearingPrice:
    """FOK can shift which price level wins the auction."""

    def test_fok_requirement_drives_clearing_price_up(self):
        # At p=95: only s1 eligible (qty 50); FOK (qty 100) vs 50 → can't fill → 0 trades
        # At p=100: s1+s2 eligible (total 120 ≥ 100) → fills → 100 trades
        # p=100 is the only level with positive volume
        b  = fok(Side.BUY, 100, 100)
        s1 = gtc(Side.SELL,  95,  50, t=0)
        s2 = gtc(Side.SELL, 100,  70, t=1)
        trades, _ = Uniform().clear([b], [s1, s2])
        assert all(t.price == 100 for t in trades)
        assert sum(t.quantity for t in trades) == 100

    def test_fok_failure_does_not_block_gtc_at_same_level(self):
        # FOK buy (qty 80, t=0) fails because only 50 sell volume exists.
        # GTC buy (qty 50, t=1) fills normally.
        b_fok = fok(Side.BUY, 105, 80, t=0)
        b_gtc = gtc(Side.BUY, 105, 50, t=1)
        s     = gtc(Side.SELL, 100, 50, t=0)
        trades, _ = Uniform().clear([b_fok, b_gtc], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id == b_gtc.order_id for t in trades)


class TestOpposingFOKs:
    """FOK orders on opposite sides — the recursive walk cases."""

    def test_opposing_foks_equal_qty_both_fill(self):
        b = fok(Side.BUY,  105, 50, t=0)
        s = fok(Side.SELL, 100, 50, t=1)
        trades, unfilled = Uniform().clear([b], [s])
        assert sum(t.quantity for t in trades) == 50
        assert any(t.buy_order_id  == b.order_id for t in trades)
        assert any(t.sell_order_id == s.order_id for t in trades)
        assert unfilled == []

    def test_opposing_foks_one_too_large_both_fail(self):
        # FOK sell (100) needs more than FOK buy (50) can provide; neither fills
        b = fok(Side.BUY,  105,  50, t=0)
        s = fok(Side.SELL, 100, 100, t=1)
        trades, unfilled = Uniform().clear([b], [s])
        assert trades   == []
        assert unfilled == []

    def test_fok_buy_skips_oversized_fok_sell_fills_with_gtc(self):
        # FOK buy (50) meets FOK sell (100) first in the timestamp-sorted list.
        # 100 > 50 so recursive check fails; FOK sell is skipped.
        # GTC sell (50) follows and covers the full 50 → FOK buy fills.
        # FOK sell has no remaining buyers and is dropped.
        b_fok = fok(Side.BUY,  105,  50, t=0)
        s_fok = fok(Side.SELL, 100, 100, t=1)
        s_gtc = gtc(Side.SELL, 100,  50, t=2)
        trades, unfilled = Uniform().clear([b_fok], [s_fok, s_gtc])
        assert len(trades) == 1
        assert trades[0].quantity == 50
        assert trades[0].buy_order_id  == b_fok.order_id
        assert trades[0].sell_order_id == s_gtc.order_id
        assert not any(o.order_id == s_fok.order_id for o in unfilled)


class TestChainedFOK:
    """
    The core edge case: FOK A's available volume includes FOK B.
    Whether A is accepted depends on whether B can itself be fully filled
    from the remaining pool. The recursive _fok_walk handles this.
    """

    def test_fok_buy_uses_fok_sell_when_fok_sell_can_fill(self):
        # FOK BUY A qty=4 (t=2), FOK SELL B qty=10 (t=1), GTC BUY C qty=6 (t=3)
        #
        # A needs 4. B provides 10 but needs 10 in return.
        # B gets 4 from A + 6 from GTC C = 10 → B fills. So A may rely on B.
        # Total traded: A(4) + C(6) against B(10) = 10.
        b_fok_a = fok(Side.BUY,  105,  4, t=2)
        b_gtc_c = gtc(Side.BUY,  105,  6, t=3)
        s_fok_b = fok(Side.SELL, 100, 10, t=1)
        trades, unfilled = Uniform().clear([b_fok_a, b_gtc_c], [s_fok_b])
        assert sum(t.quantity for t in trades) == 10
        assert unfilled == []

    def test_fok_buy_rejected_when_chained_fok_sell_cannot_fill(self):
        # FOK BUY A qty=4 (t=2), FOK SELL B qty=10 (t=1),
        # GTC SELL D qty=2 (t=0), GTC BUY C qty=5 (t=3).
        #
        # Naive view: D(2) + B(10) = 12 ≥ 4 → would accept A. WRONG.
        # Reality: B needs 10. It gets 2 from A + 5 from C = 7 < 10. B cannot fill.
        # B's volume is unreliable → A can only use D(2) → 2 < 4 → A is rejected.
        b_fok_a = fok(Side.BUY,  105,  4, t=2)
        b_gtc_c = gtc(Side.BUY,  105,  5, t=3)
        s_fok_b = fok(Side.SELL, 100, 10, t=1)
        s_gtc_d = gtc(Side.SELL, 100,  2, t=0)
        trades, _ = Uniform().clear([b_fok_a, b_gtc_c], [s_fok_b, s_gtc_d])
        fok_a_trades = [t for t in trades if t.buy_order_id == b_fok_a.order_id]
        assert fok_a_trades == []

    def test_gtc_still_trades_when_fok_chain_fails(self):
        # Same setup as above. After A and B are both rejected,
        # GTC C (buy, qty=5) and GTC D (sell, qty=2) can still cross.
        b_fok_a = fok(Side.BUY,  105,  4, t=2)
        b_gtc_c = gtc(Side.BUY,  105,  5, t=3)
        s_fok_b = fok(Side.SELL, 100, 10, t=1)
        s_gtc_d = gtc(Side.SELL, 100,  2, t=0)
        trades, _ = Uniform().clear([b_fok_a, b_gtc_c], [s_fok_b, s_gtc_d])
        assert sum(t.quantity for t in trades) > 0
        assert any(t.buy_order_id  == b_gtc_c.order_id for t in trades)
        assert any(t.sell_order_id == s_gtc_d.order_id for t in trades)
