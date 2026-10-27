from __future__ import annotations

from .order import Order, Side
from collections import deque
from sortedcontainers import SortedDict

# price is a always an int of tickers

class OrderBook:
    """
    Levels are SortedDicts, the counterpart of the C++ std::map: iteration is
    in ascending price order and the best level is the first or last key, so
    best_bid/best_ask are O(1) instead of a max()/min() scan over every level.
    """

    def __init__(self, instrument: str) -> None:
        self.instrument: str = instrument
        self.bids: SortedDict[int, deque[int]] = SortedDict()     # price -> resting orders queue (bids are buying)
        self.asks: SortedDict[int, deque[int]] = SortedDict()     # asks are selling
        self._index: dict[int, Order] = {} # order_id -> Order (used for cancels)

    def _side_book(self, side: Side) -> SortedDict:
        return self.bids if side is Side.BUY else self.asks

    def add(self, order: Order) -> None:
        """Rest an order. Caller guarantees it is non-marketable right now."""
        if order.price is None: # market orders have no level to rest on, so they never rest
            return
        if order.order_id in self._index:
            raise ValueError(f"order {order.order_id} is already resting")
        level = self._side_book(order.side)
        queue = level.get(order.price)
        if queue is not None:
            queue.append(order.order_id) # append order id to the queue
        else:
            level[order.price] = deque([order.order_id]) # if the levels doesn't exist create a new one
        self._index[order.order_id] = order # saves element to order map for quick cancelling, avoid saving order twice

    def delete_level(self, side: Side, price: int) -> None:
        del self._side_book(side)[price]

    def cancel(self, order_id: int) -> Order: # cancels order form and index and level, cancels level if empty
        order = self._index.pop(order_id) # delete order form the index map, KeyError if unknown

        level = self._side_book(order.side)[order.price]
        if level[0] == order_id:
            level.popleft() # the common case: the matcher exhausts the front of the queue
        else:
            level.remove(order_id) # user cancel from anywhere in the queue
        if not level: # if level is empty delete it
            self.delete_level(order.side, order.price)
        return order

    def best_bid(self) -> tuple[int, int] | tuple[None, None]: # highest price trader is willing to buy
        if not self.bids:
            return None, None
        price, queue = self.bids.peekitem(-1)
        return price, queue[0]

    def best_ask(self) -> tuple[int, int] | tuple[None, None]: # lowest price market is willing to sell
        if not self.asks:
            return None, None
        price, queue = self.asks.peekitem(0)
        return price, queue[0]
