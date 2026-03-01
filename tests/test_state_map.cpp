// test_state_map.cpp — Tests for StateMap (Robin Hood hash map)

#include "state_map.h"
#include "arena.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace fuzzyfst::internal;

// Helper: allocate a TrieNode with given transitions in the arena.
// Assigns node->id = id and sets hash_cache = 0.
static TrieNode* make_node(Arena& arena,
                            std::vector<TrieNode*>& pool,
                            bool is_final,
                            const std::vector<std::pair<uint8_t, uint32_t>>& trans) {
    auto* node = arena.create<TrieNode>();
    node->is_final = is_final;
    node->num_transitions = static_cast<uint8_t>(trans.size());
    node->hash_cache = 0;
    node->id = static_cast<uint32_t>(pool.size());

    if (!trans.empty()) {
        node->transitions = arena.alloc_array<TrieNode::Trans>(trans.size());
        for (size_t i = 0; i < trans.size(); ++i) {
            node->transitions[i].label = trans[i].first;
            node->transitions[i].child_idx = trans[i].second;
        }
    } else {
        node->transitions = nullptr;
    }

    pool.push_back(node);
    return node;
}

// ── Test: basic insert and dedup ─────────────────────────────

static void test_basic_insert_dedup() {
    Arena arena(4096);
    std::vector<TrieNode*> pool;

    // Node 0: leaf, final
    make_node(arena, pool, true, {});

    // Node 1: leaf, final — structurally identical to node 0
    make_node(arena, pool, true, {});

    // Node 2: leaf, NOT final — different from node 0
    make_node(arena, pool, false, {});

    StateMap map(16);

    // Insert node 0 — first insert, should return its own ID.
    uint32_t id0 = map.find_or_insert(pool[0], pool.data());
    assert(id0 == 0);
    assert(map.size() == 1);

    // Insert node 1 — structurally equal to node 0, should dedup.
    uint32_t id1 = map.find_or_insert(pool[1], pool.data());
    assert(id1 == 0);  // Returns node 0's ID.
    assert(map.size() == 1);  // No new entry.

    // Insert node 2 — different (not final), should get new entry.
    uint32_t id2 = map.find_or_insert(pool[2], pool.data());
    assert(id2 == 2);
    assert(map.size() == 2);

    std::printf("  PASS: test_basic_insert_dedup\n");
}

// ── Test: transitions matter for equality ────────────────────

static void test_transitions_differ() {
    Arena arena(4096);
    std::vector<TrieNode*> pool;

    // Node 0: leaf (target for transitions)
    make_node(arena, pool, false, {});

    // Node 1: one transition 'a' -> node 0
    make_node(arena, pool, false, {{'a', 0}});

    // Node 2: one transition 'b' -> node 0 (different label)
    make_node(arena, pool, false, {{'b', 0}});

    // Node 3: one transition 'a' -> node 0 (identical to node 1)
    make_node(arena, pool, false, {{'a', 0}});

    StateMap map(16);

    uint32_t r0 = map.find_or_insert(pool[0], pool.data());
    assert(r0 == 0);

    uint32_t r1 = map.find_or_insert(pool[1], pool.data());
    assert(r1 == 1);
    assert(map.size() == 2);

    uint32_t r2 = map.find_or_insert(pool[2], pool.data());
    assert(r2 == 2);  // Different label, new entry.
    assert(map.size() == 3);

    uint32_t r3 = map.find_or_insert(pool[3], pool.data());
    assert(r3 == 1);  // Dedup with node 1.
    assert(map.size() == 3);

    std::printf("  PASS: test_transitions_differ\n");
}

// ── Test: child IDs matter (not just labels) ─────────────────

static void test_child_ids_matter() {
    Arena arena(4096);
    std::vector<TrieNode*> pool;

    // Node 0 and node 1: two distinct leaves
    make_node(arena, pool, true, {});
    make_node(arena, pool, false, {});

    // Node 2: 'a' -> node 0
    make_node(arena, pool, false, {{'a', 0}});

    // Node 3: 'a' -> node 1 (same label, different child)
    make_node(arena, pool, false, {{'a', 1}});

    StateMap map(16);
    map.find_or_insert(pool[0], pool.data());
    map.find_or_insert(pool[1], pool.data());

    uint32_t r2 = map.find_or_insert(pool[2], pool.data());
    assert(r2 == 2);

    uint32_t r3 = map.find_or_insert(pool[3], pool.data());
    assert(r3 == 3);  // Different child ID, must NOT dedup.
    assert(map.size() == 4);

    std::printf("  PASS: test_child_ids_matter\n");
}

// ── Test: grow (many inserts trigger rehash) ─────────────────

static void test_grow() {
    Arena arena(1 << 20);
    std::vector<TrieNode*> pool;

    // Create 1000 structurally unique nodes: each has a different
    // single transition label (wrapping) and child pointing to itself.
    // We vary both label and is_final to ensure uniqueness.
    for (uint32_t i = 0; i < 1000; ++i) {
        uint8_t label = static_cast<uint8_t>(i & 0xFF);
        bool fin = (i >= 256);  // First 256 not final, rest final
        uint32_t child = i;     // Self-referencing (unique child per node)
        make_node(arena, pool, fin, {{label, child}});
    }

    StateMap map(16);  // Start very small to force many grows.

    for (uint32_t i = 0; i < 1000; ++i) {
        uint32_t r = map.find_or_insert(pool[i], pool.data());
        assert(r == i);  // Each should be unique.
    }
    assert(map.size() == 1000);

    // Re-insert all — every one should dedup.
    for (uint32_t i = 0; i < 1000; ++i) {
        uint32_t r = map.find_or_insert(pool[i], pool.data());
        assert(r == i);
    }
    assert(map.size() == 1000);

    std::printf("  PASS: test_grow\n");
}

// ── Test: multiple transitions per node ──────────────────────

static void test_multi_trans() {
    Arena arena(4096);
    std::vector<TrieNode*> pool;

    // Leaf nodes
    make_node(arena, pool, true, {});   // 0
    make_node(arena, pool, false, {});  // 1

    // Node 2: transitions a->0, b->1
    make_node(arena, pool, false, {{'a', 0}, {'b', 1}});

    // Node 3: transitions a->0, b->1 (identical to node 2)
    make_node(arena, pool, false, {{'a', 0}, {'b', 1}});

    // Node 4: transitions a->1, b->0 (same labels, swapped children)
    make_node(arena, pool, false, {{'a', 1}, {'b', 0}});

    // Node 5: transitions a->0, c->1 (different second label)
    make_node(arena, pool, false, {{'a', 0}, {'c', 1}});

    StateMap map(16);
    map.find_or_insert(pool[0], pool.data());
    map.find_or_insert(pool[1], pool.data());

    uint32_t r2 = map.find_or_insert(pool[2], pool.data());
    assert(r2 == 2);

    uint32_t r3 = map.find_or_insert(pool[3], pool.data());
    assert(r3 == 2);  // Dedup with node 2.

    uint32_t r4 = map.find_or_insert(pool[4], pool.data());
    assert(r4 == 4);  // Different — children are swapped.

    uint32_t r5 = map.find_or_insert(pool[5], pool.data());
    assert(r5 == 5);  // Different — label 'c' vs 'b'.

    assert(map.size() == 5);  // 0, 1, 2, 4, 5

    std::printf("  PASS: test_multi_trans\n");
}

// ── Test: dedup count matches hand-computed value ────────────

static void test_dedup_count() {
    Arena arena(4096);
    std::vector<TrieNode*> pool;

    // Build a small trie-like structure:
    // 5 unique structural signatures, 3 duplicates = 8 nodes total.
    // Unique: final-leaf, non-final-leaf, a->0-final, a->0-nonfinal, ab->0,1-nonfinal
    make_node(arena, pool, true, {});           // 0: final leaf
    make_node(arena, pool, false, {});          // 1: non-final leaf
    make_node(arena, pool, true, {});           // 2: dup of 0
    make_node(arena, pool, false, {{'a', 0}});  // 3: a->0 non-final
    make_node(arena, pool, true, {{'a', 0}});   // 4: a->0 final
    make_node(arena, pool, false, {{'a', 0}});  // 5: dup of 3
    make_node(arena, pool, false, {{'a', 0}, {'b', 1}});  // 6: unique
    make_node(arena, pool, true, {{'a', 0}});   // 7: dup of 4

    StateMap map(16);
    for (auto* node : pool) {
        map.find_or_insert(node, pool.data());
    }

    // 5 unique signatures: 0, 1, 3, 4, 6
    assert(map.size() == 5);

    std::printf("  PASS: test_dedup_count\n");
}

// ── Main ────────────────────────────────────────────────────

void run_state_map_tests() {
    std::printf("=== StateMap Tests ===\n");
    std::printf("Running basic_insert_dedup...\n"); test_basic_insert_dedup();
    std::printf("Running transitions_differ...\n"); test_transitions_differ();
    std::printf("Running child_ids_matter...\n"); test_child_ids_matter();
    std::printf("Running grow...\n"); test_grow();
    std::printf("Running multi_trans...\n"); test_multi_trans();
    std::printf("Running dedup_count...\n"); test_dedup_count();
    std::printf("=== All StateMap tests passed ===\n");
}
