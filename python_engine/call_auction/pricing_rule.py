from abc import ABC, abstractmethod
from ..orderbook.order import Order, Trade


class PricingRule(ABC):
    """
    Given the orders collected during a call auction, decide the clearing
    price(s) and how each order fills. Shared crossing logic (build cumulative
    demand & supply, find the volume-maximising price, ration the long side)
    can live in a helper both subclasses call.
    """

    @abstractmethod
    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]:
        ...