from ..engine import MatchingEngine
from ..orderbook.order import Trade, Order, Side, TimeInForce
from uuid import UUID

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

        return not order.quantity == 0 # order has now been satisfied

    def _fok_walk(self, order:Order) -> bool:
        filled_qty = 0
        if order.side is Side.BUY:
            for price, queue in sorted(self.book.asks.items()):# sorted in ascending order
                if price > order.price:
                    break
                for uid in queue:
                    filled_qty += self.book._index[uid].quantity
                    if filled_qty >= order.quantity:
                        return True
        else: # SELL
            for price, queue in sorted(self.book.bids.items(), reverse=True): # sorted in descending order (you want the freater bid)
                if price < order.price:
                    break
                for uid in queue:
                    filled_qty += self.book._index[uid].quantity
                    if filled_qty >= order.quantity:
                        return True
        return False

    def submit(self, order: Order) -> list[Trade]: # for now assumes that the order is marketable
        trades: list[Trade] = []
        # precedence of order is given by 
        market_continue = order.price is None
        limit_continue = not market_continue

        # TODO: implement FOK
        if order.tif is TimeInForce.FOK:
            if not self._fok_walk(order=order):
                return [] # fok gets cancelled (not appended to )
        if order.side is Side.BUY:
            while market_continue or limit_continue:

                best_sell, sell_id = self.book.best_ask()
                sell_ctr = self.book._index.get(sell_id, None) # pull the sell contract, if no id matching then get none (happens when best_sell is None)

                if best_sell is None: # if there is no seller, then put order in the book
                    if order.tif is TimeInForce.GTC:
                        self.book.add(order=order)
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
                    if order.tif is TimeInForce.GTC:
                        self.book.add(order=order) # append the buy contract to the orderbook buy side
                    # if order is IOC or FOK pass
                    break

        else: # Side.sell
            while market_continue or  limit_continue:
                best_buy, buy_id = self.book.best_bid()
                buy_ctr = self.book._index.get(buy_id, None) # pull the sell contract, if no id matching then get none (happens when best_sell is None)

                if best_buy is None: # if there is no seller, then put order in the book
                    if order.tif is TimeInForce.GTC:
                        self.book.add(order=order)
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
                    if order.tif is TimeInForce.GTC:
                        self.book.add(order=order) # append the buy contract to the orderbook buy side
                    # if the order is FOK or IOC, no add remaining qty to book
                    break

        self.trades.extend(trades)
        return trades