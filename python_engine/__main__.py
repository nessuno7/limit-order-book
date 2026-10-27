"""Usage sketch: same interface, three markets. Run with `python -m python_engine`."""
from python_engine.orderbook.order import Order, Side
from python_engine.continuous_auction.continuous_auction import ContinuousDoubleAuction
from python_engine.call_auction.call_auction import CallAuction
from python_engine.call_auction.rules.uniform import Uniform
from python_engine.call_auction.rules.discriminatory import Discriminatory

def flow():
    trader = 1
    return [
        Order(Side.SELL, 101, 5, trader),
        Order(Side.SELL, 103, 5, trader),
        Order(Side.BUY, 99, 4, trader),
        Order(Side.BUY, 104, 8, trader),
    ]

markets = [
    ("continuous", ContinuousDoubleAuction()),
    ("uniform call", CallAuction(Uniform())),
    ("discriminatory call", CallAuction(Discriminatory())),
]
for name, engine in markets:
    for order in flow():
        engine.submit(order)
    if isinstance(engine, CallAuction):
        engine.run()
    fills = ", ".join(f"{t.quantity}@{t.price}" for t in engine.trades) or "none"
    print(f"{name:>20}: {fills}")
