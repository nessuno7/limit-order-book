from ..pricing_rule import PricingRule
from python_engine.orderbook.order import Order, Trade, TimeInForce, Side
from uuid import UUID
import copy
import random

random.seed(17)


class Uniform(PricingRule):
    def _populate_levels(self, orders: list[Order], levels: dict[int, list[Order]]):
        for order in orders:
            if order.price is None:
                continue
            if order.price in levels:
                levels[order.price].append(order)
            else:
                levels[order.price] = [order]

    def create_order_list(self, price_level: int, orders: dict[int, list[Order]], side: Side, market_orders: list[Order] | None = None) -> list[Order]:
        r: list[Order] = []
        if market_orders:
            r.extend(copy.copy(o) for o in market_orders)
        if side is Side.BUY:
            for p in sorted(orders.keys(), reverse=True):
                if p >= price_level:
                    r.extend(copy.copy(o) for o in orders[p])
        else:
            for p in sorted(orders.keys()):
                if p <= price_level:
                    r.extend(copy.copy(o) for o in orders[p])
        return sorted(r, key=lambda o: o.timestamp)

    def _fok_walk(self, order: Order, sorted_list: list[Order], opp_sorted_list: list[Order], orders_to_ignore: dict[UUID, Order] | None = None, partially_filled_orders: dict[UUID, int] | None = None, starting_vol: int = 0):
        if orders_to_ignore is None:
            orders_to_ignore = {}
        if partially_filled_orders is None:
            partially_filled_orders = {}
        volume = starting_vol
        compare = lambda o1, o2: True if (o1.price is None or o2.price is None) else (
            (o1.price >= o2.price) if o1.side is Side.BUY else (o2.price >= o1.price)
        )

        for opp_order in opp_sorted_list:
            if opp_order.order_id not in orders_to_ignore:
                if opp_order.tif is TimeInForce.FOK:
                    if opp_order.quantity > order.quantity - volume:
                        orders_to_ignore[order.order_id] = order
                        if self._fok_walk(
                            order=opp_order,
                            sorted_list=opp_sorted_list,
                            opp_sorted_list=sorted_list,
                            orders_to_ignore=orders_to_ignore,
                            partially_filled_orders=partially_filled_orders,
                            starting_vol=order.quantity - volume
                        ):
                            return True
                        else:
                            orders_to_ignore[opp_order.order_id] = opp_order
                            continue

                if compare(order, opp_order):
                    if opp_order.order_id in partially_filled_orders:
                        opp_order_quantity = partially_filled_orders[opp_order.order_id]
                        if order.quantity - volume < opp_order_quantity:
                            partially_filled_orders[opp_order.order_id] = opp_order_quantity - (order.quantity - volume)
                        else:
                            del partially_filled_orders[opp_order.order_id]
                            orders_to_ignore[opp_order.order_id] = opp_order
                        volume += opp_order_quantity
                    else:
                        if order.quantity - volume < opp_order.quantity:
                            partially_filled_orders[opp_order.order_id] = opp_order.quantity - (order.quantity - volume)
                        else:
                            orders_to_ignore[opp_order.order_id] = opp_order
                        volume += opp_order.quantity
                    if volume >= order.quantity:
                        return True
                else:
                    return False
        return False

    def _fok_helper(self, order: Order, sorted_list: list[Order], opp_sorted_list: list[Order]) -> Order:
        while order.tif is TimeInForce.FOK:
            if not self._fok_walk(order, sorted_list=sorted_list, opp_sorted_list=opp_sorted_list):
                order = sorted_list.pop(0) if sorted_list else None
            else:
                return order
            if order is None:
                return order
        return order

    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]:
        buy_levels: dict[int, list[Order]] = {}
        sell_levels: dict[int, list[Order]] = {}
        level_volume: dict[int, int] = {}
        level_trades: dict[int, tuple[list[Trade], list[Order]]] = {}
        level_surplus: dict[int, int] = {}

        mkt_buys = [o for o in buys if o.price is None]
        mkt_sells = [o for o in sells if o.price is None]
        self._populate_levels([o for o in buys if o.price is not None], buy_levels)
        self._populate_levels([o for o in sells if o.price is not None], sell_levels)

        candidate_prices = sorted(set(buy_levels.keys()) | set(sell_levels.keys()))

        for p in candidate_prices:
            sorted_sells = self.create_order_list(price_level=p, orders=sell_levels, side=Side.SELL, market_orders=mkt_sells)
            sorted_buys = self.create_order_list(price_level=p, orders=buy_levels, side=Side.BUY, market_orders=mkt_buys)

            if not sorted_buys or not sorted_sells:
                continue

            surplus = abs(sum(o.quantity for o in sorted_buys) - sum(o.quantity for o in sorted_sells))

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

                if buy_order.price is not None and sell_order.price is not None and buy_order.price < sell_order.price:
                    break

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

                trade = Trade(price=resting_price, quantity=resting_quantity, buy_order_id=b_id, sell_order_id=s_id)
                level_volume[p] = level_volume.get(p, 0) + resting_quantity
                trades.append(trade)

            unfilled = []
            unfilled.extend(o for o in sorted_sells if o.tif is TimeInForce.GTC)
            unfilled.extend(o for o in sorted_buys if o.tif is TimeInForce.GTC)
            if buy_order is not None and buy_order.quantity > 0:
                if buy_order.tif is TimeInForce.GTC:
                    unfilled.append(buy_order)
            if sell_order is not None and sell_order.quantity > 0:
                if sell_order.tif is TimeInForce.GTC:
                    unfilled.append(sell_order)

            level_trades[p] = (trades, unfilled)
            level_surplus[p] = surplus

        if not level_volume:
            return [], [o for o in buys + sells if o.tif is TimeInForce.GTC]

        max_val = max(level_volume.values())
        max_keys = [k for k, v in level_volume.items() if v == max_val]

        if len(max_keys) > 1:
            min_surplus = min(level_surplus[k] for k in max_keys)
            trade_keys = [k for k in max_keys if level_surplus[k] == min_surplus]
            index = random.randint(0, len(trade_keys) - 1)
            best_p = trade_keys[index]
        else:
            best_p = max_keys[0]

        trades, sim_unfilled = level_trades[best_p]

        # Orders that weren't eligible at best_p never entered the simulation;
        # return any GTC ones so they roll into the next batch.
        def _eligible(o: Order) -> bool:
            if o.price is None:
                return True
            return o.price >= best_p if o.side is Side.BUY else o.price <= best_p

        ineligible_gtc = [o for o in buys + sells if not _eligible(o) and o.tif is TimeInForce.GTC]
        return trades, sim_unfilled + ineligible_gtc
