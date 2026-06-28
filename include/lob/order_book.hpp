#pragma once
#include "lob/types.hpp"
#include <map>
#include <list>
#include <unordered_map>
#include <functional>

namespace lob {

struct Level {
    std::list<Order> orders;  // front = oldest = highest priority
    Quantity total = 0;       // cached depth — avoid re-summing
};

class OrderBook {
public:
    struct SubmitResult {
        std::vector<Trade> trades;
        // RiskResult added in M4
    };

    SubmitResult submit(Order order);
    bool cancel(OrderId id);

    bool best_bid(Price& out) const;
    bool best_ask(Price& out) const;
    Quantity depth_at(Side side, Price price) const;

private:
    // Bids: highest price first (std::greater); Asks: lowest price first (default)
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level>                      asks_;

    struct Location {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<OrderId, Location> index_;

    Sequence seq_ = 0;

    // Template: works for both bid-side and ask-side maps
    // (different comparators = different types)
    template <typename BookSide>
    void match_side(Order& incoming, BookSide& opposite, std::vector<Trade>& trades);
};

} // namespace lob
