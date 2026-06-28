"""
engine.py — matching-engine core for the limit-order-book project.

Design in one sentence:
    An OrderBook holds resting orders; a MatchingEngine decides the *rule* by
    which an incoming order matches against it. Every market type (continuous,
    uniform-price call auction, discriminatory call auction) is just a
    different MatchingEngine, so they all share ONE interface.

Two independent axes live in this file:
    timing  : continuous matching        vs.  call auction (batch then cross)
    pricing : uniform (single price)      vs.  discriminatory (pay-as-bid)

Pricing is a pluggable strategy (PricingRule) so a single CallAuction class
supports both. The genuinely instructive algorithms are left as TODOs on
purpose — this is your project to build.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from .orderbook.orderbook import OrderBook
from .orderbook.order import Trade, Order

class MatchingEngine(ABC):
    def __init__(self) -> None:
        self.book = OrderBook("TEST")
        self.trades: list[Trade] = []

    @abstractmethod
    def submit(self, order: Order) -> list[Trade]:
        """Accept one order; return the trades it generated (maybe empty)."""
        ...



if __name__ == "__main__":
    # Usage sketch — same interface, three markets:
    ...
