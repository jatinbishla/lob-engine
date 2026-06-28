#include <iostream>
#include "lob/order_book.hpp"

int main() {
    lob::OrderBook book;

    // Post two asks at price 100 and 101
    book.submit({1, lob::Side::Sell, lob::OrderType::Limit, 10'100, 50});
    book.submit({2, lob::Side::Sell, lob::OrderType::Limit, 10'101, 50});

    // Aggressive buy crosses through both levels
    auto result = book.submit({3, lob::Side::Buy, lob::OrderType::Limit, 10'101, 80});

    std::cout << "Trades: " << result.trades.size() << "\n";
    for (const auto& t : result.trades)
        std::cout << "  maker=" << t.maker_id
                  << " taker=" << t.taker_id
                  << " qty="   << t.quantity
                  << " px="    << t.price << "\n";
    return 0;
}
