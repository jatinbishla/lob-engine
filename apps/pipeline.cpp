// M7 — concurrent ingest + match pipeline.
//
// One producer thread feeds Orders into a lock-free SPSC ring; one consumer
// thread owns the OrderBook and drains the ring, submitting each order. The
// matcher stays strictly single-threaded — only ONE thread ever touches the
// book — so price-time priority stays a clean total ordering with zero locks
// inside the matching path. The ring is the only shared state, and it is
// synchronized purely by the acquire/release handshake.
//
// We run the identical order stream twice: once straight-line (single thread)
// and once through the pipeline, then assert the trade logs are bit-for-bit
// identical. Determinism is the whole point — threading must not change results.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"

using namespace lob;

namespace {

// A deterministic synthetic stream: resting sell wall plus crossing buys.
std::vector<Order> make_stream(int n) {
    std::vector<Order> orders;
    orders.reserve(n);
    OrderId id = 0;
    // Resting quantity kept small so notional (price * qty) stays under the
    // risk guard's 5M cap — otherwise the wall would be REJECT_NOTIONAL.
    for (int i = 0; i < 100; ++i)
        orders.push_back({++id, Side::Sell, OrderType::Limit,
                          10'100 + (i % 20), 100});
    for (int i = 0; i < n - 100; ++i)
        orders.push_back({++id, Side::Buy, OrderType::Limit,
                          10'100 + (i % 25),
                          static_cast<Quantity>(1 + (i % 7))});
    return orders;
}

// Compact fingerprint of a trade so we can compare the two runs cheaply.
std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}
std::uint64_t fingerprint(const std::vector<Trade>& trades) {
    std::uint64_t h = 0;
    for (const auto& t : trades) {
        h = mix(h, t.maker_id);
        h = mix(h, t.taker_id);
        h = mix(h, static_cast<std::uint64_t>(t.price));
        h = mix(h, t.quantity);
        h = mix(h, t.seq);
    }
    return h;
}

std::vector<Trade> run_single_threaded(const std::vector<Order>& stream) {
    OrderBook book;
    std::vector<Trade> all;
    for (const auto& o : stream) {
        auto r = book.submit(o);
        for (const auto& t : r.trades) all.push_back(t);
    }
    return all;
}

std::vector<Trade> run_pipeline(const std::vector<Order>& stream) {
    SpscRing<Order, 1024> ring;
    std::atomic<bool> done{false};
    std::vector<Trade> all;

    // Consumer owns the book. It is the only thread that touches it.
    std::thread consumer([&] {
        OrderBook book;
        Order o;
        std::size_t seen = 0;
        const std::size_t total = stream.size();
        while (seen < total) {
            if (ring.pop(o)) {
                auto r = book.submit(o);
                for (const auto& t : r.trades) all.push_back(t);
                ++seen;
            } else if (done.load(std::memory_order_acquire) && ring.empty()) {
                break; // producer finished and ring drained
            }
        }
    });

    // Producer thread feeds the ring, spinning while it is full (back-pressure).
    for (const auto& o : stream)
        while (!ring.push(o)) { /* spin: ring full, let consumer catch up */ }
    done.store(true, std::memory_order_release);

    consumer.join();
    return all;
}

} // namespace

int main() {
    const auto stream = make_stream(50'000);

    const auto single = run_single_threaded(stream);
    const auto piped  = run_pipeline(stream);

    const auto h_single = fingerprint(single);
    const auto h_piped  = fingerprint(piped);

    std::cout << "=== LOB Engine SPSC Pipeline ===\n";
    std::cout << "Orders streamed:      " << stream.size() << "\n";
    std::cout << "Trades (single-thread): " << single.size() << "\n";
    std::cout << "Trades (pipeline):      " << piped.size()  << "\n";
    std::cout << "Fingerprint single:   " << std::hex << h_single << std::dec << "\n";
    std::cout << "Fingerprint pipeline: " << std::hex << h_piped  << std::dec << "\n";

    const bool ok = (single.size() == piped.size()) && (h_single == h_piped);
    std::cout << (ok ? "[PASS] pipeline is deterministic and matches single-threaded\n"
                     : "[FAIL] pipeline diverged from single-threaded\n");
    return ok ? 0 : 1;
}
