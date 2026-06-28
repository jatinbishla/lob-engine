#include <iostream>
#include "lob/types.hpp"

int main() {
    lob::Order o{ 1, lob::Side::Buy, lob::OrderType::Limit, 10'099, 100 };
    std::cout << "Order " << o.id
              << " Buy " << o.quantity
              << " @ " << o.price << " ticks\n";
    return 0;
}
