//
// Port of the Trade dataclass in python_engine/orderbook/order.py
//

#include "Trade.h"

Trade::Trade(const Price price_,
             const Quantity quantity_,
             const OrderId buyOrderId_,
             const OrderId sellOrderId_,
             std::optional<Datetime> timestamp_)
    : price(price_),
      quantity(quantity_),
      buyOrderId(buyOrderId_),
      sellOrderId(sellOrderId_),
      timestamp(timestamp_.value_or(std::chrono::system_clock::now())) {}
