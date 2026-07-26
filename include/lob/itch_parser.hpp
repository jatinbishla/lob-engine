#pragma once
#include "lob/order_book.hpp"
#include <cstdint>
#include <span>
#include <string>

namespace lob::itch {

// Read a big-endian integer of type T from a byte pointer. ITCH is a
// network-order (big-endian) binary protocol, so every multi-byte field must
// be byte-swapped on a little-endian host.
template <typename T>
T read_be(const std::uint8_t* p) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v = static_cast<T>((v << 8) | p[i]);
    return v;
}

// NASDAQ TotalView-ITCH 5.0 message types we reconstruct the book from.
enum class MsgType : char {
    AddOrder      = 'A', // add, no attribution
    AddOrderMPID  = 'F', // add, with market-participant attribution
    OrderExecuted = 'E', // resting order (partially) executed
    OrderCancel   = 'X', // resting order shares cancelled
    OrderDelete   = 'D', // resting order fully removed
    OrderReplace  = 'U', // cancel-and-replace (new ref, price, size)
};

struct ParseStats {
    std::uint64_t messages_processed = 0;
    std::uint64_t add_orders         = 0;
    std::uint64_t executions         = 0;
    std::uint64_t cancels            = 0;
    std::uint64_t deletes            = 0;
    std::uint64_t replaces           = 0;
    std::uint64_t skipped            = 0; // message types we don't reconstruct
    std::uint64_t malformed          = 0; // truncated / wrong-length messages
};

// Parses an ITCH stream and drives an OrderBook into a faithful L3 book.
// The order reference number is used directly as the book's OrderId (ITCH refs
// are unique for the trading day), so no id-translation table is needed.
class ItchHandler {
public:
    explicit ItchHandler(OrderBook& book) : book_(book) {}

    // Process one message body (points at the type byte; len is its full length).
    // Returns true if the message was recognized and applied.
    bool process_message(const std::uint8_t* data, std::size_t len);

    // Process a length-prefixed stream: each message is preceded by a 2-byte
    // big-endian length. Returns aggregate stats.
    ParseStats process_stream(std::span<const std::uint8_t> stream);

    // ---- Microstructure signals (from current top-of-book) ----
    // Depth-weighted fair price: leans toward the side with less size, because
    // the thin side is where the price is more likely to move next.
    //   microprice = Pbid*(Qask/(Qbid+Qask)) + Pask*(Qbid/(Qbid+Qask))
    double microprice() const;

    // Order-flow imbalance ∈ [-1, 1]: +1 = all bid pressure, -1 = all ask.
    //   OFI = (Qbid - Qask) / (Qbid + Qask)
    double order_flow_imbalance() const;

    const ParseStats& stats() const { return stats_; }

private:
    OrderBook& book_;
    ParseStats stats_;

    void handle_add(const std::uint8_t* d);
    void handle_executed(const std::uint8_t* d);
    void handle_cancel(const std::uint8_t* d);
    void handle_delete(const std::uint8_t* d);
    void handle_replace(const std::uint8_t* d);
};

} // namespace lob::itch
