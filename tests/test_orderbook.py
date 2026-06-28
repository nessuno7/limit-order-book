from uuid import uuid4
import pytest
from python_engine.orderbook.orderbook import OrderBook
from python_engine.orderbook.order import Order, Side

TRADER = uuid4()


def lmt(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


class TestAdd:
    def test_bid_appears_in_bids(self):
        book = OrderBook("TEST")
        o = lmt(Side.BUY, 100, 10)
        book.add(o)
        assert 100 in book.bids
        assert book._index[o.order_id] is o

    def test_ask_appears_in_asks(self):
        book = OrderBook("TEST")
        o = lmt(Side.SELL, 105, 5)
        book.add(o)
        assert 105 in book.asks
        assert book._index[o.order_id] is o

    def test_multiple_orders_same_level_queued(self):
        book = OrderBook("TEST")
        o1 = lmt(Side.BUY, 100, 10)
        o2 = lmt(Side.BUY, 100, 5)
        book.add(o1)
        book.add(o2)
        assert len(book.bids[100]) == 2

    def test_orders_at_different_levels(self):
        book = OrderBook("TEST")
        book.add(lmt(Side.BUY, 100, 10))
        book.add(lmt(Side.BUY, 101, 5))
        assert 100 in book.bids and 101 in book.bids

    def test_market_order_not_added(self):
        book = OrderBook("TEST")
        o = Order(side=Side.BUY, price=None, quantity=10, trader_id=TRADER)
        book.add(o)
        assert not book.bids


class TestBestPrices:
    def test_best_bid_empty(self):
        assert OrderBook("TEST").best_bid() == (None, None)

    def test_best_ask_empty(self):
        assert OrderBook("TEST").best_ask() == (None, None)

    def test_best_bid_single(self):
        book = OrderBook("TEST")
        o = lmt(Side.BUY, 100, 10)
        book.add(o)
        price, uid = book.best_bid()
        assert price == 100 and uid == o.order_id

    def test_best_ask_single(self):
        book = OrderBook("TEST")
        o = lmt(Side.SELL, 105, 5)
        book.add(o)
        price, uid = book.best_ask()
        assert price == 105 and uid == o.order_id

    def test_best_bid_is_highest_price(self):
        book = OrderBook("TEST")
        book.add(lmt(Side.BUY, 99, 3))
        book.add(lmt(Side.BUY, 100, 10))
        book.add(lmt(Side.BUY, 101, 5))
        price, _ = book.best_bid()
        assert price == 101

    def test_best_ask_is_lowest_price(self):
        book = OrderBook("TEST")
        book.add(lmt(Side.SELL, 107, 3))
        book.add(lmt(Side.SELL, 105, 10))
        book.add(lmt(Side.SELL, 103, 5))
        price, _ = book.best_ask()
        assert price == 103


class TestCancel:
    def test_cancel_removes_from_index(self):
        book = OrderBook("TEST")
        o = lmt(Side.BUY, 100, 10)
        book.add(o)
        book.cancel(o.order_id)
        assert o.order_id not in book._index

    def test_cancel_removes_empty_level(self):
        book = OrderBook("TEST")
        o = lmt(Side.BUY, 100, 10)
        book.add(o)
        book.cancel(o.order_id)
        assert 100 not in book.bids

    def test_cancel_front_leaves_level_intact(self):
        book = OrderBook("TEST")
        o1 = lmt(Side.BUY, 100, 10)
        o2 = lmt(Side.BUY, 100, 5)
        book.add(o1)
        book.add(o2)
        book.cancel(o1.order_id)  # o1 is at front of the deque
        assert 100 in book.bids
        assert len(book.bids[100]) == 1
        assert book.bids[100][0] == o2.order_id
