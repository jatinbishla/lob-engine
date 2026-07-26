#pragma once
#include "lob/types.hpp"
#include "lob/risk.hpp"
#include "lob/pool.hpp"
#include <map>
#include <list>
#include <unordered_map>
#include <functional>

namespace lob {

// Order queue at a price level. Backed by the arena pool so resting an order
// costs no system allocation on the hot path. (M6 optimization — Opt 1.)
using OrderList = std::list<Order, PoolAllocator<Order>>;

struct Level {
    OrderList orders;    // front = oldest = highest priority
    Quantity  total = 0; // cached depth — avoid re-summing
};

class OrderBook {
public:
    struct SubmitResult {
        std::vector<Trade> trades;
        RiskResult         risk = RiskResult::OK;
    };

    SubmitResult submit(Order order);
    bool cancel(OrderId id);

    // ---- Feed-replay API (M8) ----
    // An ITCH feed is already the *result* of matching upstream, so replayed
    // orders never cross — they rest, and are later reduced or replaced by
    // reference number. These bypass both the matcher and the risk guard.
    void rest(Order order);                  // place a resting order directly
    bool reduce(OrderId id, Quantity qty);   // executed/cancelled shares; erase at 0
    bool replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty);

    bool best_bid(Price& out) const;
    bool best_ask(Price& out) const;
    Quantity depth_at(Side side, Price price) const;

private:
    // Bids: highest price first (std::greater); Asks: lowest price first (default)
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level>                      asks_;

    struct Location {
        Side                 side;
        Price                price;
        OrderList::iterator  it;
    };
    std::unordered_map<OrderId, Location> index_;

    RiskGuard risk_;
    Sequence  seq_ = 0;

    // Template: works for both bid-side and ask-side maps
    // (different comparators = different types)
    template <typename BookSide>
    void match_side(Order& incoming, BookSide& opposite, std::vector<Trade>& trades);
};

} // namespace lob
