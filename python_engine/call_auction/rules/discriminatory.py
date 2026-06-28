from ..pricing_rule import PricingRule
from python_engine.orderbook.order import Order, Trade, TimeInForce, Side
from uuid import UUID

class Discriminatory(PricingRule):

    def _fok_walk(self, order: Order, sorted_list:list[Order], opp_sorted_list: list[Order], orders_to_ignore: dict[UUID, Order] = None, partially_filled_orders: dict[UUID, int] = None,starting_vol: int = 0): # no need to check for FOK as they canno be resting
        if orders_to_ignore is None: orders_to_ignore = {}
        if partially_filled_orders is None: partially_filled_orders = {}
        volume = starting_vol # here I do need a recurrent FOK check
        compare = lambda o1, o2: (o1.price >= o2.price) if o1.side is Side.BUY else (o2.price >= o1.price)

        
        for opp_order in opp_sorted_list:
            if opp_order.order_id not in orders_to_ignore: # check that order shoud not be ignored
                if opp_order.tif is TimeInForce.FOK:
                    if opp_order.quantity > order.quantity - volume: # must check first if the opp FOK order quantity is greater than the remaing qty to be filled
                        orders_to_ignore[order.order_id] = order
                        if self._fok_walk(
                            order=opp_order,
                            sorted_list=opp_sorted_list,
                            opp_sorted_list=sorted_list,
                            orders_to_ignore=orders_to_ignore,
                            partially_filled_orders=partially_filled_orders,
                            starting_vol=order.quantity-volume
                        ):
                            return True
                        else:
                            orders_to_ignore[opp_order.order_id] = opp_order # append to order to ignore because it canno be filled
                            continue # skips the current 

                if compare(order, opp_order): # tehre are still below the buy limit
                    if opp_order.order_id in partially_filled_orders:
                        opp_order_quantity = partially_filled_orders[opp_order.order_id]
                        if order.quantity - volume < opp_order_quantity:
                            partially_filled_orders[opp_order.order_id] = opp_order_quantity - (order.quantity - volume) # replace volue with leftover volume
                        else:
                            del partially_filled_orders[opp_order.order_id]
                            orders_to_ignore[opp_order.order_id] = opp_order
                        volume += opp_order_quantity
                    else:
                        if order.quantity - volume < opp_order.quantity:
                            partially_filled_orders[opp_order.order_id] = opp_order.quantity - (order.quantity - volume) # replace volue with leftover volume
                        else:
                            orders_to_ignore[opp_order.order_id] = opp_order
                        volume += opp_order.quantity
                    if volume >= order.quantity:
                        return True
                else:
                    return False
        return False

    def _fok_helper(self, order:Order, sorted_list: list[Order], opp_sorted_list: list[Order]) -> Order:
        while order.tif is TimeInForce.FOK:
            if not self._fok_walk(order, sorted_list=sorted_list, opp_sorted_list=opp_sorted_list):
                order = sorted_list.pop(0) if sorted_list else None
            else:
                return order
            if order is None:
                return order

        return order
            

    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]: # priority = price, then time, reamining order go into the continous auction
        # buys must be sorted most expensive on top (price descending)
        sorted_buys = sorted(buys, key=lambda o:(-o.price, o.timestamp.timestamp())) # + ascending, - descending
        
        # sells must be sorted cheapest top (price ascending)
        sorted_sells = sorted(sells, key=lambda o:(o.price, o.timestamp.timestamp())) 

        trades: list[Trade]=[]
        # implemnet GTC first 
        buy_order = sorted_buys.pop(0) if sorted_buys else None
        sell_order = sorted_sells.pop(0) if sorted_sells else None
        while buy_order is not None and sell_order is not None:

            buy_order = self._fok_helper(buy_order, sorted_buys, [sell_order] + sorted_sells)
            if buy_order is None:
                break
            sell_order = self._fok_helper(sell_order, sorted_sells, [buy_order] + sorted_buys)
            if sell_order is None:
                break

            b_id = buy_order.order_id
            s_id = sell_order.order_id

            if buy_order.price < sell_order.price: # order cannot be filled because they are beyond the limit (buy would go over and sell would go under the limit)
                break
            else: # decide price based on time priority (price becomes the resting order price -> the oldest order price)
                resting_price = buy_order.price if buy_order.timestamp < sell_order.timestamp else sell_order.price
    
            if buy_order.quantity == sell_order.quantity:
                resting_quantity = sell_order.quantity
                buy_order = sorted_buys.pop(0) if sorted_buys else None
                sell_order = sorted_sells.pop(0) if sorted_sells else None
            elif buy_order.quantity < sell_order.quantity:
                resting_quantity = buy_order.quantity
                sell_order.quantity -= resting_quantity
                buy_order = sorted_buys.pop(0) if sorted_buys else None
            else:
                resting_quantity = sell_order.quantity
                buy_order.quantity -= resting_quantity
                sell_order = sorted_sells.pop(0) if sorted_sells else None

            trade = Trade(
                price=resting_price,
                quantity=resting_quantity,
                buy_order_id=b_id,
                sell_order_id=s_id
            )
            trades.append(trade)
        
        unfilled = []
        unfilled.extend(sorted_sells)
        unfilled.extend(sorted_buys)
        if buy_order is not None and buy_order.quantity > 0: # must check if it is GTC or IOC
            if buy_order.tif is TimeInForce.GTC:
                unfilled.append(buy_order)
        if sell_order is not None and sell_order.quantity > 0: # must check if it is GTC or IOC
            if sell_order.tif is TimeInForce.GTC:
                unfilled.append(sell_order)
                
        # return all the trades and the unfilled trades
        return trades, unfilled
        





