// test_fuzzy_correctness.cpp — THE most important test file in the project.
//
// For a dictionary of words, run fuzzy queries and assert that the set of
// results from fuzzy_search() is IDENTICAL to brute-force Levenshtein
// computation against all dictionary words.

#include "arena.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;

// ── Brute-force Levenshtein distance ─────────────────────────

static uint32_t levenshtein(std::string_view a, std::string_view b) {
    size_t m = a.size(), n = b.size();
    // Use single-row DP for memory efficiency.
    std::vector<uint32_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = static_cast<uint32_t>(j);
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<uint32_t>(i);
        for (size_t j = 1; j <= n; ++j) {
            uint32_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1,
                                 prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

// ── Brute-force oracle: find all words within distance d ─────

static std::set<std::pair<std::string, uint32_t>>
brute_force_fuzzy(const std::vector<std::string>& dict,
                  std::string_view query, uint32_t max_dist) {
    std::set<std::pair<std::string, uint32_t>> results;
    for (const auto& word : dict) {
        uint32_t d = levenshtein(query, word);
        if (d <= max_dist) {
            results.insert({word, d});
        }
    }
    return results;
}

// ── Build FST from dictionary ────────────────────────────────

static FstReader build_fst(std::vector<std::string>& dict) {
    std::sort(dict.begin(), dict.end());
    dict.erase(std::unique(dict.begin(), dict.end()), dict.end());

    Arena arena;
    FstBuilder builder(arena);
    for (const auto& w : dict) {
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

// ── Run fuzzy search via the iterator ────────────────────────

static std::set<std::pair<std::string, uint32_t>>
fst_fuzzy(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[8192];
    FuzzyResult result_buf[4096];

    FuzzyIterator iter(reader, nfa, word_buf, sizeof(word_buf),
                       result_buf, 4096);

    std::set<std::pair<std::string, uint32_t>> results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i) {
            results.insert({std::string(result_buf[i].word),
                            result_buf[i].distance});
        }
    }
    return results;
}

// ── Test: small dictionary, exhaustive queries ───────────────

static void test_small_dict() {
    std::vector<std::string> dict = {
        "cat", "bat", "hat", "car", "card", "care", "cart",
        "bar", "barn", "bare", "bark", "dark", "dart",
        "far", "farm", "fast", "fat", "rat", "sat", "mat"
    };
    auto reader = build_fst(dict);

    std::vector<std::string_view> queries = {
        "cat", "car", "bat", "bar", "far", "hat",
        "cart", "card", "care", "carz", "baz",
        "xyz", "a", "", "cats", "ca"
    };

    for (auto query : queries) {
        for (uint32_t d = 1; d <= 3; ++d) {
            auto expected = brute_force_fuzzy(dict, query, d);
            auto got = fst_fuzzy(reader, query, d);

            if (got != expected) {
                std::fprintf(stderr,
                    "FAIL: query=\"%.*s\" d=%u: expected %zu results, got %zu\n",
                    static_cast<int>(query.size()), query.data(),
                    d, expected.size(), got.size());

                // Print differences.
                for (auto& [w, dist] : expected) {
                    if (got.find({w, dist}) == got.end()) {
                        std::fprintf(stderr, "  MISSING: \"%s\" (d=%u)\n",
                                     w.c_str(), dist);
                    }
                }
                for (auto& [w, dist] : got) {
                    if (expected.find({w, dist}) == expected.end()) {
                        std::fprintf(stderr, "  EXTRA:   \"%s\" (d=%u)\n",
                                     w.c_str(), dist);
                    }
                }
                assert(false);
            }
        }
    }

    std::printf("  PASS: test_small_dict (%zu queries x 3 distances)\n",
                queries.size());
}

// ── Test: medium dictionary with random-ish queries ──────────

static void test_medium_dict() {
    // Build a medium dictionary of ~200 words with common English patterns.
    std::vector<std::string> dict;
    std::vector<std::string> prefixes = {
        "ab", "ac", "ad", "ba", "be", "bi", "bo", "ca", "co", "da",
        "de", "di", "do", "fa", "fi", "fo", "ga", "go", "ha", "he",
        "hi", "ho", "in", "it", "la", "le", "li", "lo", "ma", "me",
        "mi", "mo", "na", "ne", "no", "pa", "pe", "pi", "po", "ra",
        "re", "ri", "ro", "sa", "se", "si", "so", "ta", "te", "ti",
    };
    std::vector<std::string> suffixes = {
        "t", "n", "d", "ng", "nt", "nd", "st", "ck", "ll", "ss",
    };

    for (auto& p : prefixes) {
        for (auto& s : suffixes) {
            dict.push_back(p + s);
        }
    }

    auto reader = build_fst(dict);

    // Query a selection of words that may or may not be in the dict.
    std::vector<std::string_view> queries = {
        "cat", "bat", "ant", "can", "ban", "bad", "big",
        "hit", "hot", "lot", "mat", "not", "sit", "got",
        "zing", "pod", "test", "fox", "luck"
    };

    for (auto query : queries) {
        for (uint32_t d = 1; d <= 2; ++d) {
            auto expected = brute_force_fuzzy(dict, query, d);
            auto got = fst_fuzzy(reader, query, d);
            if (got != expected) {
                std::fprintf(stderr,
                    "FAIL: query=\"%.*s\" d=%u: expected %zu results, got %zu\n",
                    static_cast<int>(query.size()), query.data(),
                    d, expected.size(), got.size());
                for (auto& [w, dist] : expected) {
                    if (got.find({w, dist}) == got.end())
                        std::fprintf(stderr, "  MISSING: \"%s\" (d=%u)\n", w.c_str(), dist);
                }
                for (auto& [w, dist] : got) {
                    if (expected.find({w, dist}) == expected.end())
                        std::fprintf(stderr, "  EXTRA:   \"%s\" (d=%u)\n", w.c_str(), dist);
                }
                assert(false);
            }
        }
    }

    std::printf("  PASS: test_medium_dict (%zu words, %zu queries x 2 distances)\n",
                dict.size(), queries.size());
}

// ── Test: distance 0 = exact match only ──────────────────────

static void test_distance_zero() {
    std::vector<std::string> dict = {"apple", "banana", "cherry"};
    auto reader = build_fst(dict);

    for (const auto& w : dict) {
        auto got = fst_fuzzy(reader, w, 0);
        assert(got.size() == 1);
        assert(got.begin()->first == w);
        assert(got.begin()->second == 0);
    }

    // Non-existent word at distance 0 = empty.
    auto got = fst_fuzzy(reader, "grape", 0);
    assert(got.empty());

    std::printf("  PASS: test_distance_zero\n");
}

// ── Test: empty query returns all words of length <= max_dist ─

static void test_empty_query() {
    std::vector<std::string> dict = {
        "a", "ab", "abc", "b", "ba", "bat", "c", "cat", "xy", "xyz"
    };
    auto reader = build_fst(dict);

    // fuzzy_search("", d=2) should return all words of length 0, 1, 2.
    auto expected = brute_force_fuzzy(dict, "", 2);
    auto got = fst_fuzzy(reader, "", 2);

    // Expected: "a"(d=1), "ab"(d=2), "b"(d=1), "ba"(d=2), "c"(d=1), "xy"(d=2)
    assert(got == expected);
    assert(got.size() == 6);
    // Verify specific entries.
    assert(got.count({"a", 1}) == 1);
    assert(got.count({"ab", 2}) == 1);
    assert(got.count({"b", 1}) == 1);
    // Words of length 3+ should NOT appear.
    assert(got.count({"abc", 3}) == 0);
    assert(got.count({"cat", 3}) == 0);

    // d=0: only the empty string matches, but it's not in our dict.
    auto got0 = fst_fuzzy(reader, "", 0);
    assert(got0.empty());

    std::printf("  PASS: test_empty_query\n");
}

// ── Test: word stability across different DFS depths ─────────

static void test_word_stability() {
    // Build a dict with words at very different depths to verify
    // that results emitted early in the DFS are stable (not corrupted
    // by later path mutations).
    std::vector<std::string> dict = {
        "a",               // depth 1
        "ab",              // depth 2
        "abc",             // depth 3
        "abcdefghij",      // depth 10
        "abcdefghijklmno", // depth 15
        "z",               // depth 1, different subtree
        "zy",              // depth 2
        "zyx",             // depth 3
    };
    auto reader = build_fst(dict);

    // Query "a" at d=1: should find "a"(d=0), "ab"(d=1), "z"(d=1).
    auto expected = brute_force_fuzzy(dict, "a", 1);
    auto got = fst_fuzzy(reader, "a", 1);
    assert(got == expected);

    // Verify each result word is intact (not corrupted by later DFS).
    for (auto& [word, dist] : got) {
        // Each word should be a valid dict entry at the claimed distance.
        uint32_t actual_dist = levenshtein("a", word);
        assert(actual_dist == dist);
    }

    // Query "abcdefghij" at d=2: exercises deep DFS paths.
    auto expected2 = brute_force_fuzzy(dict, "abcdefghij", 2);
    auto got2 = fst_fuzzy(reader, "abcdefghij", 2);
    assert(got2 == expected2);

    std::printf("  PASS: test_word_stability\n");
}

// ── Test: real word list oracle ──────────────────────────────

static std::vector<std::string> load_words(const char* path, size_t max_words) {
    std::vector<std::string> words;
    FILE* f = std::fopen(path, "r");
    if (!f) return words;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) && words.size() < max_words) {
        // Strip trailing newline/carriage return.
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            --len;
        if (len > 0 && len <= 64) {  // Skip empty and too-long words.
            words.emplace_back(line, len);
        }
    }
    std::fclose(f);
    return words;
}

static void test_real_word_list() {
    // Load the FULL word list — no subset.
    auto dict = load_words("data/words.txt", SIZE_MAX);
    if (dict.empty()) {
        std::printf("  SKIP: test_real_word_list (data/words.txt not found)\n");
        return;
    }

    auto reader = build_fst(dict);  // sorts + deduplicates dict

    // 10 real misspellings covering different word structures.
    // Brute force on 370K words at d=2 is O(370K * m) per query,
    // so we limit to 5 queries at d=2 to keep test runtime reasonable.
    std::vector<std::string_view> queries = {
        "helo",       // hello
        "algoritm",   // algorithm
        "aple",       // apple
        "acress",     // actress / across / access
        "adres",      // address
        "agre",       // agree / acre
        "becuase",    // because
        "accidnet",   // accident
        "wrld",       // world
        "abotu",      // about
    };

    size_t total_checked = 0;

    // All queries at d=1.
    for (auto query : queries) {
        auto expected = brute_force_fuzzy(dict, query, 1);
        auto got = fst_fuzzy(reader, query, 1);

        if (got != expected) {
            std::fprintf(stderr,
                "FAIL: real word list: query=\"%.*s\" d=1: "
                "expected %zu results, got %zu\n",
                static_cast<int>(query.size()), query.data(),
                expected.size(), got.size());
            for (auto& [w, dist] : expected) {
                if (got.find({w, dist}) == got.end())
                    std::fprintf(stderr, "  MISSING: \"%s\" (d=%u)\n",
                                 w.c_str(), dist);
            }
            for (auto& [w, dist] : got) {
                if (expected.find({w, dist}) == expected.end())
                    std::fprintf(stderr, "  EXTRA:   \"%s\" (d=%u)\n",
                                 w.c_str(), dist);
            }
            assert(false);
        }
        total_checked++;
    }

    // First 5 queries at d=2.
    for (size_t qi = 0; qi < 5; ++qi) {
        auto query = queries[qi];
        auto expected = brute_force_fuzzy(dict, query, 2);
        auto got = fst_fuzzy(reader, query, 2);

        if (got != expected) {
            std::fprintf(stderr,
                "FAIL: real word list: query=\"%.*s\" d=2: "
                "expected %zu results, got %zu\n",
                static_cast<int>(query.size()), query.data(),
                expected.size(), got.size());
            for (auto& [w, dist] : expected) {
                if (got.find({w, dist}) == got.end())
                    std::fprintf(stderr, "  MISSING: \"%s\" (d=%u)\n",
                                 w.c_str(), dist);
            }
            for (auto& [w, dist] : got) {
                if (expected.find({w, dist}) == expected.end())
                    std::fprintf(stderr, "  EXTRA:   \"%s\" (d=%u)\n",
                                 w.c_str(), dist);
            }
            assert(false);
        }
        total_checked++;
    }

    // Spot-check exact matches across the dictionary.
    for (size_t i = 0; i < dict.size(); i += dict.size() / 100) {
        auto got = fst_fuzzy(reader, dict[i], 0);
        assert(got.size() == 1);
        assert(got.begin()->first == dict[i]);
        assert(got.begin()->second == 0);
    }

    std::printf("  PASS: test_real_word_list (%zu words, %zu query checks, "
                "~100 exact-match spot checks)\n",
                dict.size(), total_checked);
}

// ── Main ────────────────────────────────────────────────────

void run_fuzzy_correctness_tests() {
    std::printf("=== Fuzzy Correctness Tests (Oracle) ===\n");
    std::printf("Running small_dict...\n"); test_small_dict();
    std::printf("Running medium_dict...\n"); test_medium_dict();
    std::printf("Running distance_zero...\n"); test_distance_zero();
    std::printf("Running empty_query...\n"); test_empty_query();
    std::printf("Running word_stability...\n"); test_word_stability();
    std::printf("Running real_word_list...\n"); test_real_word_list();
    std::printf("=== All fuzzy correctness tests passed ===\n");
}
