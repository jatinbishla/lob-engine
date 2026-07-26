#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <new>

namespace lob {

// Wait-free single-producer / single-consumer ring buffer.
//
// Exactly one thread calls push(), exactly one (different) thread calls pop().
// Correctness rests on the acquire/release handshake:
//   - The producer publishes the slot write with a RELEASE store to tail_.
//   - The consumer observes it with an ACQUIRE load of tail_, which guarantees
//     the buffer write is visible before the index. release/acquire is exactly
//     the ordering we need — seq_cst would add a full barrier for nothing.
//
// head_ and tail_ live on separate cache lines (alignas) so the producer and
// consumer never contend on the same line (false sharing would serialize them).
template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2, "ring needs at least two slots");
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t kLine = std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t kLine = 64;
#endif

    alignas(kLine) std::array<T, N> buf_;
    alignas(kLine) std::atomic<std::size_t> tail_{0}; // producer writes, consumer reads
    alignas(kLine) std::atomic<std::size_t> head_{0}; // consumer writes, producer reads

public:
    // Producer side. Returns false if the ring is full (one slot is kept empty
    // to distinguish full from empty).
    bool push(const T& v) {
        const std::size_t t    = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & (N - 1);
        if (next == head_.load(std::memory_order_acquire)) return false; // full
        buf_[t] = v;
        tail_.store(next, std::memory_order_release); // publish the write
        return true;
    }

    // Consumer side. Returns false if the ring is empty.
    bool pop(T& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false; // empty
        out = buf_[h];
        head_.store((h + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N - 1; }
};

} // namespace lob
