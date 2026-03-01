// test_fst_roundtrip.cpp — Build → serialize → load → verify round-trip

#include "arena.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;

// Helper: build an FST from sorted words, serialize, and load into reader.
static FstReader build_and_load(const std::vector<std::string_view>& words) {
    Arena arena;
    FstBuilder builder(arena);
    for (auto w : words) {
        auto r = builder.add(w);
        assert(r.has_value());
    }
    auto f = builder.finish();
    assert(f.has_value());

    auto bytes = fst_serialize(builder.root(), builder.node_pool());
    auto reader = FstReader::from_bytes(std::move(bytes));
    assert(reader.has_value());
    return std::move(*reader);
}

// ── Test: basic round-trip ───────────────────────────────────

static void test_basic_roundtrip() {
    std::vector<std::string_view> words = {
        "apple", "apply", "bat", "bath", "car", "card"
    };

    auto reader = build_and_load(words);

    // All inserted words must be found.
    for (auto w : words) {
        assert(reader.contains(w));
    }

    // Non-existent words must NOT be found.
    assert(!reader.contains("app"));
    assert(!reader.contains("apples"));
    assert(!reader.contains("ba"));
    assert(!reader.contains("cards"));
    assert(!reader.contains(""));
    assert(!reader.contains("zzz"));

    std::printf("  PASS: test_basic_roundtrip\n");
}

// ── Test: single word round-trip ─────────────────────────────

static void test_single_word_roundtrip() {
    auto reader = build_and_load({"hello"});

    assert(reader.contains("hello"));
    assert(!reader.contains("hell"));
    assert(!reader.contains("helloo"));
    assert(!reader.contains(""));

    std::printf("  PASS: test_single_word_roundtrip\n");
}

// ── Test: empty FST ──────────────────────────────────────────

static void test_empty_roundtrip() {
    auto reader = build_and_load({});

    assert(!reader.contains(""));
    assert(!reader.contains("anything"));

    std::printf("  PASS: test_empty_roundtrip\n");
}

// ── Test: prefix words ───────────────────────────────────────

static void test_prefix_roundtrip() {
    std::vector<std::string_view> words = {
        "a", "ab", "abc", "abcd"
    };

    auto reader = build_and_load(words);

    for (auto w : words) {
        assert(reader.contains(w));
    }
    assert(!reader.contains("abcde"));
    assert(!reader.contains(""));
    assert(!reader.contains("b"));

    std::printf("  PASS: test_prefix_roundtrip\n");
}

// ── Test: many words with shared suffixes ────────────────────

static void test_shared_suffix_roundtrip() {
    std::vector<std::string_view> words = {
        "acting", "bing", "bring", "caring", "ding",
        "fling", "going", "king", "ring", "sing"
    };

    auto reader = build_and_load(words);

    for (auto w : words) {
        assert(reader.contains(w));
    }
    assert(!reader.contains("ing"));
    assert(!reader.contains("thing"));
    assert(!reader.contains("bingo"));

    std::printf("  PASS: test_shared_suffix_roundtrip\n");
}

// ── Test: header fields ──────────────────────────────────────

static void test_header_fields() {
    std::vector<std::string_view> words = {
        "bar", "bard", "bars", "car", "card"
    };

    auto reader = build_and_load(words);

    // num_nodes should be > 0 and reasonable
    assert(reader.num_nodes() > 0);
    assert(reader.num_nodes() <= 20);  // 5 words, can't need more than 20 nodes

    std::printf("  PASS: test_header_fields (num_nodes=%u)\n", reader.num_nodes());
}

// ── Test: larger word set with no false positives ────────────

static void test_no_false_positives() {
    std::vector<std::string_view> words = {
        "act", "bar", "bat", "car", "card", "care", "cared",
        "cares", "cars", "cat", "do", "dog", "done", "dot",
        "ear", "eat", "far", "fat", "hat", "hot", "jar",
        "mat", "oar", "pat", "rat", "sat", "tar", "vat"
    };

    auto reader = build_and_load(words);

    // All words present.
    for (auto w : words) {
        assert(reader.contains(w));
    }

    // Generate random non-words and verify no false positives.
    // Use predictable "random" strings.
    std::vector<std::string> non_words = {
        "ac", "ba", "ca", "card!", "dogs", "do!", "eaten",
        "fart", "hats", "jars", "mats", "oars", "pats",
        "rats", "sats", "tars", "vats", "zzz", "aaa",
        "", "a", "b", "c", "d", "e", "f",
        "carz", "carda", "carex"
    };

    for (const auto& w : non_words) {
        assert(!reader.contains(w));
    }

    std::printf("  PASS: test_no_false_positives\n");
}

// ── Test: file-based mmap round-trip ─────────────────────────

static void test_file_roundtrip() {
    std::vector<std::string_view> words = {
        "alpha", "beta", "delta", "gamma", "omega"
    };

    // Build and serialize.
    Arena arena;
    FstBuilder builder(arena);
    for (auto w : words) {
        auto r = builder.add(w);
        assert(r.has_value());
    }
    builder.finish();
    auto bytes = fst_serialize(builder.root(), builder.node_pool());

    // Write to a temp file.
    const char* tmp_path = "test_roundtrip.fst";
    {
        FILE* fp = fopen(tmp_path, "wb");
        assert(fp);
        size_t written = fwrite(bytes.data(), 1, bytes.size(), fp);
        assert(written == bytes.size());
        fclose(fp);
    }

    // Open via mmap.
    auto reader = FstReader::open(tmp_path);
    assert(reader.has_value());

    for (auto w : words) {
        assert(reader->contains(w));
    }
    assert(!reader->contains("alph"));
    assert(!reader->contains("omegas"));

    // Clean up.
    std::remove(tmp_path);

    std::printf("  PASS: test_file_roundtrip\n");
}

// ── Main ────────────────────────────────────────────────────

void run_fst_roundtrip_tests() {
    std::printf("=== FST Round-trip Tests ===\n");
    std::printf("Running basic_roundtrip...\n"); test_basic_roundtrip();
    std::printf("Running single_word_roundtrip...\n"); test_single_word_roundtrip();
    std::printf("Running empty_roundtrip...\n"); test_empty_roundtrip();
    std::printf("Running prefix_roundtrip...\n"); test_prefix_roundtrip();
    std::printf("Running shared_suffix_roundtrip...\n"); test_shared_suffix_roundtrip();
    std::printf("Running header_fields...\n"); test_header_fields();
    std::printf("Running no_false_positives...\n"); test_no_false_positives();
    std::printf("Running file_roundtrip...\n"); test_file_roundtrip();
    std::printf("=== All FST round-trip tests passed ===\n");
}
