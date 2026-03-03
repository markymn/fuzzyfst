// test_metric_comparison.cpp — Verify relationship between Levenshtein
// and Damerau-Levenshtein results.
//
// Key property: at any distance d, the Damerau result set is always a
// SUPERSET of the Levenshtein result set (Damerau can only find MORE
// matches, never fewer, because transpositions provide an additional
// edit operation).

#include "arena.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"
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

// ── Build FST ────────────────────────────────────────────────

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

// ── Search helpers ───────────────────────────────────────────

static std::set<std::string>
lev_search(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    FuzzyIterator iter(reader, nfa, word_buf, sizeof(word_buf),
                       result_buf, 8192);

    std::set<std::string> results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i)
            results.insert(std::string(result_buf[i].word));
    }
    return results;
}

static std::set<std::string>
dam_search(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    DamerauNFA dfa;
    auto r = dfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    DamerauIterator iter(reader, dfa, word_buf, sizeof(word_buf),
                         result_buf, 8192);

    std::set<std::string> results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i)
            results.insert(std::string(result_buf[i].word));
    }
    return results;
}

// ── Test: Damerau is superset of Levenshtein ────────────────

static void test_damerau_superset() {
    std::vector<std::string> dict = {
        "the", "teh", "eth", "het", "cat", "act", "tac",
        "friend", "freind", "fiend", "frined",
        "ab", "ba", "a", "b", "abc", "bac", "acb",
        "algorithm", "algortihm", "hello", "hlelo",
        "car", "card", "cart", "care", "bar", "barn"
    };
    auto reader = build_fst(dict);

    std::vector<std::string_view> queries = {
        "teh", "freind", "ab", "cat", "algorithm", "hello",
        "car", "bar", "abc", "a", ""
    };

    size_t extra_count = 0;
    for (auto query : queries) {
        for (uint32_t d = 1; d <= 3; ++d) {
            auto lev = lev_search(reader, query, d);
            auto dam = dam_search(reader, query, d);

            // Every Levenshtein result must also be in Damerau.
            for (const auto& w : lev) {
                if (dam.find(w) == dam.end()) {
                    std::fprintf(stderr,
                        "FAIL: query=\"%.*s\" d=%u: "
                        "Levenshtein found \"%s\" but Damerau did not\n",
                        static_cast<int>(query.size()), query.data(),
                        d, w.c_str());
                    assert(false);
                }
            }

            extra_count += dam.size() - lev.size();
        }
    }

    std::printf("  PASS: test_damerau_superset (%zu queries x 3 distances, "
                "%zu extra Damerau results total)\n",
                queries.size(), extra_count);
}

// ── Test: transpositions show difference at d=1 ──────────────

static void test_transposition_d1_differences() {
    std::vector<std::string> dict = {
        "the", "teh", "ab", "ba", "friend", "freind"
    };
    auto reader = build_fst(dict);

    // "teh" d=1: Levenshtein should NOT find "the" (d=2 by Lev).
    //            Damerau SHOULD find "the" (d=1 by transposition).
    auto lev = lev_search(reader, "teh", 1);
    auto dam = dam_search(reader, "teh", 1);
    assert(lev.find("the") == lev.end());
    assert(dam.find("the") != dam.end());

    // "ab" d=1: same story with "ba".
    lev = lev_search(reader, "ab", 1);
    dam = dam_search(reader, "ab", 1);
    assert(lev.find("ba") == lev.end());
    assert(dam.find("ba") != dam.end());

    // "freind" d=1: Damerau finds "friend".
    lev = lev_search(reader, "freind", 1);
    dam = dam_search(reader, "freind", 1);
    assert(lev.find("friend") == lev.end());
    assert(dam.find("friend") != dam.end());

    std::printf("  PASS: test_transposition_d1_differences\n");
}

// ── Test: at d=0, both metrics agree ─────────────────────────

static void test_d0_agreement() {
    std::vector<std::string> dict = {"apple", "banana", "cherry", "date"};
    auto reader = build_fst(dict);

    for (const auto& w : dict) {
        auto lev = lev_search(reader, w, 0);
        auto dam = dam_search(reader, w, 0);
        assert(lev == dam);
        assert(lev.size() == 1);
    }

    std::printf("  PASS: test_d0_agreement\n");
}

// ── Main ────────────────────────────────────────────────────

void run_metric_comparison_tests() {
    std::printf("=== Metric Comparison Tests ===\n");
    test_damerau_superset();
    test_transposition_d1_differences();
    test_d0_agreement();
    std::printf("=== All metric comparison tests passed ===\n");
}
