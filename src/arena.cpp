#include "arena.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace fuzzyfst {
namespace internal {

Arena::Arena(size_t block_size)
    : block_size_(block_size) {
    assert(block_size > 0);
}

Arena::~Arena() {
    for (auto& b : blocks_) {
        std::free(b.data);
    }
}

Arena::Arena(Arena&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      block_size_(other.block_size_) {
    other.blocks_.clear();
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        for (auto& b : blocks_) {
            std::free(b.data);
        }
        blocks_ = std::move(other.blocks_);
        block_size_ = other.block_size_;
        other.blocks_.clear();
    }
    return *this;
}

size_t Arena::align_up(size_t offset, size_t align) {
    // align must be a power of two
    return (offset + align - 1) & ~(align - 1);
}

void Arena::add_block(size_t min_size) {
    assert(min_size <= (SIZE_MAX / 2) && "requested allocation too large");
    size_t cap = std::max(block_size_, min_size);
    auto* data = static_cast<uint8_t*>(std::malloc(cap));
    if (!data) {
        // Construction failure is unrecoverable.
        std::abort();
    }
    blocks_.push_back(Block{data, cap, 0});
}

void* Arena::alloc(size_t size, size_t align) {
    assert(align > 0 && ((align & (align - 1)) == 0) && "alignment must be power of two");

    // Zero-size allocations return nullptr.
    // create<T>() never hits this path because sizeof(T) >= 1 always.
    // alloc_array<T>() guards count == 0 before calling alloc().
    if (size == 0) {
        return nullptr;
    }

    // Try to fit in the current (last) block.
    if (!blocks_.empty()) {
        auto& cur = blocks_.back();
        uintptr_t curr_addr = reinterpret_cast<uintptr_t>(cur.data) + cur.used;
        uintptr_t aligned_addr = align_up(curr_addr, align);
        size_t new_used = aligned_addr - reinterpret_cast<uintptr_t>(cur.data) + size;
        
        if (new_used <= cur.capacity) {
            cur.used = new_used;
            return reinterpret_cast<void*>(aligned_addr);
        }
    }

    // Need a new block.
    // ensure enough room even after maximum alignment padding
    add_block(size + align);  
    auto& cur = blocks_.back();
    uintptr_t curr_addr = reinterpret_cast<uintptr_t>(cur.data) + cur.used;
    uintptr_t aligned_addr = align_up(curr_addr, align);
    size_t new_used = aligned_addr - reinterpret_cast<uintptr_t>(cur.data) + size;
    
    cur.used = new_used;
    return reinterpret_cast<void*>(aligned_addr);
}

void Arena::reset() {
    for (auto& b : blocks_) {
        b.used = 0;
    }
}

size_t Arena::bytes_used() const {
    size_t total = 0;
    for (const auto& b : blocks_) {
        total += b.used;
    }
    return total;
}

size_t Arena::bytes_reserved() const {
    size_t total = 0;
    for (const auto& b : blocks_) {
        total += b.capacity;
    }
    return total;
}

}  // namespace internal
}  // namespace fuzzyfst
