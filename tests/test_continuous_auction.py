"""
Tests for ContinuousDoubleAuction.
"""
import pytest
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side

TRADER = 1


def lmt(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


def mkt(side, qty):
    return Order(side=side, price=None, quantity=qty, trader_id=TRADER)


class TestNoMatch:
    def test_buy_with_no_asks_rests(self):
        e = ContinuousDoubleAuction()
        trades = e.submit(lmt(Side.BUY, 100, 10))
        assert trades == []
        assert e.book.best_bid()[0] == 100

    def test_sell_with_no_bids_rests(self):
        e = ContinuousDoubleAuction()
        trades = e.submit(lmt(Side.SELL, 100, 10))
        assert trades == []
        assert e.book.best_ask()[0] == 100

    def test_buy_price_below_ask_rests(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 105, 10))
        trades = e.submit(lmt(Side.BUY, 100, 10))
        assert trades == []
        assert e.book.best_bid()[0] == 100
        assert e.book.best_ask()[0] == 105

    def test_sell_price_above_bid_rests(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.BUY, 100, 10))
        trades = e.submit(lmt(Side.SELL, 105, 10))
        assert trades == []


class TestExactMatch:
    def test_exact_qty_and_price(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 10))
        trades = e.submit(lmt(Side.BUY, 100, 10))
        assert len(trades) == 1
        assert trades[0].quantity == 10
        assert trades[0].price == 100

    def test_book_empty_after_exact_match(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 10))
        e.submit(lmt(Side.BUY, 100, 10))
        assert e.book.best_bid() == (None, None)
        assert e.book.best_ask() == (None, None)

    def test_sell_aggresses_exact_match(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.BUY, 100, 10))
        trades = e.submit(lmt(Side.SELL, 100, 10))
        assert len(trades) == 1
        assert trades[0].quantity == 10


class TestPartialFill:
    def test_aggressor_smaller_resting_has_remainder(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 10))
        trades = e.submit(lmt(Side.BUY, 100, 4))
        assert len(trades) == 1
        assert trades[0].quantity == 4
        ask_price, ask_id = e.book.best_ask()
        assert ask_price == 100
        assert e.book._index[ask_id].quantity == 6

    def test_resting_smaller_aggressor_rests_remainder(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 4))
        trades = e.submit(lmt(Side.BUY, 100, 10))
        assert len(trades) == 1
        assert trades[0].quantity == 4
        bid_price, bid_id = e.book.best_bid()
        assert bid_price == 100
        assert e.book._index[bid_id].quantity == 6


class TestPricePriority:
    def test_trade_executes_at_resting_ask_price(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 99, 10))
        trades = e.submit(lmt(Side.BUY, 105, 10))
        assert trades[0].price == 99

    def test_trade_executes_at_resting_bid_price(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.BUY, 105, 10))
        trades = e.submit(lmt(Side.SELL, 99, 10))
        assert trades[0].price == 105


class TestFIFO:
    def test_earlier_resting_order_fills_first(self):
        e = ContinuousDoubleAuction()
        s1 = lmt(Side.SELL, 100, 5)
        s2 = lmt(Side.SELL, 100, 5)
        e.submit(s1)
        e.submit(s2)
        trades = e.submit(lmt(Side.BUY, 100, 5))
        assert len(trades) == 1
        assert trades[0].sell_order_id == s1.order_id
        assert len(e.book.asks[100]) == 1  # s2 still resting


class TestMultiLevelSweep:
    def test_buy_sweeps_two_ask_levels(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 5))
        e.submit(lmt(Side.SELL, 101, 5))
        trades = e.submit(lmt(Side.BUY, 101, 10))
        assert len(trades) == 2
        assert sum(t.quantity for t in trades) == 10
        assert e.book.best_ask() == (None, None)

    def test_sell_sweeps_two_bid_levels(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.BUY, 101, 5))
        e.submit(lmt(Side.BUY, 100, 5))
        trades = e.submit(lmt(Side.SELL, 100, 10))
        assert len(trades) == 2
        assert sum(t.quantity for t in trades) == 10
        assert e.book.best_bid() == (None, None)


class TestMarketOrders:
    def test_market_buy_hits_best_ask(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 10))
        trades = e.submit(mkt(Side.BUY, 10))
        assert len(trades) == 1
        assert trades[0].quantity == 10
        assert trades[0].price == 100

    def test_market_sell_hits_best_bid(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.BUY, 100, 10))
        trades = e.submit(mkt(Side.SELL, 10))
        assert len(trades) == 1
        assert trades[0].quantity == 10
        assert trades[0].price == 100

    def test_market_buy_sweeps_multiple_levels(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 5))
        e.submit(lmt(Side.SELL, 101, 5))
        trades = e.submit(mkt(Side.BUY, 10))
        assert len(trades) == 2
        assert sum(t.quantity for t in trades) == 10


class TestTradeAccumulation:
    def test_trades_stored_on_engine(self):
        e = ContinuousDoubleAuction()
        e.submit(lmt(Side.SELL, 100, 5))
        e.submit(lmt(Side.SELL, 101, 5))
        e.submit(lmt(Side.BUY, 101, 10))
        assert len(e.trades) == 2
