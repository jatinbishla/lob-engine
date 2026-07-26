#include "lob/itch_parser.hpp"

namespace lob::itch {

namespace {
// Field offsets within each message body (ITCH 5.0 spec). Offset 0 is the type
// byte; offsets 5..10 hold the 6-byte timestamp (unused for reconstruction).
constexpr std::size_t kRef       = 11; // 8-byte order reference number
// Add Order ('A' = 36 bytes, 'F' = 40 bytes with attribution)
constexpr std::size_t kAddSide   = 19; // 1 byte 'B'/'S'
constexpr std::size_t kAddShares = 20; // 4 bytes
constexpr std::size_t kAddPrice  = 32; // 4 bytes, 4 implied decimals
constexpr std::size_t kAddLen    = 36;
// Order Executed ('E' = 31 bytes)
constexpr std::size_t kExecShares = 19; // 4 bytes
constexpr std::size_t kExecLen    = 31;
// Order Cancel ('X' = 23 bytes)
constexpr std::size_t kCancelShares = 19; // 4 bytes
constexpr std::size_t kCancelLen    = 23;
// Order Delete ('D' = 19 bytes)
constexpr std::size_t kDeleteLen = 19;
// Order Replace ('U' = 35 bytes)
constexpr std::size_t kReplNewRef  = 19; // 8 bytes
constexpr std::size_t kReplShares  = 27; // 4 bytes
constexpr std::size_t kReplPrice   = 31; // 4 bytes
constexpr std::size_t kReplLen     = 35;
} // namespace

void ItchHandler::handle_add(const std::uint8_t* d) {
    OrderId  ref    = read_be<std::uint64_t>(d + kRef);
    Side     side   = (d[kAddSide] == 'B') ? Side::Buy : Side::Sell;
    Quantity shares = read_be<std::uint32_t>(d + kAddShares);
    Price    price  = static_cast<Price>(read_be<std::uint32_t>(d + kAddPrice));
    book_.rest({ ref, side, OrderType::Limit, price, shares });
    ++stats_.add_orders;
}

void ItchHandler::handle_executed(const std::uint8_t* d) {
    OrderId  ref    = read_be<std::uint64_t>(d + kRef);
    Quantity shares = read_be<std::uint32_t>(d + kExecShares);
    book_.reduce(ref, shares);
    ++stats_.executions;
}

void ItchHandler::handle_cancel(const std::uint8_t* d) {
    OrderId  ref    = read_be<std::uint64_t>(d + kRef);
    Quantity shares = read_be<std::uint32_t>(d + kCancelShares);
    book_.reduce(ref, shares);
    ++stats_.cancels;
}

void ItchHandler::handle_delete(const std::uint8_t* d) {
    OrderId ref = read_be<std::uint64_t>(d + kRef);
    book_.cancel(ref);
    ++stats_.deletes;
}

void ItchHandler::handle_replace(const std::uint8_t* d) {
    OrderId  old_ref = read_be<std::uint64_t>(d + kRef);
    OrderId  new_ref = read_be<std::uint64_t>(d + kReplNewRef);
    Quantity shares  = read_be<std::uint32_t>(d + kReplShares);
    Price    price   = static_cast<Price>(read_be<std::uint32_t>(d + kReplPrice));
    book_.replace(old_ref, new_ref, price, shares);
    ++stats_.replaces;
}

bool ItchHandler::process_message(const std::uint8_t* data, std::size_t len) {
    if (len == 0) { ++stats_.malformed; return false; }
    ++stats_.messages_processed;

    // Each handler needs its full fixed-length body; a truncated message is
    // dropped rather than read out of bounds.
    switch (static_cast<MsgType>(data[0])) {
        case MsgType::AddOrder:
        case MsgType::AddOrderMPID:
            if (len < kAddLen) { ++stats_.malformed; return false; }
            handle_add(data); return true;
        case MsgType::OrderExecuted:
            if (len < kExecLen) { ++stats_.malformed; return false; }
            handle_executed(data); return true;
        case MsgType::OrderCancel:
            if (len < kCancelLen) { ++stats_.malformed; return false; }
            handle_cancel(data); return true;
        case MsgType::OrderDelete:
            if (len < kDeleteLen) { ++stats_.malformed; return false; }
            handle_delete(data); return true;
        case MsgType::OrderReplace:
            if (len < kReplLen) { ++stats_.malformed; return false; }
            handle_replace(data); return true;
        default:
            ++stats_.skipped; return false; // system events, trades, etc.
    }
}

ParseStats ItchHandler::process_stream(std::span<const std::uint8_t> stream) {
    std::size_t pos = 0;
    while (pos + 2 <= stream.size()) {
        std::size_t msg_len = read_be<std::uint16_t>(stream.data() + pos);
        pos += 2;
        if (msg_len == 0 || pos + msg_len > stream.size()) break; // truncated tail
        process_message(stream.data() + pos, msg_len);
        pos += msg_len;
    }
    return stats_;
}

double ItchHandler::microprice() const {
    Price bid_px, ask_px;
    if (!book_.best_bid(bid_px) || !book_.best_ask(ask_px)) return 0.0;
    double Qbid = static_cast<double>(book_.depth_at(Side::Buy,  bid_px));
    double Qask = static_cast<double>(book_.depth_at(Side::Sell, ask_px));
    if (Qbid + Qask == 0.0) return (bid_px + ask_px) / 2.0;
    return bid_px * (Qask / (Qbid + Qask)) +
           ask_px * (Qbid / (Qbid + Qask));
}

double ItchHandler::order_flow_imbalance() const {
    Price bid_px, ask_px;
    if (!book_.best_bid(bid_px) || !book_.best_ask(ask_px)) return 0.0;
    double Qbid = static_cast<double>(book_.depth_at(Side::Buy,  bid_px));
    double Qask = static_cast<double>(book_.depth_at(Side::Sell, ask_px));
    if (Qbid + Qask == 0.0) return 0.0;
    return (Qbid - Qask) / (Qbid + Qask);
}

} // namespace lob::itch
