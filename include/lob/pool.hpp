#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <new>

namespace lob {

// Fixed-size free-list arena for order-queue nodes.
//
// The matcher is single-threaded by design (price-time priority is a total
// ordering), so a single process-wide arena needs no locks. Nodes are handed
// out from pre-allocated, cache-line-rounded slots; freeing pushes the slot
// back onto an intrusive free list. This removes the per-order malloc/free that
// std::list would otherwise pay on every resting order — the hot path never
// touches the system allocator once the arena is warm.
class NodeArena {
public:
    void* allocate(std::size_t bytes) {
        if (bytes > slot_) {            // first allocation fixes the node size
            slot_ = round_up(bytes);
            free_ = nullptr;            // (only grows before any node is live)
        }
        if (!free_) grow();
        void* p = free_;
        free_ = *static_cast<void**>(p);
        return p;
    }

    void deallocate(void* p) noexcept {
        *static_cast<void**>(p) = free_;
        free_ = p;
    }

    static NodeArena& instance() {
        static NodeArena a;
        return a;
    }

private:
    static constexpr std::size_t kAlign = alignof(std::max_align_t);
    static constexpr std::size_t kSlotsPerChunk = 4096;

    static std::size_t round_up(std::size_t n) {
        std::size_t s = (n < sizeof(void*)) ? sizeof(void*) : n;
        return (s + kAlign - 1) & ~(kAlign - 1);
    }

    void grow() {
        // max_align_t storage keeps every slot suitably aligned for any node.
        const std::size_t units = (slot_ * kSlotsPerChunk + sizeof(std::max_align_t) - 1)
                                  / sizeof(std::max_align_t);
        chunks_.push_back(std::make_unique<std::max_align_t[]>(units));
        auto* base = reinterpret_cast<std::byte*>(chunks_.back().get());
        for (std::size_t i = 0; i < kSlotsPerChunk; ++i) {
            void* slot = base + i * slot_;
            *static_cast<void**>(slot) = free_;
            free_ = slot;
        }
    }

    std::size_t slot_ = 0;
    void*       free_ = nullptr;
    std::vector<std::unique_ptr<std::max_align_t[]>> chunks_;
};

// STL-compatible allocator that draws fixed-size nodes from the shared arena.
// Stateless and default-constructible, so it slots into std::map's value type
// without disturbing operator[].
template <typename T>
struct PoolAllocator {
    using value_type = T;

    PoolAllocator() noexcept = default;
    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(NodeArena::instance().allocate(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t) noexcept {
        NodeArena::instance().deallocate(p);
    }

    template <typename U> bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const PoolAllocator<U>&) const noexcept { return false; }
};

} // namespace lob
