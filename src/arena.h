#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Arena (bump) allocator for construction-phase memory.
//
// All intermediate trie nodes are born and die together, so a linear
// bump allocator gives O(1) alloc with zero fragmentation.  The entire
// arena is freed in one shot when construction finishes.
//
// Thread safety: NOT thread-safe.  Used only in single-threaded build path.
class Arena {
public:
    // `block_size` is the default capacity of each memory block (1 MiB default).
    explicit Arena(size_t block_size = 1 << 20);
    ~Arena();

    // Non-copyable, movable.
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    // Allocate `size` bytes aligned to `align`.
    // Aborts on OOM (construction failure is unrecoverable).
    void* alloc(size_t size, size_t align = alignof(std::max_align_t));

    // Typed construction helper.  Allocates and placement-news a T.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
            "Arena only supports trivially destructible types in create");
        void* p = alloc(sizeof(T), alignof(T));
        return ::new (p) T(static_cast<Args&&>(args)...);
    }

    // Allocate a contiguous array of `count` elements of type T.
    // Elements are default-initialized.
    template <typename T>
    T* alloc_array(size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
            "Arena only supports trivially destructible types in alloc_array");
        if (count == 0) return nullptr;
        void* p = alloc(sizeof(T) * count, alignof(T));
        T* arr = static_cast<T*>(p);
        for (size_t i = 0; i < count; ++i) {
            ::new (&arr[i]) T();
        }
        return arr;
    }

    // Reset the arena: reuse all memory blocks.  Invalidates ALL pointers
    // previously returned by alloc()/create().
    void reset();

    // Total bytes allocated (across all blocks).
    size_t bytes_used() const;

    // Total bytes reserved (capacity across all blocks).
    size_t bytes_reserved() const;

private:
    struct Block {
        uint8_t* data;
        size_t   capacity;
        size_t   used;
    };

    std::vector<Block> blocks_;
    size_t             block_size_;  // Default block capacity

    // Allocate a new block of at least `min_size` bytes.
    void add_block(size_t min_size);

    // Align `offset` up to `align`.
    static size_t align_up(size_t offset, size_t align);
};

}  // namespace internal
}  // namespace fuzzyfst
