//
// Port of the Trade dataclass in python_engine/orderbook/order.py
//

#ifndef LIMIT_ORDER_BOOK_TRADE_H
#define LIMIT_ORDER_BOOK_TRADE_H

#include "Types.h"

#include <optional>

struct Trade {
    Price price;
    Quantity quantity;
    OrderId buyOrderId;
    OrderId sellOrderId;
    Datetime timestamp;

    Trade(Price price_,
          Quantity quantity_,
          OrderId buyOrderId_,
          OrderId sellOrderId_,
          std::optional<Datetime> timestamp_ = std::nullopt);

    [[nodiscard]] Price getPrice() const { return price; }
    [[nodiscard]] Quantity getQuantity() const { return quantity; }
    [[nodiscard]] OrderId getBuyOrderId() const { return buyOrderId; }
    [[nodiscard]] OrderId getSellOrderId() const { return sellOrderId; }
    [[nodiscard]] Datetime getTimestamp() const { return timestamp; }
};

#endif //LIMIT_ORDER_BOOK_TRADE_H
