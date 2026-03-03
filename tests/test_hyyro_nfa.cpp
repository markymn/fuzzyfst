// test_hyyro_nfa.cpp — Tests for Hyyro's bit-parallel Damerau-Levenshtein NFA

#include "hyyro_nfa.h"
#include "levenshtein_nfa.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;

// Naive O(m*n) Damerau-Levenshtein distance for reference.
static uint32_t naive_damerau(std::string_view a, std::string_view b) {
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

// Naive Levenshtein (no transposition) for comparison.
static uint32_t naive_levenshtein(std::string_view a, std::string_view b) {
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
        }
    }
    return dp[m][n];
}

// Simulate processing a full word through the Hyyro NFA and return final distance.
static uint32_t hyyro_distance(const HyyroNFA& nfa, std::string_view word) {
    HyyroState s = nfa.start_state();
    for (char c : word) {
        s = HyyroNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                            nfa.query_len);
    }
    return s.dist;
}

// Simulate processing a full word through the Myers NFA and return final distance.
static uint32_t myers_distance(const LevenshteinNFA& nfa, std::string_view word) {
    LevenshteinState s = nfa.start_state();
    for (char c : word) {
        s = LevenshteinNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                                  nfa.query_len);
    }
    return s.dist;
}

// ── Test: hand-computed transposition cases ──

static void test_transposition_cases() {
    // These are the required cases from the spec:
    // "ab" -> "ba": Hyyro d=1, Myers' d=2
    {
        HyyroNFA nfa;
        nfa.init("ab", 3);
        assert(hyyro_distance(nfa, "ba") == 1);

        LevenshteinNFA lnfa;
        lnfa.init("ab", 3);
        assert(myers_distance(lnfa, "ba") == 2);
    }

    // "teh" -> "the": Hyyro d=1, Myers' d=2
    {
        HyyroNFA nfa;
        nfa.init("teh", 3);
        assert(hyyro_distance(nfa, "the") == 1);

        LevenshteinNFA lnfa;
        lnfa.init("teh", 3);
        assert(myers_distance(lnfa, "the") == 2);
    }

    // "freind" -> "friend": Hyyro d=1, Myers' d=2
    {
        HyyroNFA nfa;
        nfa.init("freind", 3);
        assert(hyyro_distance(nfa, "friend") == 1);

        LevenshteinNFA lnfa;
        lnfa.init("freind", 3);
        assert(myers_distance(lnfa, "friend") == 2);
    }

    // "wierd" -> "weird": Hyyro d=1, Myers' d=2
    {
        HyyroNFA nfa;
        nfa.init("wierd", 3);
        assert(hyyro_distance(nfa, "weird") == 1);

        LevenshteinNFA lnfa;
        lnfa.init("wierd", 3);
        assert(myers_distance(lnfa, "weird") == 2);
    }

    // "recieve" -> "receive": Hyyro d=1, Myers' d=2
    {
        HyyroNFA nfa;
        nfa.init("recieve", 3);
        assert(hyyro_distance(nfa, "receive") == 1);

        LevenshteinNFA lnfa;
        lnfa.init("recieve", 3);
        assert(myers_distance(lnfa, "receive") == 2);
    }

    std::printf("  PASS: test_transposition_cases\n");
}

// ── Test: non-transposition cases give identical results to Myers' ──

static void test_non_transposition_agreement() {
    std::vector<std::string_view> queries = {
        "cat", "hello", "world", "test", "fuzzy",
        "a", "ab", "abc", "abcd", "abcde",
    };

    // Targets that do NOT involve transpositions relative to the queries.
    std::vector<std::string_view> targets = {
        "cat", "bat", "ca", "cats", "ct", "cap",
        "hello", "helo", "hell", "helloo", "jello",
        "world", "word", "wold", "worlds",
        "test", "tes", "tests", "text",
        "a", "b", "", "ab", "abc",
        "fuzzy", "fuzz", "fuzzi", "fuzy", "buzzy",
    };

    for (auto query : queries) {
        HyyroNFA hnfa;
        hnfa.init(query, 3);

        LevenshteinNFA lnfa;
        lnfa.init(query, 3);

        for (auto target : targets) {
            uint32_t h_dist = hyyro_distance(hnfa, target);
            uint32_t m_dist = myers_distance(lnfa, target);
            uint32_t true_dam = naive_damerau(query, target);
            uint32_t true_lev = naive_levenshtein(query, target);

            // For non-transposition cases, Damerau == Levenshtein,
            // so Hyyro should equal Myers.
            if (true_dam == true_lev) {
                if (h_dist != m_dist) {
                    std::fprintf(stderr,
                        "FAIL: query=\"%.*s\" target=\"%.*s\" "
                        "hyyro=%u myers=%u true_dam=%u true_lev=%u\n",
                        (int)query.size(), query.data(),
                        (int)target.size(), target.data(),
                        h_dist, m_dist, true_dam, true_lev);
                    assert(false);
                }
            }

            // Hyyro distance should always match naive Damerau.
            if (h_dist != true_dam) {
                std::fprintf(stderr,
                    "FAIL: query=\"%.*s\" target=\"%.*s\" "
                    "hyyro=%u true_damerau=%u\n",
                    (int)query.size(), query.data(),
                    (int)target.size(), target.data(),
                    h_dist, true_dam);
                assert(false);
            }
        }
    }

    std::printf("  PASS: test_non_transposition_agreement (%zu queries x %zu targets)\n",
                queries.size(), targets.size());
}

// ── Test: step matches naive Damerau DP ──

static void test_step_vs_naive() {
    std::vector<std::string_view> queries = {
        "cat", "hello", "ab", "teh", "freind",
        "algorithm", "test", "fuzzy", "abc", "world"
    };

    std::vector<std::string_view> targets = {
        "cat", "act", "tac", "ca", "cats", "bat",
        "hello", "hlelo", "helo", "ehllo", "jello",
        "ab", "ba", "a", "abc", "b", "",
        "the", "teh", "eth", "het",
        "friend", "freind", "fiend", "frined",
        "algorithm", "algortihm", "algorith", "algorithms",
        "test", "tset", "tes", "text", "tests",
        "fuzzy", "fzuzy", "fuzyz", "fuzz", "buzzy",
        "abc", "bac", "acb", "cab", "bca",
        "world", "wlord", "wrold", "worl", "worlds"
    };

    for (auto query : queries) {
        HyyroNFA nfa;
        auto r = nfa.init(query, 3);
        assert(r.has_value());

        for (auto target : targets) {
            uint32_t expected = naive_damerau(query, target);
            uint32_t got = hyyro_distance(nfa, target);

            if (got != expected) {
                std::fprintf(stderr,
                    "FAIL: query=\"%.*s\" target=\"%.*s\" expected=%u got=%u\n",
                    (int)query.size(), query.data(),
                    (int)target.size(), target.data(),
                    expected, got);
                assert(false);
            }
        }
    }

    std::printf("  PASS: test_step_vs_naive (%zu queries x %zu targets)\n",
                queries.size(), targets.size());
}

// ── Test: is_match / can_match correctness ──

static void test_match_predicates() {
    HyyroNFA nfa;
    nfa.init("hello", 1);

    // "hello" -> distance 0, should match
    HyyroState s = nfa.start_state();
    for (char c : std::string_view("hello")) {
        s = HyyroNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                            nfa.query_len);
    }
    assert(nfa.is_match(s));
    assert(nfa.can_match(s));

    // "helo" -> distance 1, should match at d=1
    s = nfa.start_state();
    for (char c : std::string_view("helo")) {
        s = HyyroNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                            nfa.query_len);
    }
    assert(nfa.is_match(s));

    // "xyz" -> distance 5, should NOT match at d=1
    s = nfa.start_state();
    for (char c : std::string_view("xyz")) {
        s = HyyroNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                            nfa.query_len);
    }
    assert(!nfa.is_match(s));

    std::printf("  PASS: test_match_predicates\n");
}

// ── Test: query too long ──

static void test_query_too_long() {
    HyyroNFA nfa;
    std::string long_query(65, 'a');
    auto r = nfa.init(long_query, 1);
    assert(!r.has_value());
    assert(r.error() == Error::QueryTooLong);

    std::string ok_query(64, 'a');
    auto r2 = nfa.init(ok_query, 1);
    assert(r2.has_value());

    std::printf("  PASS: test_query_too_long\n");
}

// ── Test: empty query ──

static void test_empty_query() {
    HyyroNFA nfa;
    nfa.init("", 2);

    // Distance from "" to "" = 0
    assert(hyyro_distance(nfa, "") == 0);
    // Distance from "" to "ab" = 2
    assert(hyyro_distance(nfa, "ab") == 2);
    // Distance from "" to "abc" = 3
    assert(hyyro_distance(nfa, "abc") == 3);

    std::printf("  PASS: test_empty_query\n");
}

// ── Main ──

void run_hyyro_nfa_tests() {
    std::printf("=== Hyyro NFA Tests ===\n");
    test_transposition_cases();
    test_non_transposition_agreement();
    test_step_vs_naive();
    test_match_predicates();
    test_query_too_long();
    test_empty_query();
    std::printf("=== All Hyyro NFA tests passed ===\n");
}
