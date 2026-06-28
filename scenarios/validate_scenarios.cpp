#include <iostream>
#include <string>
#include "lob/order_book.hpp"
using namespace lob;

bool pass(const std::string& name, bool cond) {
    std::cout << (cond ? "[PASS] " : "[FAIL] ") << name << "\n";
    return cond;
}

int main() {
    std::cout << "=== LOB Engine Scenario Validation ===\n\n";
    int total = 0, passed = 0;

    // NO_SIGNAL — limit order placed, no match, rests in book
    {
        ++total;
        OrderBook b;
        auto r = b.submit({1, Side::Buy, OrderType::Limit, 99, 10});
        Price bid;
        bool ok = r.trades.empty() && r.risk == RiskResult::OK && b.best_bid(bid) && bid == 99;
        if (pass("NO_SIGNAL: limit order rests in book", ok)) ++passed;
    }

    // BUY_FILL — aggressive buy crosses ask
    {
        ++total;
        OrderBook b;
        b.submit({1, Side::Sell, OrderType::Limit, 100, 10});
        auto r = b.submit({2, Side::Buy, OrderType::Limit, 100, 10});
        bool ok = r.trades.size() == 1 && r.trades[0].maker_id == 1;
        if (pass("BUY_FILL: buy order crosses ask", ok)) ++passed;
    }

    // SELL_FILL — aggressive sell crosses bid
    {
        ++total;
        OrderBook b;
        b.submit({1, Side::Buy, OrderType::Limit, 100, 10});
        auto r = b.submit({2, Side::Sell, OrderType::Limit, 100, 10});
        bool ok = r.trades.size() == 1 && r.trades[0].maker_id == 1;
        if (pass("SELL_FILL: sell order crosses bid", ok)) ++passed;
    }

    // RISK_REJECT — order exceeds size limit
    {
        ++total;
        OrderBook b;
        auto r = b.submit({1, Side::Buy, OrderType::Limit, 100, 100'000});
        bool ok = r.risk == RiskResult::REJECT_SIZE && r.trades.empty();
        if (pass("RISK_REJECT: oversized order rejected", ok)) ++passed;
    }

    // INVALID_ORDER — zero quantity
    {
        ++total;
        OrderBook b;
        auto r = b.submit({1, Side::Buy, OrderType::Limit, 100, 0});
        bool ok = r.risk == RiskResult::REJECT_SIZE && r.trades.empty();
        if (pass("INVALID_ORDER: zero quantity rejected", ok)) ++passed;
    }

    std::cout << "\nResult: " << passed << "/" << total << " scenarios passed\n";
    return (passed == total) ? 0 : 1;
}
