"""
Fill-or-kill feasibility checks shared by the call-auction pricing rules.

In a batch, a FOK's opposing volume may itself come from another FOK that can
only fill if *this* order fills it. `fok_walk` resolves that by recursing into
the opposing FOK, and `fok_helper` drops FOKs from the front of a queue until
one is found that can fill.
"""
from ..orderbook.order import Order, Side, TimeInForce


def _crosses(o1: Order, o2: Order) -> bool:
    if o1.price is None or o2.price is None: # a market order on either side always crosses
        return True
    return (o1.price >= o2.price) if o1.side is Side.BUY else (o2.price >= o1.price)


def fok_walk(order: Order, sorted_list: list[Order], opp_sorted_list: list[Order],
             orders_to_ignore: set[int] | None = None,
             partially_filled_orders: dict[int, int] | None = None,
             starting_vol: int = 0) -> bool:
    """True if `order` can fill completely from `opp_sorted_list`."""
    if orders_to_ignore is None:
        orders_to_ignore = set()
    if partially_filled_orders is None:
        partially_filled_orders = {}
    volume = starting_vol

    for opp_order in opp_sorted_list:
        if opp_order.order_id in orders_to_ignore:
            continue
        if opp_order.tif is TimeInForce.FOK and opp_order.quantity > order.quantity - volume:
            # The opposing FOK is bigger than what we still need, so it can only
            # fill by also taking volume from our side. Try that speculatively
            # on copies: a failed attempt must not leave orders marked as
            # consumed for the next opposing FOK we try.
            trial_ignore = set(orders_to_ignore)
            trial_ignore.add(order.order_id)
            if fok_walk(
                order=opp_order,
                sorted_list=opp_sorted_list,
                opp_sorted_list=sorted_list,
                orders_to_ignore=trial_ignore,
                partially_filled_orders=dict(partially_filled_orders),
                starting_vol=order.quantity - volume,
            ):
                return True
            orders_to_ignore.add(opp_order.order_id) # cannot be filled, skip it from now on
            continue

        if not _crosses(order, opp_order): # beyond the limit, and the list is in priority order
            return False
        available = partially_filled_orders.get(opp_order.order_id, opp_order.quantity)
        needed = order.quantity - volume
        if needed < available:
            partially_filled_orders[opp_order.order_id] = available - needed # leftover volume
        else:
            partially_filled_orders.pop(opp_order.order_id, None)
            orders_to_ignore.add(opp_order.order_id)
        volume += available
        if volume >= order.quantity:
            return True
    return False


def fok_helper(order: Order | None, sorted_list: list[Order], opp_sorted_list: list[Order]) -> Order | None:
    """Drop FOKs that cannot fill from the front of the queue.

    Returns the first order that can trade (any non-FOK, or a FOK that passes
    the walk), or None once `sorted_list` is exhausted.
    """
    while order is not None and order.tif is TimeInForce.FOK:
        if fok_walk(order, sorted_list=sorted_list, opp_sorted_list=opp_sorted_list):
            return order
        order = sorted_list.pop(0) if sorted_list else None
    return order
