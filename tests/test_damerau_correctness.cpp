// test_damerau_correctness.cpp — Brute-force Damerau-Levenshtein oracle.
//
// Runs fuzzy_search with DistanceMetric::DamerauLevenshtein against a
// brute-force DP oracle and asserts identical result sets.

#include "arena.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "damerau_nfa.h"
#include "damerau_search.h"

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

// ── Brute-force Damerau-Levenshtein distance ──────────────────

static uint32_t damerau_levenshtein(std::string_view a, std::string_view b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<uint32_t>> dp(m + 1, std::vector<uint32_t>(n + 1));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<uint32_t>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<uint32_t>(j);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            uint32_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                  dp[i][j - 1] + 1,
                                  dp[i - 1][j - 1] + cost});
            if (i >= 2 && j >= 2 &&
                a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1);
            }
        }
    }
    return dp[m][n];
}

// ── Brute-force oracle ────────────────────────────────────────

static std::set<std::pair<std::string, uint32_t>>
brute_force_damerau(const std::vector<std::string>& dict,
                    std::string_view query, uint32_t max_dist) {
    std::set<std::pair<std::string, uint32_t>> results;
    for (const auto& word : dict) {
        uint32_t d = damerau_levenshtein(query, word);
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

// ── Run Damerau fuzzy search via DamerauIterator ──────────────

static std::set<std::pair<std::string, uint32_t>>
fst_damerau(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    DamerauNFA dfa;
    auto r = dfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];

    DamerauIterator iter(reader, dfa, word_buf, sizeof(word_buf),
                         result_buf, 8192);

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

// ── Test: small dictionary ────────────────────────────────────

static void test_small_dict_damerau() {
    std::vector<std::string> dict = {
        "the", "teh", "eth", "het", "cat", "act", "tac",
        "friend", "freind", "fiend", "frined",
        "ab", "ba", "a", "b", "abc", "bac", "acb",
        "algorithm", "algortihm", "hello", "hlelo"
    };
    auto reader = build_fst(dict);

    std::vector<std::string_view> queries = {
        "teh", "the", "freind", "ab", "ba", "cat", "act",
        "algorithm", "hello", "abc", ""
    };

    for (auto query : queries) {
        for (uint32_t d = 1; d <= 3; ++d) {
            auto expected = brute_force_damerau(dict, query, d);
            auto got = fst_damerau(reader, query, d);

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

    std::printf("  PASS: test_small_dict_damerau (%zu queries x 3 distances)\n",
                queries.size());
}

// ── Test: real word list oracle ──────────────────────────────

static std::vector<std::string> load_words(const char* path, size_t max_words) {
    std::vector<std::string> words;
    FILE* f = std::fopen(path, "r");
    if (!f) return words;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) && words.size() < max_words) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            --len;
        if (len > 0 && len <= 64) {
            words.emplace_back(line, len);
        }
    }
    std::fclose(f);
    return words;
}

static void test_real_word_list_damerau() {
    auto dict = load_words("data/words.txt", SIZE_MAX);
    if (dict.empty()) {
        std::printf("  SKIP: test_real_word_list_damerau (data/words.txt not found)\n");
        return;
    }

    auto reader = build_fst(dict);

    // Queries chosen to exercise transpositions.
    std::vector<std::string_view> queries = {
        "teh",        // the (transposition)
        "freind",     // friend (transposition)
        "abotu",      // about (transposition)
        "hte",        // the (transposition)
        "algortihm",  // algorithm (transposition)
        "recieve",    // receive (transposition of ie)
        "helo",       // hello (standard edit)
        "aple",       // apple (standard edit)
        "wrld",       // world (standard edit)
        "becuase",    // because (transposition)
    };

    size_t total_checked = 0;

    // All queries at d=1.
    for (auto query : queries) {
        auto expected = brute_force_damerau(dict, query, 1);
        auto got = fst_damerau(reader, query, 1);

        if (got != expected) {
            std::fprintf(stderr,
                "FAIL: real word list damerau: query=\"%.*s\" d=1: "
                "expected %zu results, got %zu\n",
                static_cast<int>(query.size()), query.data(),
                expected.size(), got.size());
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
        total_checked++;
    }

    // First 5 queries at d=2.
    for (size_t qi = 0; qi < 5; ++qi) {
        auto query = queries[qi];
        auto expected = brute_force_damerau(dict, query, 2);
        auto got = fst_damerau(reader, query, 2);

        if (got != expected) {
            std::fprintf(stderr,
                "FAIL: real word list damerau: query=\"%.*s\" d=2: "
                "expected %zu results, got %zu\n",
                static_cast<int>(query.size()), query.data(),
                expected.size(), got.size());
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
        total_checked++;
    }

    std::printf("  PASS: test_real_word_list_damerau (%zu words, %zu query checks)\n",
                dict.size(), total_checked);
}

// ── Main ────────────────────────────────────────────────────

void run_damerau_correctness_tests() {
    std::printf("=== Damerau-Levenshtein Correctness Tests (Oracle) ===\n");
    test_small_dict_damerau();
    test_real_word_list_damerau();
    std::printf("=== All Damerau-Levenshtein correctness tests passed ===\n");
}
