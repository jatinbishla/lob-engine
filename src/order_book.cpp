#include "lob/order_book.hpp"
#include <algorithm>
#include <cstdlib>

namespace lob {

// --- RiskGuard ---

RiskResult RiskGuard::check(const Order& o) const {
    if (o.quantity == 0 || o.quantity > limits_.max_order_size)
        return RiskResult::REJECT_SIZE;
    int64_t notional = static_cast<int64_t>(o.price) * static_cast<int64_t>(o.quantity);
    if (notional > limits_.max_notional)
        return RiskResult::REJECT_NOTIONAL;
    int64_t projected = net_position_ +
        (o.side == Side::Buy ? static_cast<int64_t>(o.quantity)
                             : -static_cast<int64_t>(o.quantity));
    if (std::abs(projected) > static_cast<int64_t>(limits_.max_net_position))
        return RiskResult::REJECT_POSITION;
    return RiskResult::OK;
}

void RiskGuard::apply_fill(Side side, Quantity qty) {
    if (side == Side::Buy)  net_position_ += static_cast<int64_t>(qty);
    else                    net_position_ -= static_cast<int64_t>(qty);
}

// --- Matching ---

template <typename BookSide>
void OrderBook::match_side(Order& incoming, BookSide& opposite,
                           std::vector<Trade>& trades) {
    while (incoming.quantity > 0 && !opposite.empty()) {
        auto  level_it    = opposite.begin();  // best price level
        Price level_price = level_it->first;

        bool crosses =
            (incoming.type == OrderType::Market) ||
            (incoming.side == Side::Buy  && incoming.price >= level_price) ||
            (incoming.side == Side::Sell && incoming.price <= level_price);
        if (!crosses) break;

        Level& level = level_it->second;
        auto&  queue = level.orders;

        while (incoming.quantity > 0 && !queue.empty()) {
            Order& resting = queue.front();
            Quantity fill  = std::min(incoming.quantity, resting.quantity);

            trades.push_back(Trade{
                incoming.id, resting.id, level_price, fill, ++seq_
            });

            incoming.quantity -= fill;
            resting.quantity  -= fill;
            level.total       -= fill;

            if (resting.quantity == 0) {
                index_.erase(resting.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) opposite.erase(level_it);
    }
}

// --- Public API ---

OrderBook::SubmitResult OrderBook::submit(Order order) {
    SubmitResult result;

    // Risk check before matching
    result.risk = risk_.check(order);
    if (result.risk != RiskResult::OK)
        return result;  // rejected — no trades, no resting

    // Match against opposite side
    if (order.side == Side::Buy)  match_side(order, asks_, result.trades);
    else                          match_side(order, bids_, result.trades);

    // Update risk position for each fill
    for (const auto& t : result.trades)
        risk_.apply_fill(order.side, t.quantity);

    // Rest unfilled limit quantity
    if (order.quantity > 0 && order.type == OrderType::Limit) {
        auto& level = (order.side == Side::Buy)
                      ? bids_[order.price]
                      : asks_[order.price];
        level.orders.push_back(order);
        level.total += order.quantity;
        auto it = std::prev(level.orders.end());
        index_[order.id] = Location{ order.side, order.price, it };
    }

    return result;
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;

    Location& loc = found->second;
    if (loc.side == Side::Buy) {
        auto& level = bids_[loc.price];
        level.total -= loc.it->quantity;
        level.orders.erase(loc.it);
        if (level.orders.empty()) bids_.erase(loc.price);
    } else {
        auto& level = asks_[loc.price];
        level.total -= loc.it->quantity;
        level.orders.erase(loc.it);
        if (level.orders.empty()) asks_.erase(loc.price);
    }
    index_.erase(found);
    return true;
}

bool OrderBook::best_bid(Price& out) const {
    if (bids_.empty()) return false;
    out = bids_.begin()->first;
    return true;
}

bool OrderBook::best_ask(Price& out) const {
    if (asks_.empty()) return false;
    out = asks_.begin()->first;
    return true;
}

Quantity OrderBook::depth_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return (it != bids_.end()) ? it->second.total : 0;
    } else {
        auto it = asks_.find(price);
        return (it != asks_.end()) ? it->second.total : 0;
    }
}

// Explicit template instantiations (must be in .cpp — not header)
template void OrderBook::match_side<std::map<Price, Level, std::greater<Price>>>(
    Order&, std::map<Price, Level, std::greater<Price>>&, std::vector<Trade>&);
template void OrderBook::match_side<std::map<Price, Level>>(
    Order&, std::map<Price, Level>&, std::vector<Trade>&);

} // namespace lob
