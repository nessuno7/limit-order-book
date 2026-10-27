from sortedcontainers import SortedDict

from ..pricing_rule import PricingRule
from ..fok import fok_helper
from python_engine.orderbook.order import Order, Trade, TimeInForce, Side


class Uniform(PricingRule):
    @staticmethod
    def _populate_levels(orders: list[Order], levels: SortedDict) -> None:
        for order in orders:
            if order.price is None:
                continue
            level = levels.get(order.price)
            if level is not None:
                level.append(order)
            else:
                levels[order.price] = [order]

    @staticmethod
    def create_order_list(price_level: int, orders: SortedDict, side: Side, market_orders: list[Order]) -> list[Order]:
        # Copies: each candidate price runs its own simulation, which mutates
        # quantities. Only the winning simulation is written back to the
        # originals.
        r: list[Order] = [o.copy() for o in market_orders]
        if side is Side.BUY:
            for p, level in reversed(orders.items()): # descending
                if p >= price_level:
                    r.extend(o.copy() for o in level)
        else:
            for p, level in orders.items(): # ascending
                if p <= price_level:
                    r.extend(o.copy() for o in level)
        r.sort(key=lambda o: o.priority) # stable, like std::stable_sort
        return r

    @staticmethod
    def choose_tie_break(tied_prices: list[int]) -> int:
        """Among prices tied on volume and surplus, take the highest."""
        return max(tied_prices)

    def clear(self, buys: list[Order], sells: list[Order]) -> tuple[list[Trade], list[Order]]:
        buy_levels: SortedDict[int, list[Order]] = SortedDict()
        sell_levels: SortedDict[int, list[Order]] = SortedDict()
        level_volume: dict[int, int] = {}
        level_results: dict[int, tuple[list[Trade], list[Order], list[Order], int]] = {} # trades, unfilled, simulated, surplus

        mkt_buys = [o for o in buys if o.price is None]
        mkt_sells = [o for o in sells if o.price is None]
        self._populate_levels(buys, buy_levels)
        self._populate_levels(sells, sell_levels)

        # Market orders add no candidate price of their own: a batch of only
        # market orders has no price discovery and does not trade.
        candidate_prices = sorted(set(buy_levels) | set(sell_levels))

        for p in candidate_prices: # ascending
            sorted_sells = self.create_order_list(p, sell_levels, Side.SELL, mkt_sells)
            sorted_buys = self.create_order_list(p, buy_levels, Side.BUY, mkt_buys)

            if not sorted_buys or not sorted_sells:
                continue

            simulated = sorted_buys + sorted_sells # every copy in this simulation, for write-back
            surplus = abs(sum(o.quantity for o in sorted_buys) - sum(o.quantity for o in sorted_sells))

            trades: list[Trade] = []
            buy_order = sorted_buys.pop(0)
            sell_order = sorted_sells.pop(0)
            while buy_order is not None and sell_order is not None:
                buy_order = fok_helper(buy_order, sorted_buys, [sell_order] + sorted_sells)
                if buy_order is None:
                    break
                sell_order = fok_helper(sell_order, sorted_sells, [buy_order] + sorted_buys)
                if sell_order is None:
                    break

                # Every order here is eligible at p (buy >= p >= sell), so all
                # pairs cross and every trade prints at p.
                b_id = buy_order.order_id
                s_id = sell_order.order_id

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

                # Only written when a trade actually prints, so a price that
                # crosses nothing never becomes a candidate for p*.
                level_volume[p] = level_volume.get(p, 0) + resting_quantity
                trades.append(Trade(p, resting_quantity, b_id, s_id))

            unfilled: list[Order] = []
            unfilled.extend(o for o in sorted_sells if o.tif is TimeInForce.GTC)
            unfilled.extend(o for o in sorted_buys if o.tif is TimeInForce.GTC)
            if buy_order is not None and buy_order.quantity > 0 and buy_order.tif is TimeInForce.GTC:
                unfilled.append(buy_order)
            if sell_order is not None and sell_order.quantity > 0 and sell_order.tif is TimeInForce.GTC:
                unfilled.append(sell_order)

            level_results[p] = (trades, unfilled, simulated, surplus)

        if not level_volume:
            rolled = [o for o in buys if o.tif is TimeInForce.GTC]
            rolled.extend(o for o in sells if o.tif is TimeInForce.GTC)
            return [], rolled

        max_val = max(level_volume.values())
        max_keys = [k for k, v in level_volume.items() if v == max_val] # ascending

        if len(max_keys) > 1:
            min_surplus = min(level_results[k][3] for k in max_keys)
            best_p = self.choose_tie_break([k for k in max_keys if level_results[k][3] == min_surplus])
        else:
            best_p = max_keys[0]

        trades, sim_unfilled, simulated, _ = level_results[best_p]

        # Write the winning simulation back to the caller's orders, so what
        # rolls into the next batch is the original object, not a copy.
        originals: dict[int, Order] = {}
        for o in buys:
            originals[o.order_id] = o
        for o in sells:
            originals[o.order_id] = o
        for clone in simulated:
            originals[clone.order_id].quantity = clone.quantity

        unfilled = [originals[o.order_id] for o in sim_unfilled]

        # Orders that weren't eligible at best_p never entered the simulation;
        # return any GTC ones so they roll into the next batch.
        def eligible(o: Order) -> bool:
            if o.price is None:
                return True
            return o.price >= best_p if o.side is Side.BUY else o.price <= best_p

        unfilled.extend(o for o in buys if not eligible(o) and o.tif is TimeInForce.GTC)
        unfilled.extend(o for o in sells if not eligible(o) and o.tif is TimeInForce.GTC)
        return trades, unfilled
