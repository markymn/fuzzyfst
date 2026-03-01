// test_arena.cpp — Tests for Arena bump allocator

#include "arena.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace fuzzyfst::internal;

// ── Test: basic allocation ──────────────────────────────────

static void test_basic_alloc() {
    Arena arena(4096);

    void* p1 = arena.alloc(16);
    assert(p1 != nullptr);

    void* p2 = arena.alloc(32);
    assert(p2 != nullptr);

    // Allocations should not overlap.
    assert(p1 != p2);

    // p2 should come after p1 in the same block.
    assert(static_cast<uint8_t*>(p2) >= static_cast<uint8_t*>(p1) + 16);

    assert(arena.bytes_used() > 0);
    std::printf("  PASS: test_basic_alloc\n");
}

// ── Test: alignment ─────────────────────────────────────────

static void test_alignment() {
    Arena arena(4096);

    // Test various alignments.
    for (size_t align : {1, 2, 4, 8, 16, 32, 64}) {
        void* p = arena.alloc(7, align);  // odd size
        assert(p != nullptr);
        auto addr = reinterpret_cast<uintptr_t>(p);
        assert((addr % align) == 0);
    }

    std::printf("  PASS: test_alignment\n");
}

// ── Test: zero-size allocation ──────────────────────────────

static void test_zero_alloc() {
    Arena arena(4096);
    // Zero-size alloc returns nullptr by contract.
    // This is safe because create<T>() always passes sizeof(T) >= 1,
    // and alloc_array<T>() guards count == 0 before calling alloc().
    void* p = arena.alloc(0);
    assert(p == nullptr);
    std::printf("  PASS: test_zero_alloc\n");
}

// ── Test: large allocation (exceeds block size) ─────────────

static void test_large_alloc() {
    Arena arena(256);  // Small default block.

    // Allocate something much larger than the block size.
    void* p = arena.alloc(1024);
    assert(p != nullptr);

    // Should still be able to allocate more after the oversized block.
    void* p2 = arena.alloc(16);
    assert(p2 != nullptr);

    assert(arena.bytes_used() >= 1024 + 16);
    std::printf("  PASS: test_large_alloc\n");
}

// ── Test: create<T>() typed helper ──────────────────────────

struct TestStruct {
    uint32_t a;
    uint64_t b;
    uint8_t  c;
};

static void test_create() {
    Arena arena(4096);

    auto* obj = arena.create<TestStruct>();
    assert(obj != nullptr);
    obj->a = 42;
    obj->b = 0xDEADBEEF;
    obj->c = 0xFF;

    // Verify alignment of the struct.
    auto addr = reinterpret_cast<uintptr_t>(obj);
    assert((addr % alignof(TestStruct)) == 0);

    assert(obj->a == 42);
    assert(obj->b == 0xDEADBEEF);
    assert(obj->c == 0xFF);

    std::printf("  PASS: test_create\n");
}

// ── Test: alloc_array<T>() ──────────────────────────────────

static void test_alloc_array() {
    Arena arena(4096);

    constexpr size_t N = 100;
    auto* arr = arena.alloc_array<uint32_t>(N);
    assert(arr != nullptr);

    // Default-initialized should be zero.
    for (size_t i = 0; i < N; ++i) {
        assert(arr[i] == 0);
    }

    // Write and read back.
    for (size_t i = 0; i < N; ++i) {
        arr[i] = static_cast<uint32_t>(i * 7);
    }
    for (size_t i = 0; i < N; ++i) {
        assert(arr[i] == static_cast<uint32_t>(i * 7));
    }

    // Zero-count array.
    auto* empty = arena.alloc_array<uint32_t>(0);
    assert(empty == nullptr);

    std::printf("  PASS: test_alloc_array\n");
}

// ── Test: reset ─────────────────────────────────────────────

static void test_reset() {
    Arena arena(4096);

    arena.alloc(100);
    arena.alloc(200);
    assert(arena.bytes_used() > 0);

    size_t reserved_before = arena.bytes_reserved();
    arena.reset();

    // After reset, bytes_used should be 0 but reserved stays the same.
    assert(arena.bytes_used() == 0);
    assert(arena.bytes_reserved() == reserved_before);

    // Can allocate again after reset.
    void* p = arena.alloc(64);
    assert(p != nullptr);
    assert(arena.bytes_used() >= 64 && arena.bytes_used() < 128);

    std::printf("  PASS: test_reset\n");
}

// ── Test: many small allocations (stress) ───────────────────

static void test_many_small_allocs() {
    Arena arena(1024);  // Small blocks to force multiple block allocations.

    std::vector<void*> ptrs;
    constexpr size_t N = 10000;
    for (size_t i = 0; i < N; ++i) {
        void* p = arena.alloc(8, 8);
        assert(p != nullptr);
        assert((reinterpret_cast<uintptr_t>(p) % 8) == 0);
        // Write a pattern to detect overlaps later.
        std::memset(p, static_cast<int>(i & 0xFF), 8);
        ptrs.push_back(p);
    }

    // Verify no two allocations overlap: each pointer should be unique
    // and at least 8 bytes apart from the next sequential allocation
    // within the same block.
    for (size_t i = 0; i < N; ++i) {
        auto* data = static_cast<uint8_t*>(ptrs[i]);
        uint8_t expected = static_cast<uint8_t>(i & 0xFF);
        for (int j = 0; j < 8; ++j) {
            assert(data[j] == expected);
        }
    }

    assert(arena.bytes_used() >= N * 8);
    std::printf("  PASS: test_many_small_allocs\n");
}

// ── Test: move semantics ────────────────────────────────────

static void test_move() {
    Arena arena1(4096);
    void* p1 = arena1.alloc(64);
    assert(p1 != nullptr);
    size_t used = arena1.bytes_used();

    // Move construct.
    Arena arena2(std::move(arena1));
    assert(arena2.bytes_used() == used);
    assert(arena1.bytes_used() == 0);  // NOLINT: testing moved-from state

    // Move assign.
    Arena arena3(2048);
    arena3.alloc(128);
    arena3 = std::move(arena2);
    assert(arena3.bytes_used() == used);

    std::printf("  PASS: test_move\n");
}

// ── Test: cache-line alignment (64 bytes) ───────────────────

static void test_cacheline_alignment() {
    Arena arena(4096);

    void* p = arena.alloc(128, 64);
    assert(p != nullptr);
    assert((reinterpret_cast<uintptr_t>(p) % 64) == 0);

    void* p2 = arena.alloc(32, 64);
    assert(p2 != nullptr);
    assert((reinterpret_cast<uintptr_t>(p2) % 64) == 0);

    std::printf("  PASS: test_cacheline_alignment\n");
}

// ── Test: block boundary alignment ────────────────────────────

static void test_block_boundary_alignment() {
    Arena arena(64);  // Tiny block

    // Fill most of the block — 56 bytes used, 8 remaining
    arena.alloc(56, 1);

    // 8 bytes with 64-byte alignment cannot fit after padding
    // in the remaining 8 bytes — must allocate a new block
    void* p = arena.alloc(8, 64);
    assert(p != nullptr);
    assert((reinterpret_cast<uintptr_t>(p) % 64) == 0);
    std::printf("  PASS: test_block_boundary_alignment\n");
}

// ── Test: create with arguments ───────────────────────────────

struct Point {
    int x, y;
    Point(int x_, int y_) : x(x_), y(y_) {}
};

static void test_create_with_args() {
    Arena arena(4096);
    auto* p = arena.create<Point>(3, 7);
    assert(p != nullptr);
    assert(p->x == 3);
    assert(p->y == 7);
    assert((reinterpret_cast<uintptr_t>(p) % alignof(Point)) == 0);
    std::printf("  PASS: test_create_with_args\n");
}

// ── Main ────────────────────────────────────────────────────

void run_arena_tests() {
    std::printf("=== Arena Allocator Tests ===\n");
    std::printf("Running basic_alloc...\n"); test_basic_alloc();
    std::printf("Running alignment...\n"); test_alignment();
    std::printf("Running zero_alloc...\n"); test_zero_alloc();
    std::printf("Running large_alloc...\n"); test_large_alloc();
    std::printf("Running create...\n"); test_create();
    std::printf("Running alloc_array...\n"); test_alloc_array();
    std::printf("Running reset...\n"); test_reset();
    std::printf("Running many_small_allocs...\n"); test_many_small_allocs();
    std::printf("Running move...\n"); test_move();
    std::printf("Running cacheline_alignment...\n"); test_cacheline_alignment();
    std::printf("Running block_boundary_alignment...\n"); test_block_boundary_alignment();
    std::printf("Running create_with_args...\n"); test_create_with_args();
    std::printf("=== All arena tests passed ===\n");
}
