#pragma once
#include "lob/types.hpp"

namespace lob {

struct RiskLimits {
    Quantity max_order_size   = 10'000;
    Quantity max_net_position = 50'000;
    int64_t  max_notional     = 5'000'000; // price * quantity, in ticks
};

enum class RiskResult : uint8_t {
    OK,
    REJECT_SIZE,
    REJECT_POSITION,
    REJECT_NOTIONAL
};

class RiskGuard {
public:
    explicit RiskGuard(RiskLimits limits = {}) : limits_(limits) {}

    RiskResult check(const Order& o) const;
    void apply_fill(Side side, Quantity qty);  // update net position after fill
    int64_t net_position() const { return net_position_; }

private:
    RiskLimits limits_;
    int64_t    net_position_ = 0; // +ve = long, -ve = short
};

} // namespace lob
