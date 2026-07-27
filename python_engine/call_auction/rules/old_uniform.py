from ..pricing_rule import PricingRule
from python_engine.orderbook.order import Order, Trade, TimeInForce, Side
from uuid import UUID
import random
import copy

random.seed(17)

class Uniform(PricingRule):
    def _populate_levels(self, buys: list[Order], levels: dict[int, list[Order]]):
        for buy_order in buys:
            if buy_order.price in levels:
                levels[buy_order.price].append(buy_order)
            else:
                levels[buy_order.price] = []
                levels[buy_order.price].append(buy_order)

    def create_order_list(self, price_level: int, orders: dict[int, list[Order]], side: Side) -> list[Order]:
        r: list[Order] = []
        if side is Side.BUY:
            for p in sorted(orders.keys(), reverse=True):
                if p >= price_level:
                    r.extend(copy.copy(o) for o in orders[p])
        else: # side SELL
            for p in sorted(orders.keys()):
                if p <= price_level:
                    r.extend(copy.copy(o) for o in orders[p])
        return  sorted(r, key=lambda o: o.timestamp)

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
    
    @staticmethod
    def _eligible(o:Order, best_p: int):
        return o.price >= best_p if o.side is Side.BUY else o.price <= best_p


    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]:
        # create the price levels:
        buy_levels: dict[int, list[Order]] = {}
        sell_levels: dict[int, list[Order]] = {}

        level_volume: dict[int, int] = {}
        self._populate_levels(buys, buy_levels)
        self._populate_levels(sells, sell_levels)

        level_trades: dict[str, tuple[list[Order], list[Order], int]] = {}

        price_levels = set(sell_levels.keys() | buy_levels.keys())

        for p in sorted(price_levels): # ascending order
            # all sell order from this price downrds are considered
               # all buy order form thi price upwards are considered   
            sorted_sells = self.create_order_list(price_level=p, orders=sell_levels, side=Side.SELL)
            sorted_buys = self.create_order_list(price_level=p, orders=buy_levels, side=Side.BUY)
                # TODO: surplus fomrula does not count actually tradeable volume, so you could fix it
            if len(sorted_sells) >0  and len(sorted_buys)>0:
                # unpacking all sell order that valid for level p
                surplus = abs(sum(o.quantity for o in sorted_sells) - sum(o.quantity for o in sorted_buys))

                trades = []
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
                        resting_price = p
            
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
                    level_volume[p] = level_volume.get(p, 0) + resting_quantity

                    trades.append(trade)
                
                unfilled = []
                unfilled.extend(o for o in sorted_sells if o.tif is TimeInForce.GTC)
                unfilled.extend(o for o in sorted_buys if o.tif is TimeInForce.GTC)
                if buy_order is not None and buy_order.quantity > 0: # must check if it is GTC or IOC
                    if buy_order.tif is TimeInForce.GTC:
                        unfilled.append(buy_order)
                if sell_order is not None and sell_order.quantity > 0: # must check if it is GTC or IOC
                    if sell_order.tif is TimeInForce.GTC:
                        unfilled.append(sell_order)
                        
                # return all the trades and the unfilled trades

                level_trades[p] = (trades, unfilled, surplus)
            else:
                continue # move onto the next level
        
        if not level_volume:
            return [], [o for o in buys + sells if o.tif is TimeInForce.GTC]

        # pick the one at max, if there are more find the surplus maximising 
        max_val = max(level_volume.values())
        max_keys = [k for k, v in level_volume.items() if v == max_val]
        price_level = 0

        if len(max_keys) > 1:
            min_surplus = min(level_trades[k][2] for k in max_keys)
            trade_keys = [k for k in max_keys if level_trades[k][2] == min_surplus]
            index = random.randint(0, len(trade_keys)-1)
            # if surplus is the same just put the fires in the bag (use random even If I could use momentum from previous trades)
            price_level = trade_keys[index]
            trades =  level_trades[price_level][0]
            unfilled = level_trades[price_level][1]
        else:
            price_level = max_keys[0]
            trades =  level_trades[price_level][0]
            unfilled = level_trades[price_level][1]

        ineligible_gtc = [o for o in buys + sells if not Uniform._eligible(o, price_level) and o.tif is TimeInForce.GTC]
         
        return trades, unfilled + ineligible_gtc


