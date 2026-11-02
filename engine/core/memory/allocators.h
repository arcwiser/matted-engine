// Matted Engine - memory allocators: linear arena + frame scratch
#pragma once
#include <vector>
#include "engine/core/mtt_types.h"

namespace mtt::mem {

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
            blocks_.push_back(Block{ new (std::align_val_t(64)) u8[bs], bs });
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
                operator delete[] (blocks_[i].data, std::align_val_t(64));
            }
            Block last = blocks_.back();
            blocks_.clear();
            blocks_.push_back(last);
        }
        used_ = 0;
    }
    void free_all() {
        for (auto& b : blocks_) operator delete[] (b.data, std::align_val_t(64));
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
