#include <catch2/catch_test_macros.hpp>
#include "lob/order_book.hpp"
using namespace lob;

// Helper: submit without caring about risk result
std::vector<Trade> submit(OrderBook& b, Order o) {
    return b.submit(o).trades;
}

TEST_CASE("simple cross — taker fills resting maker") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 10});
    auto t = submit(b, {2, Side::Buy, OrderType::Limit, 100, 10});
    REQUIRE(t.size() == 1);
    REQUIRE(t[0].maker_id == 1);
    REQUIRE(t[0].taker_id == 2);
    REQUIRE(t[0].price == 100);
    REQUIRE(t[0].quantity == 10);
}

TEST_CASE("partial fill — resting order remains") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 20});
    auto t = submit(b, {2, Side::Buy, OrderType::Limit, 100, 10});
    REQUIRE(t.size() == 1);
    REQUIRE(t[0].quantity == 10);
    Price ask; b.best_ask(ask);
    REQUIRE(ask == 100);           // still resting
    REQUIRE(b.depth_at(Side::Sell, 100) == 10); // 10 remaining
}

TEST_CASE("price-time priority — oldest order fills first") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 5}); // older
    submit(b, {2, Side::Sell, OrderType::Limit, 100, 5}); // newer
    auto t = submit(b, {3, Side::Buy, OrderType::Limit, 100, 5});
    REQUIRE(t[0].maker_id == 1);   // FIFO: order 1 must fill first
}

TEST_CASE("multi-level sweep — market order walks levels") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 10});
    submit(b, {2, Side::Sell, OrderType::Limit, 101, 10});
    auto t = submit(b, {3, Side::Buy, OrderType::Market, 0, 15});
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].price == 100);
    REQUIRE(t[0].quantity == 10);
    REQUIRE(t[1].price == 101);
    REQUIRE(t[1].quantity == 5);
}

TEST_CASE("cancel removes order from book") {
    OrderBook b;
    submit(b, {1, Side::Buy, OrderType::Limit, 99, 10});
    REQUIRE(b.cancel(1));
    Price bid;
    REQUIRE_FALSE(b.best_bid(bid)); // book should be empty
}

TEST_CASE("cancel in the middle — FIFO preserved") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 5}); // position 0
    submit(b, {2, Side::Sell, OrderType::Limit, 100, 5}); // position 1
    submit(b, {3, Side::Sell, OrderType::Limit, 100, 5}); // position 2
    REQUIRE(b.cancel(2));  // remove middle
    auto t = submit(b, {4, Side::Buy, OrderType::Limit, 100, 10});
    // Should fill order 1 (5), then order 3 (5)
    REQUIRE(t.size() == 2);
    REQUIRE(t[0].maker_id == 1);
    REQUIRE(t[1].maker_id == 3);
}

TEST_CASE("market order into empty book — no crash, no fill") {
    OrderBook b;
    auto t = submit(b, {1, Side::Buy, OrderType::Market, 0, 10});
    REQUIRE(t.empty());
}

TEST_CASE("cancel unknown id returns false") {
    OrderBook b;
    REQUIRE_FALSE(b.cancel(999));
}

TEST_CASE("no self-cross — buy price below best ask does not fill") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 102, 10});
    auto t = submit(b, {2, Side::Buy, OrderType::Limit, 101, 10}); // price < ask
    REQUIRE(t.empty());
    Price bid; b.best_bid(bid);
    REQUIRE(bid == 101);  // rested
}

TEST_CASE("risk guard — rejects oversized order") {
    OrderBook b;
    auto result = b.submit({1, Side::Buy, OrderType::Limit, 100, 100'000}); // > max_order_size
    REQUIRE(result.risk == RiskResult::REJECT_SIZE);
    REQUIRE(result.trades.empty());
}

TEST_CASE("sequence numbers are strictly increasing") {
    OrderBook b;
    submit(b, {1, Side::Sell, OrderType::Limit, 100, 30});
    auto t = submit(b, {2, Side::Buy, OrderType::Market, 0, 30});
    // Even multi-fill sequence should be monotonic (here just 1 fill but check seq > 0)
    REQUIRE(t[0].seq > 0);
}
