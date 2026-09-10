//
// Shared type aliases for the engine. Include this instead of spelling out
// <cstdint> / <chrono> types at every use site: changing a width or a clock
// here changes it everywhere.
//

#ifndef LIMIT_ORDER_BOOK_TYPES_H
#define LIMIT_ORDER_BOOK_TYPES_H

#include <chrono>
#include <cstdint>

// Wall-clock instant. system_clock is the only standard clock that maps to
// calendar time, which is what an exchange timestamp has to be -- steady_clock
// has no epoch you can print.
using Datetime = std::chrono::system_clock::time_point;

using Price    = std::int64_t;   // integer ticks, never a float
using Quantity = std::uint64_t;
using OrderId  = std::uint64_t;
using TraderId = std::uint64_t;

#endif //LIMIT_ORDER_BOOK_TYPES_H
