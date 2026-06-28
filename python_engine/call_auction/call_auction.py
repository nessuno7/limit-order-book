from ..engine import MatchingEngine
from ..orderbook.order import Order, Trade, Side
from .pricing_rule import PricingRule


class CallAuction(MatchingEngine):
    """
    Collects orders without matching, then crosses them all at once when run()
    is called. The pricing rule is injected, so the SAME class supports both
    uniform and discriminatory clearing — just swap the strategy.
    """

    def __init__(self, pricing: PricingRule) -> None:
        super().__init__()
        self.pricing = pricing
        self._pending: list[Order] = []

    def submit(self, order: Order) -> list[Trade]:
        self._pending.append(order)   # collect only; no immediate match
        # timestamp for the order is when the Order was created and submitted to the batch, not when the batch was crossed -> a maker always exists so price or trade become the Maker's price
        return []

    def run(self) -> list[Trade]:
        buys = [o for o in self._pending if o.side is Side.BUY]
        sells = [o for o in self._pending if o.side is Side.SELL]
        trades, unfilled = self.pricing.clear(buys, sells)
        self.trades.extend(trades)
        self._pending.clear()
        # uinfilled order get moved to next bacth
        self._pending.extend(unfilled)
        return trades