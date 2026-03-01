// test_fst_builder.cpp — Tests for FstBuilder (Daciuk's algorithm)

#include "trie_builder.h"
#include "arena.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;

// Helper: collect all words accepted by the trie via DFS.
static void collect_words(const TrieNode* node,
                          const std::vector<TrieNode*>& pool,
                          std::string& buf,
                          std::set<std::string>& out) {
    if (node->is_final) {
        out.insert(buf);
    }
    for (uint8_t i = 0; i < node->num_transitions; ++i) {
        buf.push_back(static_cast<char>(node->transitions[i].label));
        collect_words(pool[node->transitions[i].child_idx], pool, buf, out);
        buf.pop_back();
    }
}

static std::set<std::string> all_words(const FstBuilder& builder) {
    std::set<std::string> result;
    std::string buf;
    collect_words(builder.root(), builder.node_pool(), buf, result);
    return result;
}

// ── Test: single word ────────────────────────────────────────

static void test_single_word() {
    Arena arena;
    FstBuilder builder(arena);

    auto r = builder.add("hello");
    assert(r.has_value());
    auto f = builder.finish();
    assert(f.has_value());

    auto words = all_words(builder);
    assert(words.size() == 1);
    assert(words.count("hello") == 1);

    // "hello" = 6 nodes: root -> h -> e -> l -> l -> o (final)
    // No sharing possible with a single word.
    assert(builder.unique_node_count() == 6);

    std::printf("  PASS: test_single_word\n");
}

// ── Test: two words with shared prefix ───────────────────────

static void test_shared_prefix() {
    Arena arena;
    FstBuilder builder(arena);

    builder.add("abc");
    builder.add("abd");
    builder.finish();

    auto words = all_words(builder);
    assert(words.size() == 2);
    assert(words.count("abc") == 1);
    assert(words.count("abd") == 1);

    // Trie structure:
    //   root -> a -> b -> c (final)
    //                  \-> d (final)
    // Nodes: root, a, b, c, d = 5
    // c and d are both final leaves with 0 transitions, but they're
    // structurally identical (both final, no children) so they dedup to 1.
    // After dedup: root, a, b, leaf = 4 unique nodes
    assert(builder.unique_node_count() == 4);

    std::printf("  PASS: test_shared_prefix\n");
}

// ── Test: shared suffixes get deduped ────────────────────────

static void test_shared_suffix() {
    Arena arena;
    FstBuilder builder(arena);

    // "bing" and "sing" share suffix "ing"
    builder.add("bing");
    builder.add("sing");
    builder.finish();

    auto words = all_words(builder);
    assert(words.size() == 2);
    assert(words.count("bing") == 1);
    assert(words.count("sing") == 1);

    // Without minimization: root -> b -> i -> n -> g (final)
    //                            \-> s -> i -> n -> g (final)
    // = 9 nodes
    // With minimization: 'i -> n -> g' chain is shared, AND the nodes
    // reached by 'b' and 's' are structurally identical (both: not-final,
    // one transition 'i' -> same child), so they dedup too.
    // root, b/s (deduped), i, n, g = 5 unique nodes
    assert(builder.unique_node_count() == 5);

    std::printf("  PASS: test_shared_suffix\n");
}

// ── Test: sorted order enforcement ───────────────────────────

static void test_sorted_order_check() {
    Arena arena;
    FstBuilder builder(arena);

    builder.add("banana");
    auto r = builder.add("apple");  // Out of order!
    assert(!r.has_value());
    assert(r.error() == Error::InputNotSorted);

    std::printf("  PASS: test_sorted_order_check\n");
}

// ── Test: duplicate word rejected ────────────────────────────

static void test_duplicate_rejected() {
    Arena arena;
    FstBuilder builder(arena);

    builder.add("cat");
    auto r = builder.add("cat");  // Duplicate (not strictly greater).
    assert(!r.has_value());
    assert(r.error() == Error::InputNotSorted);

    std::printf("  PASS: test_duplicate_rejected\n");
}

// ── Test: 10 words, hand-verified node count ─────────────────

static void test_ten_words() {
    Arena arena;
    FstBuilder builder(arena);

    // Words chosen to exercise suffix sharing.
    std::vector<std::string_view> words = {
        "bar", "bard", "bars", "car", "card", "cards", "care",
        "cared", "cares", "cars"
    };

    for (auto w : words) {
        auto r = builder.add(w);
        assert(r.has_value());
    }
    builder.finish();

    auto found = all_words(builder);
    assert(found.size() == words.size());
    for (auto w : words) {
        assert(found.count(std::string(w)) == 1);
    }

    // Verify minimization actually reduced node count.
    // An unminimized trie would have many more nodes.
    // The exact count depends on suffix sharing. Let's verify
    // it's significantly less than the sum of all word lengths + 1 (root).
    size_t total_chars = 0;
    for (auto w : words) total_chars += w.size();
    // total_chars = 3+4+4+3+4+5+4+5+5+4 = 41
    // Unminimized would be ~42 nodes (root + one per char).
    // Minimized should be much less.
    assert(builder.unique_node_count() < total_chars);

    std::printf("  PASS: test_ten_words (unique_nodes=%zu)\n",
                builder.unique_node_count());
}

// ── Test: empty input ────────────────────────────────────────

static void test_empty_input() {
    Arena arena;
    FstBuilder builder(arena);
    builder.finish();

    auto words = all_words(builder);
    assert(words.empty());

    // Just the root node.
    assert(builder.unique_node_count() == 1);

    std::printf("  PASS: test_empty_input\n");
}

// ── Test: single-char words ──────────────────────────────────

static void test_single_char_words() {
    Arena arena;
    FstBuilder builder(arena);

    builder.add("a");
    builder.add("b");
    builder.add("c");
    builder.finish();

    auto words = all_words(builder);
    assert(words.size() == 3);

    // root has 3 children: a, b, c — all final leaves.
    // All three leaves are structurally identical (final, no children),
    // so they dedup to 1.
    // Unique: root + 1 leaf = 2
    assert(builder.unique_node_count() == 2);

    std::printf("  PASS: test_single_char_words\n");
}

// ── Test: prefix words (one word is prefix of another) ───────

static void test_prefix_words() {
    Arena arena;
    FstBuilder builder(arena);

    builder.add("car");
    builder.add("card");
    builder.add("care");
    builder.finish();

    auto words = all_words(builder);
    assert(words.size() == 3);
    assert(words.count("car") == 1);
    assert(words.count("card") == 1);
    assert(words.count("care") == 1);

    // root -> c -> a -> r (final, 2 children: d, e)
    //                       -> d (final leaf)
    //                       -> e (final leaf)
    // d and e are identical final leaves → dedup to 1 leaf
    // Unique: root, c, a, r, leaf = 5
    assert(builder.unique_node_count() == 5);

    std::printf("  PASS: test_prefix_words\n");
}

// ── Main ────────────────────────────────────────────────────

void run_fst_builder_tests() {
    std::printf("=== FST Builder Tests ===\n");
    std::printf("Running single_word...\n"); test_single_word();
    std::printf("Running shared_prefix...\n"); test_shared_prefix();
    std::printf("Running shared_suffix...\n"); test_shared_suffix();
    std::printf("Running sorted_order_check...\n"); test_sorted_order_check();
    std::printf("Running duplicate_rejected...\n"); test_duplicate_rejected();
    std::printf("Running ten_words...\n"); test_ten_words();
    std::printf("Running empty_input...\n"); test_empty_input();
    std::printf("Running single_char_words...\n"); test_single_char_words();
    std::printf("Running prefix_words...\n"); test_prefix_words();
    std::printf("=== All FST Builder tests passed ===\n");
}
