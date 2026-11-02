// Matted Engine - memory allocators: linear arena + frame scratch
#pragma once
#include <cstdlib>
#include <vector>
#include "engine/core/mtt_types.h"

namespace mtt::mem {

namespace detail {
inline void* aligned_alloc64(size_t bytes) {
#ifdef _WIN32
    return ::_aligned_malloc(bytes, 64);
#else
    return ::std::aligned_alloc(64, (bytes + 63) & ~size_t(63));
#endif
}
inline void aligned_free64(void* p) {
#ifdef _WIN32
    ::_aligned_free(p);
#else
    ::std::free(p);
#endif
}
} // namespace detail

// Monotonic arena made of chained blocks. O(1) bump allocation, bulk reset.
// Not thread-safe by itself; grab a per-thread Scratch for transient work.
class Arena {
public:
    explicit Arena(size_t first_block = 1 << 20) : block_size_(first_block) {}
    ~Arena() { free_all(); }

    void* alloc(size_t size, size_t align = 16) {
        size = (size + align - 1) & ~(align - 1);
        if (blocks_.empty() || used_ + size > blocks_.back().size) {
            size_t bs = size > block_size_ ? size : block_size_;
            blocks_.push_back(Block{ static_cast<u8*>(detail::aligned_alloc64(bs)), bs });
            used_ = 0;
        }
        u8* p = blocks_.back().data + used_;
        used_ += size;
        return p;
    }
    template <typename T>
    T* alloc_array(size_t n) {
        return static_cast<T*>(alloc(sizeof(T) * n, alignof(T)));
    }

    void reset() {
        // keep the largest block, drop the rest
        if (blocks_.size() > 1) {
            for (size_t i = 0; i + 1 < blocks_.size(); ++i) {
                detail::aligned_free64(blocks_[i].data);
            }
            Block last = blocks_.back();
            blocks_.clear();
            blocks_.push_back(last);
        }
        used_ = 0;
    }
    void free_all() {
        for (auto& b : blocks_) detail::aligned_free64(b.data);
        blocks_.clear();
        used_ = 0;
    }

private:
    struct Block { u8* data; size_t size; };
    std::vector<Block> blocks_;
    size_t used_ = 0;
    size_t block_size_;
};

// RAII scope marking; anything allocated inside is dropped at scope exit.
struct ScopedMark {
    explicit ScopedMark(Arena& a) : arena_(&a) {}
    ~ScopedMark() { arena_->reset(); }
    Arena* arena_;
};

// Per-thread scratch arena for transient frame allocations (tile lists,
// staging buffers...). Memory is reused across frames; grows to high-water.
Arena& scratch();
struct ScratchScope {
    ScratchScope() = default;
    ~ScratchScope() { scratch().reset(); }
};

} // namespace mtt::mem
