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
        self._pending_ids: set[int] = set() # O(1) duplicate check; a list scan made submit() O(batch)

    def submit(self, order: Order) -> list[Trade]:
        if order.order_id in self._pending_ids:
            raise ValueError(f"order {order.order_id} is already in this batch")
        if order.quantity <= 0: # e.g. resubmitting an order that already filled
            raise ValueError(f"order {order.order_id} has no quantity left to fill")
        self.pricing.validate(order) # refuse what the rule cannot clear
        self._pending.append(order)   # collect only; no immediate match
        self._pending_ids.add(order.order_id)
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
        self._pending_ids = {o.order_id for o in self._pending}
        return trades
