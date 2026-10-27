from abc import ABC, abstractmethod
from ..orderbook.order import Order, Trade


class PricingRule(ABC):
    """
    Given the orders collected during a call auction, decide the clearing
    price(s) and how each order fills. Shared crossing logic (build cumulative
    demand & supply, find the volume-maximising price, ration the long side)
    can live in a helper both subclasses call.
    """

    def validate(self, order: Order) -> None:
        """Raise ValueError if this rule cannot clear `order`.

        Called when the order is submitted, so a bad order is refused up front
        instead of sitting in the batch and failing every later run().
        """

    @abstractmethod
    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]:
        ...
