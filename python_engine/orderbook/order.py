from enum import Enum
import itertools
from dataclasses import field
from datetime import datetime
from dataclasses import dataclass


# Counters, as in cpp_engine/orderbook/Order.cpp. Ids only need to be unique
# within the process, and an int hashes and compares far cheaper than a UUID.
_order_ids = itertools.count(1)
_symbol_ids = itertools.count(1)
# Arrival counter. Timestamps can tie (coarse clocks, or callers passing the
# same instant), so priority is (timestamp, seq): seq breaks ties by the order
# in which Order objects were created.
_arrival = itertools.count(1)


@dataclass(slots=True)
class Symbol:
    instrument: str # the name of the symbol eg 'APPL'
    id: int = field(default_factory=lambda: next(_symbol_ids))

class Side(Enum):
    BUY = "buy"
    SELL = "sell"


class TimeInForce(Enum):
    GTC = "gtc"   # good-till-cancel: unfilled remainder rests in the book until cancelled or expires
    IOC = "ioc"   # immediate-or-cancel: fills as much as possible immediately, rest gets cancelled
    FOK = "fok"   # fill-or-kill: fills all of its volume immediately or cancelled


def _is_int(x) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


# slots=True: a fixed set of fields with no per-instance __dict__, the Python
# counterpart of the plain C++ struct.
@dataclass(slots=True)
class Order:
    side: Side
    price: int | None # must be an integer of the ticker
    quantity: int
    trader_id: int
    tif: TimeInForce = TimeInForce.GTC # good till cancel is the defualt for now
    order_id: int = field(default_factory=lambda: next(_order_ids))
    timestamp: datetime = field(default_factory=datetime.now)
    seq: int = field(default_factory=lambda: next(_arrival), repr=False, compare=False)

    def __post_init__(self) -> None:
        # Only checked on construction: the engines legitimately drive quantity
        # down to 0 as an order fills. The type checks stand in for what the
        # C++ compiler enforces statically.
        if not _is_int(self.quantity):
            raise TypeError(f"quantity must be an int, got {type(self.quantity).__name__}")
        if self.quantity <= 0:
            raise ValueError(f"quantity must be positive, got {self.quantity}")
        if self.price is not None and not _is_int(self.price):
            raise TypeError(f"price must be an int number of ticks or None, got {type(self.price).__name__}")

    def copy(self) -> "Order":
        """Field-by-field copy, like the C++ `Order(const Order &) = default`.

        Keeps order_id and seq (Uniform keys its write-back by id and sorts
        copies by priority); only quantity is meant to diverge afterwards.
        """
        clone = object.__new__(Order)
        clone.side = self.side
        clone.price = self.price
        clone.quantity = self.quantity
        clone.trader_id = self.trader_id
        clone.tif = self.tif
        clone.order_id = self.order_id
        clone.timestamp = self.timestamp
        clone.seq = self.seq
        return clone

    __copy__ = copy

    def renew(self, timestamp: datetime | None = None) -> None:
        """Move to the back of time priority: a new timestamp (now, by default)
        and a new arrival number. Used when an amendment loses its place."""
        self.timestamp = timestamp if timestamp is not None else datetime.now()
        self.seq = next(_arrival)

    @property
    def is_market(self) -> bool:
        return self.price is None

    @property
    def priority(self) -> tuple[datetime, int]:
        """Time priority: older first, ties broken by arrival."""
        return (self.timestamp, self.seq)



@dataclass(slots=True)
class Trade: # resolved orders
    price: int
    quantity: int
    buy_order_id: int
    sell_order_id: int
    timestamp: datetime = field(default_factory=datetime.now)
