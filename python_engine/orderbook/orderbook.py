from .order import Symbol, Order, Trade, Side
from collections import deque
from uuid import UUID

# price is a always an int of tickers

class OrderBook:

    def __init__(self, instrument: Symbol) -> None:
        self.instrument: Symbol = instrument
        self.bids: dict[int, deque[UUID]] = {}     # price -> resting orders queue (bids are buying)
        self.asks: dict[int, deque[UUID]] = {}     # asks are selling
        self._index: dict[UUID, Order] = {} # order_id -> Order (used for cancels)

    def _side_book(self, side: Side) -> dict[float, deque[Order]]:
        return self.bids if side is Side.BUY else self.asks

    def add(self, order: Order) -> None:
        """Rest an order. Caller guarantees it is non-marketable right now."""
        if order.price is not None: #  guarantees that no market IOC or GTC orders get appenede (would throw keyError when key=None)
            level = self._side_book(order.side) # setdeault either appends 
            if order.price in level.keys():
                level[order.price].append(order.order_id) # append order id to the queue
            else:
                level[order.price] = deque([order.order_id]) # if the levels doesn't exist create a new one
            self._index[order.order_id] = order # saves element to order map for quick cancelling, avoid saving order twice

    def delete_level(self, side: Side, price: int) -> None:
        if side is Side.BUY:
            self.bids.pop(price)
        else:
            self.asks.pop(price)

    def cancel(self, order_id: UUID) -> Order | None: # cancels order form and index and level, cancels level if empty
        order = self._index.pop(order_id) # delete order form the index map

        level = self._side_book(order.side)[order.price]
        level.popleft() # delete the order from level, it is guaranteed to be the top most order
        if not level: # if level is empty delete it
            self.delete_level(order.side, order.price)

    def best_bid(self) -> tuple[int, UUID] | tuple[None, None]: # highest price trader is willing to buy
        return (max(self.bids), self.bids[max(self.bids)][0]) if self.bids else (None, None)

    def best_ask(self) -> tuple[int, UUID] | tuple[None, None]: # lowest price market is willing to sell
        return (min(self.asks), self.asks[min(self.asks)][0])  if self.asks else (None, None)
    
