"""Order amendment in the continuous market; mirrors the C++ continuous_modify_* tests."""
import pytest

from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side

TRADER = 1


def gtc(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


def test_reduce_keeps_priority():
    e = ContinuousDoubleAuction()
    first, second = gtc(Side.SELL, 100, 10), gtc(Side.SELL, 100, 10)
    e.submit(first)
    e.submit(second)
    assert e.modify(first.order_id, 100, 4) == []
    assert e.book._index[first.order_id].quantity == 4
    trades = e.submit(gtc(Side.BUY, 100, 4))
    assert [t.sell_order_id for t in trades] == [first.order_id]


def test_increase_loses_priority():
    e = ContinuousDoubleAuction()
    first, second = gtc(Side.SELL, 100, 10), gtc(Side.SELL, 100, 10)
    e.submit(first)
    e.submit(second)
    e.modify(first.order_id, 100, 11)
    trades = e.submit(gtc(Side.BUY, 100, 10))
    assert [t.sell_order_id for t in trades] == [second.order_id]
    assert e.book._index[first.order_id].quantity == 11


def test_same_price_same_qty_is_a_noop():
    e = ContinuousDoubleAuction()
    first = gtc(Side.BUY, 100, 5)
    e.submit(first)
    e.submit(gtc(Side.BUY, 100, 5))
    e.modify(first.order_id, 100, 5)
    assert e.book.best_bid()[1] == first.order_id


def test_price_change_moves_level():
    e = ContinuousDoubleAuction()
    b = gtc(Side.BUY, 100, 5)
    e.submit(b)
    assert e.modify(b.order_id, 98, 5) == []
    assert e.book.best_bid()[0] == 98
    assert 100 not in e.book.bids


def test_modify_to_crossing_price_trades():
    e = ContinuousDoubleAuction()
    ask, bid = gtc(Side.SELL, 101, 3), gtc(Side.BUY, 99, 5)
    e.submit(ask)
    e.submit(bid)
    trades = e.modify(bid.order_id, 102, 5)
    assert [(t.price, t.quantity) for t in trades] == [(101, 3)]
    assert e.book._index[bid.order_id].quantity == 2
    assert e.book.best_bid()[0] == 102
    assert len(e.trades) == 1


def test_rejects_unknown_id_and_bad_qty():
    e = ContinuousDoubleAuction()
    b = gtc(Side.BUY, 100, 5)
    with pytest.raises(KeyError):
        e.modify(b.order_id, 100, 1)
    e.submit(b)
    for bad in (0, -3):
        with pytest.raises(ValueError):
            e.modify(b.order_id, 100, bad)
    assert e.book._index[b.order_id].quantity == 5
