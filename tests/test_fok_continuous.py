"""
FOK tests for ContinuousDoubleAuction.

The key invariant: FOK orders NEVER rest in the book. They either fill
completely in the same submit() call or are cancelled (return []). This
is what prevents the chained-FOK edge case that can occur in call auctions.
"""
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side, TimeInForce

TRADER = 1


def gtc(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


def fok(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER, tif=TimeInForce.FOK)


class TestFOKRejected:
    def test_fok_rejected_no_liquidity(self):
        e = ContinuousDoubleAuction()
        trades = e.submit(fok(Side.BUY, 100, 5))
        assert trades == []

    def test_fok_rejected_insufficient_volume(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 100, 3))
        trades = e.submit(fok(Side.BUY, 100, 5))
        assert trades == []

    def test_fok_rejected_price_no_cross(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 105, 10))
        trades = e.submit(fok(Side.BUY, 100, 5))
        assert trades == []

    def test_fok_sell_rejected_no_liquidity(self):
        e = ContinuousDoubleAuction()
        trades = e.submit(fok(Side.SELL, 100, 5))
        assert trades == []

    def test_fok_sell_rejected_insufficient_volume(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.BUY, 100, 3))
        trades = e.submit(fok(Side.SELL, 100, 5))
        assert trades == []


class TestFOKFills:
    def test_fok_fills_exact_volume(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 100, 5))
        trades = e.submit(fok(Side.BUY, 100, 5))
        assert len(trades) == 1
        assert trades[0].quantity == 5

    def test_fok_fills_when_volume_exceeds_needed(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 100, 10))
        trades = e.submit(fok(Side.BUY, 100, 5))
        assert len(trades) == 1
        assert trades[0].quantity == 5

    def test_fok_fills_across_multiple_levels(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 100, 3))
        e.submit(gtc(Side.SELL, 101, 3))
        trades = e.submit(fok(Side.BUY, 101, 6))
        assert len(trades) == 2
        assert sum(t.quantity for t in trades) == 6

    def test_fok_sell_fills_exact_volume(self):
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.BUY, 100, 5))
        trades = e.submit(fok(Side.SELL, 100, 5))
        assert len(trades) == 1
        assert trades[0].quantity == 5


class TestFOKNeverRests:
    """
    The core invariant that prevents the chained-FOK edge case.
    A FOK that can't fill must not appear as resting liquidity for
    any subsequent order — confirmed by checking the book is unchanged.
    """

    def test_rejected_fok_buy_not_in_book(self):
        e = ContinuousDoubleAuction()
        e.submit(fok(Side.BUY, 100, 5))
        assert e.book.best_bid() == (None, None)

    def test_rejected_fok_sell_not_in_book(self):
        e = ContinuousDoubleAuction()
        e.submit(fok(Side.SELL, 100, 5))
        assert e.book.best_ask() == (None, None)

    def test_rejected_fok_does_not_appear_as_liquidity(self):
        # FOK buy rejected (no sells). A subsequent GTC sell should NOT match it.
        e = ContinuousDoubleAuction()
        e.submit(fok(Side.BUY, 100, 5))
        trades = e.submit(gtc(Side.SELL, 100, 5))
        assert trades == []
        assert e.book.best_ask()[0] == 100  # sell rests, finds no buyer

    def test_partial_volume_fok_not_partially_rested(self):
        # 3 units available, FOK needs 5 → rejected. Book should still have 3 resting.
        e = ContinuousDoubleAuction()
        e.submit(gtc(Side.SELL, 100, 3))
        e.submit(fok(Side.BUY, 100, 5))
        # FOK was rejected: the 3-unit sell must still be intact
        ask_price, ask_id = e.book.best_ask()
        assert ask_price == 100
        assert e.book._index[ask_id].quantity == 3
