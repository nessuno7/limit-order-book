"""
Edge cases the original suite never reached.

Each class targets one behaviour that the happy-path tests walk around:
cancelling a non-front order, IOC/FOK leftovers in the discriminatory rule,
market FOKs, the batch -> clear -> pending round trip against a *real* pricing
rule, tie-breaking that must not depend on global RNG state, and input
validation.
"""
import importlib
import random
from datetime import datetime, timedelta

import pytest

from python_engine.call_auction.call_auction import CallAuction
from python_engine.call_auction.rules.discriminatory import Discriminatory
from python_engine.call_auction.rules import uniform as uniform_module
from python_engine.call_auction.rules.uniform import Uniform
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side, TimeInForce
from python_engine.orderbook.orderbook import OrderBook

TRADER = 1
T0 = datetime(2026, 1, 1, 9, 0, 0)


def order(side, price, qty, tif=TimeInForce.GTC, t=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=tif, timestamp=T0 + timedelta(seconds=t))


def ids(orders):
    return {o.order_id for o in orders}


class TestCancelAnyOrder:
    def test_cancel_middle_of_level_removes_that_order(self):
        book = OrderBook("TEST")
        o1, o2, o3 = (order(Side.BUY, 100, q) for q in (1, 2, 3))
        for o in (o1, o2, o3):
            book.add(o)
        book.cancel(o2.order_id)
        assert list(book.bids[100]) == [o1.order_id, o3.order_id]
        assert set(book._index) == {o1.order_id, o3.order_id}

    def test_cancel_back_of_level_keeps_front_as_best(self):
        book = OrderBook("TEST")
        o1, o2 = order(Side.SELL, 100, 1), order(Side.SELL, 100, 2)
        book.add(o1)
        book.add(o2)
        book.cancel(o2.order_id)
        assert book.best_ask() == (100, o1.order_id)

    def test_cancel_returns_the_cancelled_order(self):
        book = OrderBook("TEST")
        o = order(Side.BUY, 100, 5)
        book.add(o)
        assert book.cancel(o.order_id) is o

    def test_cancel_unknown_id_raises_key_error(self):
        with pytest.raises(KeyError):
            OrderBook("TEST").cancel(10**18)

    def test_matching_still_works_after_cancelling_non_front_order(self):
        e = ContinuousDoubleAuction()
        s1, s2 = order(Side.SELL, 100, 5), order(Side.SELL, 100, 5)
        e.submit(s1)
        e.submit(s2)
        e.book.cancel(s2.order_id)
        trades = e.submit(order(Side.BUY, 100, 10))
        assert [(t.sell_order_id, t.quantity) for t in trades] == [(s1.order_id, 5)]
        assert e.book.best_bid()[0] == 100  # remaining 5 rests


class TestContinuousMarketOrders:
    def test_market_fok_fills_when_liquidity_suffices(self):
        e = ContinuousDoubleAuction()
        e.submit(order(Side.SELL, 100, 5))
        e.submit(order(Side.SELL, 101, 5))
        trades = e.submit(order(Side.BUY, None, 8, TimeInForce.FOK))
        assert sum(t.quantity for t in trades) == 8

    def test_market_fok_rejected_when_liquidity_short(self):
        e = ContinuousDoubleAuction()
        e.submit(order(Side.BUY, 100, 3))
        assert e.submit(order(Side.SELL, None, 5, TimeInForce.FOK)) == []
        assert e.book._index[e.book.best_bid()[1]].quantity == 3

    def test_gtc_market_remainder_never_rests(self):
        e = ContinuousDoubleAuction()
        e.submit(order(Side.SELL, 100, 4))
        mkt = order(Side.BUY, None, 10)
        trades = e.submit(mkt)
        assert sum(t.quantity for t in trades) == 4
        assert e.book.bids == {} and e.book.asks == {}
        assert mkt.order_id not in e.book._index

    def test_ioc_limit_remainder_does_not_rest(self):
        e = ContinuousDoubleAuction()
        e.submit(order(Side.SELL, 100, 4))
        trades = e.submit(order(Side.BUY, 100, 10, TimeInForce.IOC))
        assert sum(t.quantity for t in trades) == 4
        assert e.book.bids == {}


class TestDuplicateSubmission:
    def test_resubmitting_a_resting_order_is_rejected(self):
        e = ContinuousDoubleAuction()
        o = order(Side.BUY, 100, 5)
        e.submit(o)
        with pytest.raises(ValueError):
            e.submit(o)
        assert list(e.book.bids[100]) == [o.order_id]

    def test_resubmitting_a_filled_order_is_rejected(self):
        e = ContinuousDoubleAuction()
        e.submit(order(Side.SELL, 100, 5))
        b = order(Side.BUY, 100, 5)
        e.submit(b)
        assert b.quantity == 0
        e.submit(order(Side.SELL, 100, 5))
        with pytest.raises(ValueError):
            e.submit(b)
        assert all(t.quantity > 0 for t in e.trades)
        assert b.order_id not in e.book._index

    def test_call_auction_rejects_filled_order(self):
        e = CallAuction(Uniform())
        b = order(Side.BUY, 105, 5, t=0)
        e.submit(b)
        e.submit(order(Side.SELL, 100, 5, t=1))
        e.run()
        with pytest.raises(ValueError):
            e.submit(b)

    def test_call_auction_rejects_duplicate_pending_order(self):
        e = CallAuction(Uniform())
        o = order(Side.BUY, 100, 5)
        e.submit(o)
        with pytest.raises(ValueError):
            e.submit(o)


class TestDiscriminatoryTimeInForce:
    def test_ioc_left_in_queue_is_not_rolled_forward(self):
        # b1 crosses s; b2 (IOC) is still queued when the sells run out.
        b1 = order(Side.BUY, 105, 5, t=0)
        b2 = order(Side.BUY, 104, 5, TimeInForce.IOC, t=1)
        s = order(Side.SELL, 100, 5, t=2)
        trades, unfilled = Discriminatory().clear([b1, b2], [s])
        assert sum(t.quantity for t in trades) == 5
        assert b2.order_id not in ids(unfilled)

    def test_non_crossing_ioc_and_fok_are_dropped(self):
        b_ioc = order(Side.BUY, 90, 5, TimeInForce.IOC, t=0)
        b_fok = order(Side.BUY, 89, 5, TimeInForce.FOK, t=1)
        b_gtc = order(Side.BUY, 88, 5, t=2)
        s_ioc = order(Side.SELL, 110, 5, TimeInForce.IOC, t=3)
        s_gtc = order(Side.SELL, 111, 5, t=4)
        trades, unfilled = Discriminatory().clear([b_ioc, b_fok, b_gtc], [s_ioc, s_gtc])
        assert trades == []
        assert ids(unfilled) == {b_gtc.order_id, s_gtc.order_id}

    def test_market_order_rejected_cleanly(self):
        with pytest.raises(ValueError):
            Discriminatory().clear([order(Side.BUY, None, 5)], [order(Side.SELL, 100, 5)])

    def test_call_auction_rejects_market_order_at_submit(self):
        e = CallAuction(Discriminatory())
        with pytest.raises(ValueError):
            e.submit(order(Side.BUY, None, 5))
        assert e._pending == []

    def test_equal_timestamps_price_at_older_by_arrival(self):
        # Same timestamp: the order created first is the maker, so its
        # limit sets the price.
        b = order(Side.BUY, 105, 5, t=0)
        s = order(Side.SELL, 100, 5, t=0)
        trades, _ = Discriminatory().clear([b], [s])
        assert trades[0].price == 105


class TestChainedFOKBacktracking:
    """
    FOK BUY A(4) is first in line. Its opposing sells are FOK B(10) and FOK
    C(6). B cannot fill (A's 4 + GTC D's 5 = 9 < 10). C can: A gives 4, D gives
    2. A failed attempt on B must not leave D marked as used up when C is
    tried next.
    """

    @pytest.mark.parametrize("rule", [Discriminatory, Uniform])
    def test_failed_branch_does_not_poison_the_next(self, rule):
        a = order(Side.BUY, 100, 4, TimeInForce.FOK, t=0)
        d = order(Side.BUY, 100, 5, t=3)
        b = order(Side.SELL, 100, 10, TimeInForce.FOK, t=1)
        c = order(Side.SELL, 100, 6, TimeInForce.FOK, t=2)
        trades, unfilled = rule().clear([a, d], [b, c])
        fills = sorted((t.buy_order_id == a.order_id, t.quantity) for t in trades)
        assert fills == [(False, 2), (True, 4)]
        assert all(t.sell_order_id == c.order_id for t in trades)
        assert [(o.order_id, o.quantity) for o in unfilled] == [(d.order_id, 3)]


class TestCallAuctionRoundTrip:
    """batch -> clear -> pending, against real pricing rules (not stubs)."""

    def test_uniform_rolls_original_objects_with_updated_quantity(self):
        e = CallAuction(Uniform())
        b = order(Side.BUY, 105, 50, t=0)
        s = order(Side.SELL, 100, 30, t=1)
        e.submit(b)
        e.submit(s)
        e.run()
        assert len(e._pending) == 1
        assert e._pending[0] is b
        assert b.quantity == 20

    def test_uniform_remainder_trades_in_next_batch(self):
        e = CallAuction(Uniform())
        b = order(Side.BUY, 105, 50, t=0)
        e.submit(b)
        e.submit(order(Side.SELL, 100, 30, t=1))
        e.run()
        e.submit(order(Side.SELL, 100, 20, t=2))
        trades = e.run()
        assert sum(t.quantity for t in trades) == 20
        assert all(t.buy_order_id == b.order_id for t in trades)
        assert e._pending == []

    def test_discriminatory_ioc_does_not_survive_into_next_batch(self):
        e = CallAuction(Discriminatory())
        e.submit(order(Side.BUY, 105, 5, t=0))
        e.submit(order(Side.BUY, 104, 5, TimeInForce.IOC, t=1))
        e.submit(order(Side.SELL, 100, 5, t=2))
        e.run()
        assert e._pending == []


class TestUniformTieBreak:
    @pytest.mark.parametrize("seed", range(20))
    def test_tie_break_is_highest_price_whatever_the_rng_state(self, seed):
        random.seed(seed)
        b = order(Side.BUY, 105, 50)
        s = order(Side.SELL, 95, 50)
        trades, _ = Uniform().clear([b], [s])
        assert trades[0].price == 105

    def test_importing_uniform_leaves_global_rng_alone(self):
        random.seed(1234)
        expected = random.random()
        random.seed(1234)
        importlib.reload(uniform_module)
        assert random.random() == expected


class TestValidation:
    @pytest.mark.parametrize("qty", [0, -5])
    def test_non_positive_quantity_rejected(self, qty):
        with pytest.raises(ValueError):
            order(Side.BUY, 100, qty)

    def test_float_price_rejected(self):
        with pytest.raises(TypeError):
            order(Side.BUY, 100.5, 5)

    def test_fok_enum_value(self):
        assert TimeInForce("fok") is TimeInForce.FOK
