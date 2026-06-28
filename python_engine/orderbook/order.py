from enum import Enum
import itertools
import time
from uuid import UUID, uuid4
from dataclasses import field
from datetime import datetime
from dataclasses import dataclass



@dataclass
class Symbol:
    instrument: str # the name of the symbol eg 'APPL'
    id: UUID = field(default_factory=uuid4)

class Side(Enum):
    BUY = "buy"
    SELL = "sell"


class TimeInForce(Enum):
    GTC = "gtc"   # good-till-cancel: unfilled remainder rests in the book until cancelled or expires
    IOC = "ioc"   # immediate-or-cancel: fills as much as possible immediately, rest gets cancelled
    FOK = "foc"   # fill-or-kill: fills all of its volume immediately or cancelled 


@dataclass
class Order:
    side: Side
    price: int | None # must be an integer of the ticker
    quantity: int
    trader_id: UUID
    tif: TimeInForce = TimeInForce.GTC # good till cancel is the defualt for now
    order_id: UUID = field(default_factory=uuid4)
    timestamp: datetime = field(default_factory=datetime.now)

    @property
    def is_market(self) -> bool:
        return self.price is None



@dataclass
class Trade: # resolved orders
    price: int
    quantity: int
    buy_order_id: UUID
    sell_order_id: UUID
    timestamp: datetime = field(default_factory=datetime.now)