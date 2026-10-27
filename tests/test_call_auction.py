from python_engine.call_auction.call_auction import CallAuction
from python_engine.call_auction.pricing_rule import PricingRule
from python_engine.orderbook.order import Order, Side, Trade

TRADER = 1


def lmt(side, price, qty):
    return Order(side=side, price=price, quantity=qty, trader_id=TRADER)


class _NoopPricing(PricingRule):
    def clear(self, buys, sells):
        return [], []


class _CapturePricing(PricingRule):
    def __init__(self):
        self.calls = []

    def clear(self, buys, sells):
        self.calls.append((list(buys), list(sells)))
        return [], []


class TestCollection:
    def test_submit_returns_empty_immediately(self):
        e = CallAuction(pricing=_NoopPricing())
        assert e.submit(lmt(Side.BUY, 100, 10)) == []

    def test_submit_collects_multiple_orders(self):
        e = CallAuction(pricing=_NoopPricing())
        e.submit(lmt(Side.BUY, 100, 10))
        e.submit(lmt(Side.SELL, 99, 5))
        assert len(e._pending) == 2

    def test_no_trades_until_run(self):
        e = CallAuction(pricing=_NoopPricing())
        e.submit(lmt(Side.SELL, 99, 10))
        e.submit(lmt(Side.BUY, 105, 10))
        assert e.trades == []


class TestRun:
    def test_run_clears_pending(self):
        e = CallAuction(pricing=_NoopPricing())
        e.submit(lmt(Side.BUY, 100, 10))
        e.run()
        assert e._pending == []

    def test_run_passes_correct_sides_to_pricing(self):
        pricing = _CapturePricing()
        e = CallAuction(pricing=pricing)
        b = lmt(Side.BUY, 100, 10)
        s = lmt(Side.SELL, 99, 10)
        e.submit(b)
        e.submit(s)
        e.run()
        assert len(pricing.calls) == 1
        buys, sells = pricing.calls[0]
        assert b in buys and s in sells

    def test_run_returns_trades_from_pricing(self):
        dummy = Trade(price=100, quantity=5, buy_order_id=1, sell_order_id=2)

        class _FixedPricing(PricingRule):
            def clear(self, buys, sells):
                return [dummy], []

        e = CallAuction(pricing=_FixedPricing())
        e.submit(lmt(Side.BUY, 100, 5))
        e.submit(lmt(Side.SELL, 99, 5))
        result = e.run()
        assert result == [dummy]
        assert dummy in e.trades

    def test_run_on_empty_book_returns_empty(self):
        e = CallAuction(pricing=_NoopPricing())
        assert e.run() == []

    def test_run_can_be_called_multiple_times(self):
        pricing = _CapturePricing()
        e = CallAuction(pricing=pricing)
        e.submit(lmt(Side.BUY, 100, 5))
        e.run()
        e.submit(lmt(Side.SELL, 99, 5))
        e.run()
        assert len(pricing.calls) == 2
        assert e._pending == []
