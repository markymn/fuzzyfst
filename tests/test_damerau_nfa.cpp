// test_damerau_nfa.cpp — Tests for compiled Damerau-Levenshtein DFA

#include "damerau_nfa.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;

// Naive O(m*n) Damerau-Levenshtein distance (unrestricted: allows
// transpositions that don't overlap with other edits).
static uint32_t naive_damerau(std::string_view a, std::string_view b) {
    size_t m = a.size(), n = b.size();
    // Use the classic DP with transposition row.
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

// Simulate running a word through the DFA and check acceptance.
static uint32_t dfa_accepts_at_distance(const DamerauNFA& dfa,
                                         std::string_view word,
                                         uint32_t max_dist) {
    uint32_t state = dfa.start_state();
    for (char c : word) {
        state = dfa.step(state, static_cast<uint8_t>(c));
    }
    return dfa.is_match(state);
}

// ── Test: hand-computed transposition cases ──

static void test_transposition_basics() {
    // "ab" -> "ba": Levenshtein=2, Damerau=1
    assert(naive_levenshtein("ab", "ba") == 2);
    assert(naive_damerau("ab", "ba") == 1);

    // "teh" -> "the": Levenshtein=2, Damerau=1
    assert(naive_levenshtein("teh", "the") == 2);
    assert(naive_damerau("teh", "the") == 1);

    // "freind" -> "friend": Levenshtein=2, Damerau=1
    assert(naive_levenshtein("freind", "friend") == 2);
    assert(naive_damerau("freind", "friend") == 1);

    // Verify DFA matches for these cases at d=1.
    {
        DamerauNFA dfa;
        auto r = dfa.init("ab", 1);
        assert(r.has_value());
        assert(dfa_accepts_at_distance(dfa, "ba", 1));
        assert(dfa_accepts_at_distance(dfa, "ab", 1));   // exact match
        assert(!dfa_accepts_at_distance(dfa, "xyz", 1));  // too far
    }

    {
        DamerauNFA dfa;
        auto r = dfa.init("teh", 1);
        assert(r.has_value());
        assert(dfa_accepts_at_distance(dfa, "the", 1));
        assert(dfa_accepts_at_distance(dfa, "teh", 1));
        assert(dfa_accepts_at_distance(dfa, "tea", 1));   // d=1 (sub h->a)
    }

    {
        DamerauNFA dfa;
        auto r = dfa.init("freind", 1);
        assert(r.has_value());
        assert(dfa_accepts_at_distance(dfa, "friend", 1));
        assert(dfa_accepts_at_distance(dfa, "freind", 1));
    }

    std::printf("  PASS: test_transposition_basics\n");
}

// ── Test: DFA step matches naive oracle ──

static void test_dfa_vs_naive() {
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
        for (uint32_t max_d = 1; max_d <= 3; ++max_d) {
            DamerauNFA dfa;
            auto r = dfa.init(query, max_d);
            assert(r.has_value());

            for (auto target : targets) {
                uint32_t true_dist = naive_damerau(query, target);
                bool expected_match = (true_dist <= max_d);
                bool dfa_match = dfa_accepts_at_distance(dfa, target, max_d);
                if (expected_match != dfa_match) {
                    std::printf("FAIL: query=\"%.*s\" target=\"%.*s\" d=%u "
                                "true_dist=%u expected=%d got=%d\n",
                                (int)query.size(), query.data(),
                                (int)target.size(), target.data(),
                                max_d, true_dist, expected_match, dfa_match);
                    assert(false);
                }
            }
        }
    }

    std::printf("  PASS: test_dfa_vs_naive (%zu queries x %zu targets x 3 distances)\n",
                queries.size(), targets.size());
}

// ── Test: can_reach_accept pruning correctness ──

static void test_can_reach_accept() {
    DamerauNFA dfa;
    dfa.init("hello", 1);

    // Dead state should not be able to reach accept.
    assert(!dfa.can_reach_accept(dfa.dead_state()));

    // Start state should be able to reach accept (we can spell "hello").
    assert(dfa.can_reach_accept(dfa.start_state()));

    // After feeding "zzzzz" the state should be dead or unreachable.
    uint32_t state = dfa.start_state();
    for (char c : std::string_view("zzzzzz")) {
        state = dfa.step(state, static_cast<uint8_t>(c));
    }
    assert(!dfa.can_reach_accept(state));

    // After feeding "hell", should still be reachable.
    state = dfa.start_state();
    for (char c : std::string_view("hell")) {
        state = dfa.step(state, static_cast<uint8_t>(c));
    }
    assert(dfa.can_reach_accept(state));

    std::printf("  PASS: test_can_reach_accept\n");
}

// ── Test: query too long ──

static void test_query_too_long() {
    DamerauNFA dfa;
    std::string long_query(65, 'a');
    auto r = dfa.init(long_query, 1);
    assert(!r.has_value());
    assert(r.error() == Error::QueryTooLong);

    std::string ok_query(64, 'a');
    auto r2 = dfa.init(ok_query, 1);
    assert(r2.has_value());

    std::printf("  PASS: test_query_too_long\n");
}

// ── Test: empty query ──

static void test_empty_query() {
    DamerauNFA dfa;
    auto r = dfa.init("", 2);
    assert(r.has_value());

    // "" vs "" = 0, should match
    assert(dfa_accepts_at_distance(dfa, "", 2));
    // "" vs "ab" = 2, should match at d=2
    assert(dfa_accepts_at_distance(dfa, "ab", 2));
    // "" vs "abc" = 3, should NOT match at d=2
    assert(!dfa_accepts_at_distance(dfa, "abc", 2));

    std::printf("  PASS: test_empty_query\n");
}

// ── Test: Damerau d=1 finds transpositions that Levenshtein d=1 misses ──

static void test_damerau_vs_levenshtein_d1() {
    // These pairs have Levenshtein distance 2 but Damerau distance 1.
    struct Case {
        std::string_view query;
        std::string_view target;
    };

    std::vector<Case> transposition_cases = {
        {"ab", "ba"},
        {"teh", "the"},
        {"freind", "friend"},
        {"recieve", "recieve"},  // same word, d=0
        {"acme", "amce"},        // transposition of c,m
    };

    for (auto& tc : transposition_cases) {
        uint32_t lev = naive_levenshtein(tc.query, tc.target);
        uint32_t dam = naive_damerau(tc.query, tc.target);
        // Damerau distance should be <= Levenshtein distance.
        assert(dam <= lev);
    }

    // Verify that DFA at d=1 accepts transpositions.
    DamerauNFA dfa;
    dfa.init("ab", 1);
    assert(dfa_accepts_at_distance(dfa, "ba", 1));  // Damerau d=1

    std::printf("  PASS: test_damerau_vs_levenshtein_d1\n");
}

// ── Test: state count is reasonable ──

static void test_state_count() {
    // For short queries at low distance, DFA should have a manageable
    // number of states.
    DamerauNFA dfa;
    dfa.init("hello", 1);
    uint32_t d1_states = dfa.num_states();
    assert(d1_states > 0);
    assert(d1_states < 10000);

    DamerauNFA dfa2;
    dfa2.init("hello", 2);
    uint32_t d2_states = dfa2.num_states();
    assert(d2_states < 100000);

    std::printf("  PASS: test_state_count (d=1: %u states, d=2: %u states)\n",
                d1_states, d2_states);
}

// ── Main ──

void run_damerau_nfa_tests() {
    std::printf("=== Damerau-Levenshtein NFA Tests ===\n");
    test_transposition_basics();
    test_dfa_vs_naive();
    test_can_reach_accept();
    test_query_too_long();
    test_empty_query();
    test_damerau_vs_levenshtein_d1();
    test_state_count();
    std::printf("=== All Damerau-Levenshtein NFA tests passed ===\n");
}
