from ..engine import MatchingEngine
from datetime import datetime

from ..orderbook.order import Trade, Order, Side, TimeInForce

class ContinuousDoubleAuction(MatchingEngine):
    """
    Each incoming order matches immediately against the best opposing resting
    orders by price-time priority. Trades print at the *resting* order's price,
    so execution is inherently discriminatory (different fills, different
    prices). Any GTC remainder rests.
    """

    def _gtc_helper(self, best_price: int, ctr: Order, order: Order, trades: list[Trade]) -> bool:
        quantity = order.quantity if order.quantity <= ctr.quantity else ctr.quantity
        if order.side is Side.BUY:
            trade = Trade(
                price = best_price,
                quantity=quantity,
                buy_order_id=order.order_id,
                sell_order_id=ctr.order_id,
            )
        else: # sell side just flip the sell and buycontract
            trade = Trade(
                price = best_price,
                quantity=quantity,
                buy_order_id=ctr.order_id,
                sell_order_id=order.order_id,
            )
        trades.append(trade)

        order.quantity = order.quantity-quantity
        ctr.quantity = ctr.quantity - quantity
        if ctr.quantity == 0: # if sell order has been exhausted then cancel it
            self.book.cancel(ctr.order_id)

        return order.quantity != 0 # False once the order has been satisfied

    def _fok_walk(self, order:Order) -> bool:
        filled_qty = 0
        # a market FOK has no limit: every resting order on the other side counts
        if order.side is Side.BUY:
            for price, queue in self.book.asks.items(): # SortedDict: already ascending
                if order.price is not None and price > order.price:
                    break
                for uid in queue:
                    filled_qty += self.book._index[uid].quantity
                    if filled_qty >= order.quantity:
                        return True
        else: # SELL
            for price, queue in reversed(self.book.bids.items()): # descending (you want the greater bid)
                if order.price is not None and price < order.price:
                    break
                for uid in queue:
                    filled_qty += self.book._index[uid].quantity
                    if filled_qty >= order.quantity:
                        return True
        return False

    def _rest(self, order: Order) -> None:
        # Only a GTC limit order rests. A market order has no price level to
        # rest on, so its unfilled remainder is cancelled like an IOC's.
        if order.tif is TimeInForce.GTC and order.price is not None:
            self.book.add(order=order)

    def submit(self, order: Order) -> list[Trade]: # for now assumes that the order is marketable
        if order.order_id in self.book._index:
            raise ValueError(f"order {order.order_id} is already resting in the book")
        if order.quantity <= 0: # e.g. resubmitting an order that already filled
            raise ValueError(f"order {order.order_id} has no quantity left to fill")
        trades: list[Trade] = []
        # precedence of order is given by 
        market_continue = order.price is None
        limit_continue = not market_continue

        if order.tif is TimeInForce.FOK:
            if not self._fok_walk(order=order):
                return [] # fok gets cancelled (not appended to )
        if order.side is Side.BUY:
            while market_continue or limit_continue:

                best_sell, sell_id = self.book.best_ask()
                sell_ctr = self.book._index.get(sell_id, None) # pull the sell contract, if no id matching then get none (happens when best_sell is None)

                if best_sell is None: # if there is no seller, then put order in the book
                    self._rest(order)
                    break
                elif order.price is None: # use the best market price, else you must meet the order.price ask/bid
                    market_continue = self._gtc_helper(
                        best_price=best_sell,
                        ctr=sell_ctr,
                        order=order,
                        trades=trades
                    )
                elif best_sell <= order.price: # somone is offering to sell for less than what you are asking, take the qty
                    limit_continue = self._gtc_helper(
                        best_price=best_sell,
                        ctr=sell_ctr,
                        order=order,
                        trades=trades
                    )
                else: # limit order and best_sell > order.price
                    self._rest(order) # append the buy contract to the orderbook buy side
                    # if order is IOC or FOK pass
                    break

        else: # Side.sell
            while market_continue or  limit_continue:
                best_buy, buy_id = self.book.best_bid()
                buy_ctr = self.book._index.get(buy_id, None) # pull the sell contract, if no id matching then get none (happens when best_sell is None)

                if best_buy is None: # if there is no seller, then put order in the book
                    self._rest(order)
                    break
                elif order.price is None: # use the best market price, else you must meet the order.price ask/bid
                    market_continue = self._gtc_helper(
                        best_price=best_buy,
                        ctr=buy_ctr,
                        order=order,
                        trades=trades
                    )
                elif best_buy >= order.price: # somone is offering to buy for more than what you are asking, take the qty
                    limit_continue = self._gtc_helper(
                        best_price=best_buy,
                        ctr=buy_ctr,
                        order=order,
                        trades=trades
                    )
                else: # limit order and best_buy > order.price
                    self._rest(order) # append the sell contract to the orderbook sell side
                    # if the order is FOK or IOC, no add remaining qty to book
                    break

        self.trades.extend(trades)
        return trades

    def modify(self, order_id: int, new_price: int, new_quantity: int,
               timestamp: datetime | None = None) -> list[Trade]:
        """Amend a resting order.

        Shrinking it at the same price keeps its place in the queue. Any other
        change (a new price, or more quantity) loses time priority: the order
        leaves the book and is submitted again, so a new price that crosses
        trades immediately. KeyError if the id is not resting, ValueError if
        new_quantity is not positive.
        """
        order = self.book._index[order_id] # KeyError if not resting
        if new_quantity <= 0:
            raise ValueError(f"modify needs a positive quantity, got {new_quantity}; use cancel to remove an order")
        if new_price == order.price and new_quantity <= order.quantity:
            order.quantity = new_quantity # a reduction keeps priority
            return []
        self.book.cancel(order_id)
        order.price = new_price
        order.quantity = new_quantity
        order.renew(timestamp)
        return self.submit(order)