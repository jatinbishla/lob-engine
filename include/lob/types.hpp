#pragma once
#include <cstdint>
#include <vector>

namespace lob {

using OrderId   = std::uint64_t;
using Price     = std::int64_t;   // integer ticks — NEVER double
using Quantity  = std::uint64_t;
using Sequence  = std::uint64_t;

enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;     // ignored for Market orders
    Quantity  quantity;  // remaining unfilled quantity
};

struct Trade {
    OrderId  taker_id;
    OrderId  maker_id;
    Price    price;
    Quantity quantity;
    Sequence seq;
};

} // namespace lob
