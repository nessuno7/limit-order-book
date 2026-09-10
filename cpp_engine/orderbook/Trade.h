//
// Created by Nicola on 09/09/2026.
//

#ifndef LIMIT_ORDER_BOOK_TRADE_H
#define LIMIT_ORDER_BOOK_TRADE_H

#include <chrono>

using Datetime = std::chrono::time_point<std::chrono::system_clock>;

class Trade {
    uint64_t price;
    uint64_t quantity;
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    Datetime timestamp;

public:
    Trade(uint64_t p, q, bId, sID): price(p), quantity(q), buyOrderId(sID), sellOrderId(bId) {
        timestamp = std::chrono::system_clock::now();
    }

    uint64_t getPrice(){return price;};
    uint64_t getQuantity(){return quantity;};
    uint64_t getBuyOrderId(){return buyOrderId;};
    uint64_t getSellOrderId(){return sellOrderId;};
    uint64_t getTimestamp(){return timestamp;};


};


#endif //LIMIT_ORDER_BOOK_TRADE_H
