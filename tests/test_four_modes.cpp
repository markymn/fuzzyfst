// test_four_modes.cpp — Verify that the three working search modes produce
// consistent results.
//
// Key properties:
//   1. Levenshtein BitParallel and Levenshtein DFA return identical result sets.
//   2. At d>=2, Damerau DFA results are a superset of Levenshtein results.
//   3. At d=1, Damerau DFA returns strictly more results for transposition queries.
//
// (Hyyro / Damerau BitParallel is disabled until the implementation is fixed.)

#include "arena.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"
#include "levenshtein_dfa.h"
#include "levenshtein_dfa_search.h"
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

// ── Build FST ──

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

// ── Search helpers ──

using ResultSet = std::set<std::pair<std::string, uint32_t>>;

static ResultSet lev_bp_search(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    FuzzyIterator iter(reader, nfa, word_buf, sizeof(word_buf), result_buf, 8192);

    ResultSet results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i)
            results.insert({std::string(result_buf[i].word), result_buf[i].distance});
    }
    return results;
}

static ResultSet lev_dfa_search(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    LevenshteinDFA dfa;
    auto r = dfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    LevenshteinDFAIterator iter(reader, dfa, word_buf, sizeof(word_buf), result_buf, 8192);

    ResultSet results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i)
            results.insert({std::string(result_buf[i].word), result_buf[i].distance});
    }
    return results;
}

static ResultSet dam_dfa_search(const FstReader& reader, std::string_view query, uint32_t max_dist) {
    DamerauNFA dfa;
    auto r = dfa.init(query, max_dist);
    if (!r.has_value()) return {};

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    DamerauIterator iter(reader, dfa, word_buf, sizeof(word_buf), result_buf, 8192);

    ResultSet results;
    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i)
            results.insert({std::string(result_buf[i].word), result_buf[i].distance});
    }
    return results;
}

// ── Test: Levenshtein BitParallel == Levenshtein DFA ──

static void test_lev_modes_agree() {
    std::vector<std::string> dict = {
        "the", "teh", "eth", "het", "cat", "act", "tac",
        "friend", "freind", "fiend", "frined",
        "ab", "ba", "a", "b", "abc", "bac", "acb",
        "algorithm", "algortihm", "hello", "hlelo",
        "car", "card", "cart", "care", "bar", "barn",
        "world", "word", "wold", "words"
    };
    auto reader = build_fst(dict);

    std::vector<std::string_view> queries = {
        "teh", "freind", "ab", "cat", "algorithm", "hello",
        "car", "bar", "abc", "a", "", "world", "xyz"
    };

    for (auto query : queries) {
        for (uint32_t d = 0; d <= 3; ++d) {
            auto bp = lev_bp_search(reader, query, d);
            auto dfa = lev_dfa_search(reader, query, d);

            if (bp != dfa) {
                std::fprintf(stderr,
                    "FAIL: Lev BP vs DFA disagree: query=\"%.*s\" d=%u: "
                    "BP=%zu results, DFA=%zu results\n",
                    static_cast<int>(query.size()), query.data(),
                    d, bp.size(), dfa.size());
                for (auto& [w, dist] : bp) {
                    if (dfa.find({w, dist}) == dfa.end())
                        std::fprintf(stderr, "  BP only:  \"%s\" (d=%u)\n", w.c_str(), dist);
                }
                for (auto& [w, dist] : dfa) {
                    if (bp.find({w, dist}) == bp.end())
                        std::fprintf(stderr, "  DFA only: \"%s\" (d=%u)\n", w.c_str(), dist);
                }
                assert(false);
            }
        }
    }

    std::printf("  PASS: test_lev_modes_agree (%zu queries x 4 distances)\n",
                queries.size());
}

// ── Test: Damerau DFA superset of Levenshtein at d>=2 ──

static void test_damerau_superset_d2() {
    std::vector<std::string> dict = {
        "the", "teh", "eth", "het", "cat", "act", "tac",
        "friend", "freind", "fiend", "frined",
        "ab", "ba", "a", "b", "abc", "bac", "acb",
        "algorithm", "algortihm", "hello", "hlelo",
        "car", "card", "cart", "care"
    };
    auto reader = build_fst(dict);

    std::vector<std::string_view> queries = {
        "teh", "freind", "ab", "cat", "algorithm", "hello"
    };

    for (auto query : queries) {
        for (uint32_t d = 2; d <= 3; ++d) {
            auto lev = lev_bp_search(reader, query, d);
            auto dam = dam_dfa_search(reader, query, d);

            // Every Levenshtein result word must also appear in Damerau.
            for (auto& [w, dist] : lev) {
                bool found = false;
                for (auto& [dw, dd] : dam) {
                    if (dw == w) { found = true; break; }
                }
                if (!found) {
                    std::fprintf(stderr,
                        "FAIL: query=\"%.*s\" d=%u: "
                        "Lev found \"%s\" but Damerau did not\n",
                        static_cast<int>(query.size()), query.data(),
                        d, w.c_str());
                    assert(false);
                }
            }
        }
    }

    std::printf("  PASS: test_damerau_superset_d2\n");
}

// ── Test: at d=1, Damerau DFA returns strictly more for transposition queries ──

static void test_transposition_d1_extra() {
    std::vector<std::string> dict = {
        "the", "teh", "ab", "ba", "friend", "freind",
        "weird", "wierd", "receive", "recieve"
    };
    auto reader = build_fst(dict);

    // "teh" d=1: Damerau finds "the" (transposition), Levenshtein does not.
    auto lev = lev_bp_search(reader, "teh", 1);
    auto dam = dam_dfa_search(reader, "teh", 1);
    assert(lev.find({"the", 1}) == lev.end());  // Lev: d("teh","the")=2
    assert(dam.find({"the", 1}) != dam.end());   // Dam: d("teh","the")=1
    assert(dam.size() > lev.size());

    // "ab" d=1: Damerau finds "ba".
    lev = lev_bp_search(reader, "ab", 1);
    dam = dam_dfa_search(reader, "ab", 1);
    assert(lev.find({"ba", 1}) == lev.end());
    assert(dam.find({"ba", 1}) != dam.end());
    assert(dam.size() > lev.size());

    // "freind" d=1: Damerau finds "friend".
    lev = lev_bp_search(reader, "freind", 1);
    dam = dam_dfa_search(reader, "freind", 1);
    assert(lev.find({"friend", 1}) == lev.end());
    assert(dam.find({"friend", 1}) != dam.end());
    assert(dam.size() > lev.size());

    std::printf("  PASS: test_transposition_d1_extra\n");
}

// ── Main ──

void run_four_modes_tests() {
    std::printf("=== Three Modes Tests (Hyyro disabled) ===\n");
    test_lev_modes_agree();
    test_damerau_superset_d2();
    test_transposition_d1_extra();
    std::printf("=== All three modes tests passed ===\n");
}
