from ..pricing_rule import PricingRule
from ..fok import fok_helper
from python_engine.orderbook.order import Order, Trade, TimeInForce

class Discriminatory(PricingRule):

    def validate(self, order: Order) -> None:
        # Pay-as-bid prices every trade at a limit price, so an order without
        # one has nothing to pay. Use Uniform for market orders.
        if order.price is None:
            raise ValueError("Discriminatory clearing requires limit orders, got a market order")

    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]: # priority = price, then time, reamining order go into the continous auction
        for o in buys:
            self.validate(o)
        for o in sells:
            self.validate(o)
        # buys must be sorted most expensive on top (price descending)
        sorted_buys = sorted(buys, key=lambda o:(-o.price, o.priority)) # + ascending, - descending

        # sells must be sorted cheapest top (price ascending)
        sorted_sells = sorted(sells, key=lambda o:(o.price, o.priority))

        trades: list[Trade]=[]
        # implemnet GTC first 
        buy_order = sorted_buys.pop(0) if sorted_buys else None
        sell_order = sorted_sells.pop(0) if sorted_sells else None
        while buy_order is not None and sell_order is not None:

            buy_order = fok_helper(buy_order, sorted_buys, [sell_order] + sorted_sells)
            if buy_order is None:
                break
            sell_order = fok_helper(sell_order, sorted_sells, [buy_order] + sorted_buys)
            if sell_order is None:
                break

            b_id = buy_order.order_id
            s_id = sell_order.order_id

            if buy_order.price < sell_order.price: # order cannot be filled because they are beyond the limit (buy would go over and sell would go under the limit)
                break
            else: # decide price based on time priority (price becomes the resting order price -> the oldest order price)
                resting_price = buy_order.price if buy_order.priority < sell_order.priority else sell_order.price
    
            if buy_order.quantity == sell_order.quantity:
                resting_quantity = sell_order.quantity
                buy_order.quantity = sell_order.quantity = 0
                buy_order = sorted_buys.pop(0) if sorted_buys else None
                sell_order = sorted_sells.pop(0) if sorted_sells else None
            elif buy_order.quantity < sell_order.quantity:
                resting_quantity = buy_order.quantity
                sell_order.quantity -= resting_quantity
                buy_order.quantity = 0
                buy_order = sorted_buys.pop(0) if sorted_buys else None
            else:
                resting_quantity = sell_order.quantity
                buy_order.quantity -= resting_quantity
                sell_order.quantity = 0
                sell_order = sorted_sells.pop(0) if sorted_sells else None

            trade = Trade(
                price=resting_price,
                quantity=resting_quantity,
                buy_order_id=b_id,
                sell_order_id=s_id
            )
            trades.append(trade)
        
        unfilled = []
        unfilled.extend(o for o in sorted_sells if o.tif is TimeInForce.GTC) # IOC and FOK never roll over
        unfilled.extend(o for o in sorted_buys if o.tif is TimeInForce.GTC)
        if buy_order is not None and buy_order.quantity > 0: # must check if it is GTC or IOC
            if buy_order.tif is TimeInForce.GTC:
                unfilled.append(buy_order)
        if sell_order is not None and sell_order.quantity > 0: # must check if it is GTC or IOC
            if sell_order.tif is TimeInForce.GTC:
                unfilled.append(sell_order)
                
        # return all the trades and the unfilled trades
        return trades, unfilled
