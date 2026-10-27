"""
FOK tests for Discriminatory call auction pricing rule.

Unlike the continuous auction, call auctions collect all orders before crossing.
This means FOK orders sit in the pending list alongside other FOKs, creating a
genuine edge case: a FOK may appear to have enough opposing volume, but that
volume comes from another FOK that itself cannot be filled.

The chained-FOK scenarios below test exactly this.
"""
from datetime import datetime, timedelta
from python_engine.call_auction.rules.discriminatory import Discriminatory
from python_engine.orderbook.order import Order, Side, TimeInForce

TRADER = 1
T0 = datetime(2026, 1, 1, 9, 0, 0)


def gtc(side, price, qty, t_offset_s=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.GTC,
                 timestamp=T0 + timedelta(seconds=t_offset_s))


def fok(side, price, qty, t_offset_s=0):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER,
                 tif=TimeInForce.FOK,
                 timestamp=T0 + timedelta(seconds=t_offset_s))


class TestFOKBaseline:
    """Basic FOK behaviour before chained cases."""

    def test_fok_buy_fills_against_gtc_sell(self):
        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok(Side.BUY, 105, 5)],
            sells=[gtc(Side.SELL, 100, 5)],
        )
        assert len(trades) == 1
        assert trades[0].quantity == 5

    def test_fok_buy_rejected_insufficient_gtc_volume(self):
        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok(Side.BUY, 105, 5)],
            sells=[gtc(Side.SELL, 100, 3)],
        )
        assert trades == []

    def test_fok_sell_fills_against_gtc_buy(self):
        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[gtc(Side.BUY, 105, 5)],
            sells=[fok(Side.SELL, 100, 5)],
        )
        assert len(trades) == 1
        assert trades[0].quantity == 5

    def test_fok_sell_rejected_insufficient_gtc_volume(self):
        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[gtc(Side.BUY, 105, 3)],
            sells=[fok(Side.SELL, 100, 5)],
        )
        assert trades == []


class TestChainedFOKCanFill:
    """
    FOK A uses volume from FOK B. FOK B itself CAN be fully filled by
    combining what A provides and additional GTC orders. Both should trade.
    """

    def test_fok_buy_uses_fok_sell_that_can_fill(self):
        # FOK BUY A qty=4 needs 4 units.
        # FOK SELL B qty=10: A provides 4, GTC BUY C provides 6 → B fills (4+6=10). ✓
        # So A may use B's volume.
        fok_buy_a  = fok(Side.BUY,  105, 4, t_offset_s=2)
        fok_sell_b = fok(Side.SELL, 100, 10, t_offset_s=1)
        gtc_buy_c  = gtc(Side.BUY,  105, 6, t_offset_s=3)

        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok_buy_a, gtc_buy_c],
            sells=[fok_sell_b],
        )
        total_qty = sum(t.quantity for t in trades)
        assert total_qty == 10  # all volume traded
        assert unfilled == []

    def test_fok_sell_uses_fok_buy_that_can_fill(self):
        # Mirror of above: FOK SELL A qty=4, FOK BUY B qty=10, GTC SELL C qty=6.
        fok_sell_a = fok(Side.SELL, 100, 4, t_offset_s=2)
        fok_buy_b  = fok(Side.BUY,  105, 10, t_offset_s=1)
        gtc_sell_c = gtc(Side.SELL, 100,  6, t_offset_s=3)

        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok_buy_b],
            sells=[fok_sell_a, gtc_sell_c],
        )
        total_qty = sum(t.quantity for t in trades)
        assert total_qty == 10
        assert unfilled == []


class TestChainedFOKCannotFill:
    """
    THE EDGE CASE: FOK A's available volume includes FOK B, but FOK B cannot
    itself be fully filled. Without the recursive check, A would incorrectly
    see enough volume and proceed — violating B's all-or-nothing contract.
    With the recursive check, A is correctly rejected.
    """

    def test_fok_buy_rejected_when_fok_sell_cannot_fill(self):
        # FOK BUY A qty=4.
        # Available sells: GTC SELL D qty=2 + FOK SELL B qty=10.
        # Naive sum: 2+10=12 >= 4 → would accept A. WRONG.
        # FOK B needs 10: gets 2 from A (remaining after D) + GTC BUY C qty=5 = 7 < 10. B can't fill.
        # So B's volume is not reliable. A can only use D's 2 units → 2 < 4. A rejected.
        fok_buy_a  = fok(Side.BUY,  105, 4,  t_offset_s=2)
        fok_sell_b = fok(Side.SELL, 100, 10, t_offset_s=1)
        gtc_sell_d = gtc(Side.SELL, 100, 2,  t_offset_s=0)  # older, comes first
        gtc_buy_c  = gtc(Side.BUY,  105, 5,  t_offset_s=3)  # not enough to fill B (2+5=7 < 10)

        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok_buy_a, gtc_buy_c],
            sells=[gtc_sell_d, fok_sell_b],
        )
        # A must be rejected. Only GTC orders may trade.
        fok_a_trade = [t for t in trades if t.buy_order_id == fok_buy_a.order_id]
        assert fok_a_trade == []

    def test_fok_sell_rejected_when_fok_buy_cannot_fill(self):
        # Mirror: FOK SELL A qty=4.
        # Available buys: GTC BUY D qty=2 + FOK BUY B qty=10.
        # FOK B needs 10: gets 2 from A + GTC SELL C qty=5 = 7 < 10. B can't fill.
        # A can only use D's 2 → 2 < 4. A rejected.
        fok_sell_a = fok(Side.SELL, 100, 4,  t_offset_s=2)
        fok_buy_b  = fok(Side.BUY,  105, 10, t_offset_s=1)
        gtc_buy_d  = gtc(Side.BUY,  105, 2,  t_offset_s=0)
        gtc_sell_c = gtc(Side.SELL, 100, 5,  t_offset_s=3)

        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok_buy_b, gtc_buy_d],
            sells=[fok_sell_a, gtc_sell_c],
        )
        fok_a_trade = [t for t in trades if t.sell_order_id == fok_sell_a.order_id]
        assert fok_a_trade == []

    def test_gtc_still_trades_when_fok_chain_fails(self):
        # When the chained FOK is rejected, GTC orders that CAN cross should still trade.
        fok_buy_a  = fok(Side.BUY,  105, 4,  t_offset_s=2)
        fok_sell_b = fok(Side.SELL, 100, 10, t_offset_s=1)
        gtc_sell_d = gtc(Side.SELL, 100, 2,  t_offset_s=0)
        gtc_buy_c  = gtc(Side.BUY,  105, 5,  t_offset_s=3)

        d = Discriminatory()
        trades, unfilled = d.clear(
            buys=[fok_buy_a, gtc_buy_c],
            sells=[gtc_sell_d, fok_sell_b],
        )
        # GTC buy C (qty=5) and GTC sell D (qty=2) should still match.
        gtc_qty = sum(t.quantity for t in trades)
        assert gtc_qty > 0
