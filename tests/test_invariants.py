"""
Randomised invariant tests.

Instead of asserting specific outputs, these generate many small seeded
scenarios and check properties every correct engine must keep:

  - a FOK order trades its full quantity or nothing
  - IOC / FOK orders never roll into the next batch or rest in the book
  - no order trades more than it asked for
  - a GTC order is either fully traded or returned with its exact remainder
  - every trade price respects both limits
  - uniform: one price per batch, and it maximises executed volume
  - continuous: the book stays consistent and never crossed

Seeds are fixed, so a failure is reproducible: rerun with `-k <seed>`.
"""
import random
from collections import defaultdict
from datetime import datetime, timedelta

import pytest

from python_engine.call_auction.rules.discriminatory import Discriminatory
from python_engine.call_auction.rules.uniform import Uniform
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.orderbook.order import Order, Side, TimeInForce

T0 = datetime(2026, 1, 1, 9, 0, 0)
TIFS = [TimeInForce.GTC, TimeInForce.IOC, TimeInForce.FOK]


def random_order(rng, side, allow_market):
    price = None if allow_market and rng.random() < 0.15 else rng.randint(95, 105)
    return Order(side=side, price=price, quantity=rng.randint(1, 10), trader_id=1,
                 tif=rng.choice(TIFS),
                 # coarse timestamps on purpose: ties must be handled too
                 timestamp=T0 + timedelta(seconds=rng.randint(0, 5)))


def random_batch(rng, allow_market):
    buys = [random_order(rng, Side.BUY, allow_market) for _ in range(rng.randint(0, 6))]
    sells = [random_order(rng, Side.SELL, allow_market) for _ in range(rng.randint(0, 6))]
    return buys, sells


def traded_by_order(trades):
    traded = defaultdict(int)
    for t in trades:
        traded[t.buy_order_id] += t.quantity
        traded[t.sell_order_id] += t.quantity
    return traded


def check_batch_invariants(buys, sells, trades, unfilled, original_qty):
    by_id = {o.order_id: o for o in buys + sells}
    traded = traded_by_order(trades)
    unfilled_ids = [o.order_id for o in unfilled]

    assert len(unfilled_ids) == len(set(unfilled_ids)), "order returned twice"
    remaining = {o.order_id: o.quantity for o in unfilled}

    for t in trades:
        assert t.quantity > 0
        b, s = by_id[t.buy_order_id], by_id[t.sell_order_id]
        assert b.side is Side.BUY and s.side is Side.SELL
        assert b.price is None or t.price <= b.price, "buy filled above its limit"
        assert s.price is None or t.price >= s.price, "sell filled below its limit"

    for oid, o in by_id.items():
        q0 = original_qty[oid]
        assert traded[oid] <= q0, "overfilled"
        if o.tif is TimeInForce.FOK:
            assert traded[oid] in (0, q0), f"FOK partially filled: {traded[oid]}/{q0}"
        if o.tif is not TimeInForce.GTC:
            assert oid not in remaining, f"{o.tif.name} rolled into next batch"
        else:
            assert traded[oid] + remaining.get(oid, 0) == q0, "GTC volume lost or invented"


def tradeable_volume(buys, sells, p, qty):
    demand = sum(qty[o.order_id] for o in buys if o.price is None or o.price >= p)
    supply = sum(qty[o.order_id] for o in sells if o.price is None or o.price <= p)
    return min(demand, supply)


@pytest.mark.parametrize("seed", range(400))
def test_uniform_invariants(seed):
    rng = random.Random(seed)
    buys, sells = random_batch(rng, allow_market=True)
    original_qty = {o.order_id: o.quantity for o in buys + sells}

    trades, unfilled = Uniform().clear(list(buys), list(sells))

    check_batch_invariants(buys, sells, trades, unfilled, original_qty)
    assert len({t.price for t in trades}) <= 1, "uniform batch printed several prices"

    # Without FOKs (whose all-or-nothing rule legitimately removes volume),
    # p* must achieve the best tradeable volume over the limit prices.
    if not any(o.tif is TimeInForce.FOK for o in buys + sells):
        limit_prices = {o.price for o in buys + sells if o.price is not None}
        best = max((tradeable_volume(buys, sells, p, original_qty) for p in limit_prices), default=0)
        assert sum(t.quantity for t in trades) == best


@pytest.mark.parametrize("seed", range(400))
def test_discriminatory_invariants(seed):
    rng = random.Random(seed)
    buys, sells = random_batch(rng, allow_market=False)
    original_qty = {o.order_id: o.quantity for o in buys + sells}

    trades, unfilled = Discriminatory().clear(list(buys), list(sells))

    check_batch_invariants(buys, sells, trades, unfilled, original_qty)


def check_book(book):
    queued = []
    for side_book, side in ((book.bids, Side.BUY), (book.asks, Side.SELL)):
        for price, level in side_book.items():
            assert level, "empty price level left behind"
            for oid in level:
                o = book._index[oid]
                assert o.side is side and o.price == price and o.quantity > 0
                assert o.tif is TimeInForce.GTC
                queued.append(oid)
    assert len(queued) == len(set(queued)) == len(book._index), "levels and index disagree"
    if book.bids and book.asks:
        assert max(book.bids) < min(book.asks), "book left crossed"


@pytest.mark.parametrize("seed", range(300))
def test_continuous_invariants(seed):
    rng = random.Random(seed)
    e = ContinuousDoubleAuction()
    all_orders = {}
    original_qty = {}
    resting_prices = {}  # what a resting order's limit was when it was hit

    for _ in range(40):
        o = random_order(rng, rng.choice([Side.BUY, Side.SELL]), allow_market=True)
        all_orders[o.order_id] = o
        original_qty[o.order_id] = o.quantity
        resting_prices.update({oid: r.price for oid, r in e.book._index.items()})

        trades = e.submit(o)

        filled = sum(t.quantity for t in trades)
        if o.tif is TimeInForce.FOK:
            assert filled in (0, original_qty[o.order_id]), "FOK partially filled"
        for t in trades:
            maker = t.sell_order_id if o.side is Side.BUY else t.buy_order_id
            assert t.price == resting_prices[maker], "trade not at the resting price"
            if o.price is not None:
                assert (t.price <= o.price) if o.side is Side.BUY else (t.price >= o.price)
        if o.order_id in e.book._index:
            assert o.tif is TimeInForce.GTC and o.price is not None
            assert o.quantity == original_qty[o.order_id] - filled
        check_book(e.book)

    # Over the whole run, nothing overfilled.
    traded = traded_by_order(e.trades)
    for oid, q in traded.items():
        assert q <= original_qty[oid]
