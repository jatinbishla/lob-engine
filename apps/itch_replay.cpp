// M8 — ITCH 5.0 replay: reconstruct an L3 book from a NASDAQ TotalView-ITCH
// stream and report microstructure signals.
//
//   ./itch_replay              -> replay a built-in synthetic stream (CI-friendly)
//   ./itch_replay feed.itch    -> replay a real length-prefixed ITCH 5.0 file
//
// The synthetic generator emits genuine ITCH-format bytes (big-endian,
// length-prefixed) so the same parser handles both paths — the only difference
// is where the bytes come from.

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "lob/itch_parser.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

// ---- Synthetic ITCH 5.0 stream builder ----

void put_be(std::vector<std::uint8_t>& b, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * (n - 1 - i))) & 0xFF));
}

// Append one length-prefixed message: 2-byte big-endian length, then body.
void emit(std::vector<std::uint8_t>& stream, const std::vector<std::uint8_t>& body) {
    put_be(stream, body.size(), 2);
    stream.insert(stream.end(), body.begin(), body.end());
}

std::vector<std::uint8_t> add_order(std::uint64_t ref, char side,
                                    std::uint32_t shares, std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('A');
    put_be(m, 1, 2);            // stock locate
    put_be(m, 0, 2);            // tracking number
    put_be(m, 0, 6);            // timestamp
    put_be(m, ref, 8);          // order reference number
    m.push_back(static_cast<std::uint8_t>(side));
    put_be(m, shares, 4);
    for (int i = 0; i < 8; ++i) m.push_back(' '); // stock symbol (padded)
    put_be(m, price, 4);        // price, 4 implied decimals
    return m;                   // 36 bytes
}

std::vector<std::uint8_t> exec_order(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('E');
    put_be(m, 1, 2); put_be(m, 0, 2); put_be(m, 0, 6);
    put_be(m, ref, 8);
    put_be(m, shares, 4);
    put_be(m, 0, 8);            // match number
    return m;                   // 31 bytes
}

std::vector<std::uint8_t> cancel_order(std::uint64_t ref, std::uint32_t shares) {
    std::vector<std::uint8_t> m;
    m.push_back('X');
    put_be(m, 1, 2); put_be(m, 0, 2); put_be(m, 0, 6);
    put_be(m, ref, 8);
    put_be(m, shares, 4);
    return m;                   // 23 bytes
}

std::vector<std::uint8_t> delete_order(std::uint64_t ref) {
    std::vector<std::uint8_t> m;
    m.push_back('D');
    put_be(m, 1, 2); put_be(m, 0, 2); put_be(m, 0, 6);
    put_be(m, ref, 8);
    return m;                   // 19 bytes
}

std::vector<std::uint8_t> replace_order(std::uint64_t old_ref, std::uint64_t new_ref,
                                        std::uint32_t shares, std::uint32_t price) {
    std::vector<std::uint8_t> m;
    m.push_back('U');
    put_be(m, 1, 2); put_be(m, 0, 2); put_be(m, 0, 6);
    put_be(m, old_ref, 8);
    put_be(m, new_ref, 8);
    put_be(m, shares, 4);
    put_be(m, price, 4);
    return m;                   // 35 bytes
}

// Build a book with a clear top-of-book so we can verify signals deterministically.
// Best bid 100.00 (price field 1000000) with 300 shares over two orders,
// best ask 100.02 (1000200) with 100 shares. Plus deeper levels and some churn
// that exercises every message handler (add/exec/cancel/delete/replace).
std::vector<std::uint8_t> make_synthetic_stream() {
    std::vector<std::uint8_t> s;
    // Bids
    emit(s, add_order(1, 'B', 200, 1000000)); // 100.00 x200
    emit(s, add_order(2, 'B', 100, 1000000)); // 100.00 x100  (same level, FIFO)
    emit(s, add_order(3, 'B', 500,  999900)); // 99.99  x500  (deeper)
    // Asks
    emit(s, add_order(4, 'S', 100, 1000200)); // 100.02 x100  (best ask)
    emit(s, add_order(5, 'S', 400, 1000300)); // 100.03 x400  (deeper)
    // Churn: a temporary ask is partially executed then fully cancelled;
    // a deep ask is deleted and re-added; a deep bid is replaced in place.
    emit(s, add_order(6, 'S', 250, 1000400)); // temp order
    emit(s, exec_order(6, 100));              // 250 -> 150 remaining
    emit(s, cancel_order(6, 150));            // 150 -> 0, level removed
    emit(s, delete_order(5));                 // remove 100.03 x400
    emit(s, add_order(5, 'S', 400, 1000300)); // re-add it
    emit(s, replace_order(3, 7, 600, 999800));// bid ref3 -> ref7 @ 99.98 x600
    return s;
}

} // namespace

int main(int argc, char** argv) {
    OrderBook book;
    itch::ItchHandler handler(book);

    std::vector<std::uint8_t> bytes;
    std::string source;

    if (argc >= 2) {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) { std::cerr << "cannot open " << argv[1] << "\n"; return 2; }
        bytes.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
        source = argv[1];
    } else {
        bytes  = make_synthetic_stream();
        source = "synthetic";
    }

    auto t0 = std::chrono::steady_clock::now();
    auto st = handler.process_stream(bytes);
    auto t1 = std::chrono::steady_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double mps  = secs > 0 ? st.messages_processed / secs : 0.0;

    std::cout << "=== LOB Engine ITCH 5.0 Replay ===\n";
    std::cout << "Source:   " << source << " (" << bytes.size() << " bytes)\n";
    std::cout << "Messages: " << st.messages_processed
              << "  (add=" << st.add_orders
              << " exec=" << st.executions
              << " cancel=" << st.cancels
              << " delete=" << st.deletes
              << " replace=" << st.replaces
              << " skipped=" << st.skipped
              << " malformed=" << st.malformed << ")\n";
    std::cout << "Throughput: " << static_cast<std::uint64_t>(mps) << " msgs/sec\n";

    Price bid, ask;
    bool has_bid = book.best_bid(bid), has_ask = book.best_ask(ask);
    std::cout << "Top of book: ";
    if (has_bid) std::cout << "bid " << bid << " x" << book.depth_at(Side::Buy, bid);
    else         std::cout << "bid --";
    std::cout << " | ";
    if (has_ask) std::cout << "ask " << ask << " x" << book.depth_at(Side::Sell, ask);
    else         std::cout << "ask --";
    std::cout << "\n";
    std::cout << "Microprice:            " << handler.microprice() << "\n";
    std::cout << "Order-flow imbalance:  " << handler.order_flow_imbalance() << "\n";

    // On the synthetic path the expected book state is known exactly, so we can
    // assert the reconstruction and both signals are correct.
    if (source == "synthetic") {
        bool ok = has_bid && has_ask
               && bid == 1000000 && book.depth_at(Side::Buy, bid)  == 300
               && ask == 1000200 && book.depth_at(Side::Sell, ask) == 100
               && handler.microprice() == 1000150.0
               && handler.order_flow_imbalance() == 0.5
               && st.malformed == 0;
        std::cout << (ok ? "[PASS] reconstruction + signals match expected\n"
                         : "[FAIL] reconstruction mismatch\n");
        return ok ? 0 : 1;
    }
    return 0;
}
