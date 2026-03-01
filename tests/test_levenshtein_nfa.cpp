// test_levenshtein_nfa.cpp — Tests for bit-parallel Levenshtein NFA

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

// Naive O(m*n) Levenshtein distance for reference.
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

// Simulate processing a full word through the NFA and return final distance.
static uint32_t nfa_distance(const LevenshteinNFA& nfa, std::string_view word) {
    LevenshteinState s = nfa.start_state();
    for (char c : word) {
        s = LevenshteinNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                                  nfa.query_len);
    }
    return s.dist;
}

// ── Test: step matches naive DP for single-char transitions ──

static void test_step_vs_naive() {
    // Test 20 query strings × various target strings × distance 1,2,3.
    std::vector<std::string_view> queries = {
        "cat", "hello", "world", "test", "fuzzy",
        "a", "ab", "abc", "abcd", "abcde",
        "zzz", "foo", "bar", "baz", "qux",
        "algorithm", "search", "data", "tree", "graph"
    };

    std::vector<std::string_view> targets = {
        "cat", "bat", "ca", "cats", "ct", "cap",
        "hello", "helo", "hell", "helloo", "jello",
        "world", "word", "wold", "worlds",
        "test", "tset", "tes", "tests", "text",
        "a", "b", "", "ab", "abc",
        "fuzzy", "fuzz", "fuzzi", "fuzy", "buzzy",
        "algorithm", "algorith", "algorithms", "algortihm",
        "search", "sarch", "serch", "seach",
    };

    for (auto query : queries) {
        LevenshteinNFA nfa;
        auto r = nfa.init(query, 3);
        assert(r.has_value());

        for (auto target : targets) {
            uint32_t expected = naive_levenshtein(query, target);
            uint32_t got = nfa_distance(nfa, target);
            assert(got == expected);
        }
    }

    std::printf("  PASS: test_step_vs_naive (%zu queries x %zu targets)\n",
                queries.size(), targets.size());
}

// ── Test: is_match / can_match correctness ───────────────────

static void test_match_predicates() {
    LevenshteinNFA nfa;
    nfa.init("hello", 1);

    // "hello" -> distance 0, should match
    LevenshteinState s = nfa.start_state();
    for (char c : std::string_view("hello")) {
        s = LevenshteinNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                                  nfa.query_len);
    }
    assert(nfa.is_match(s));
    assert(nfa.can_match(s));

    // "helo" -> distance 1, should match at d=1
    s = nfa.start_state();
    for (char c : std::string_view("helo")) {
        s = LevenshteinNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                                  nfa.query_len);
    }
    assert(nfa.is_match(s));

    // "xyz" -> distance 5, should NOT match at d=1
    s = nfa.start_state();
    for (char c : std::string_view("xyz")) {
        s = LevenshteinNFA::step(s, nfa.char_mask[static_cast<uint8_t>(c)],
                                  nfa.query_len);
    }
    assert(!nfa.is_match(s));

    std::printf("  PASS: test_match_predicates\n");
}

// ── Test: query too long ─────────────────────────────────────

static void test_query_too_long() {
    LevenshteinNFA nfa;
    std::string long_query(65, 'a');  // 65 chars, exceeds 64 limit
    auto r = nfa.init(long_query, 1);
    assert(!r.has_value());
    assert(r.error() == Error::QueryTooLong);

    // 64 chars should be OK.
    std::string ok_query(64, 'a');
    auto r2 = nfa.init(ok_query, 1);
    assert(r2.has_value());

    std::printf("  PASS: test_query_too_long\n");
}

// ── Test: empty query ────────────────────────────────────────

static void test_empty_query() {
    LevenshteinNFA nfa;
    nfa.init("", 2);

    // Distance from "" to "" = 0
    assert(nfa_distance(nfa, "") == 0);
    // Distance from "" to "ab" = 2
    assert(nfa_distance(nfa, "ab") == 2);
    // Distance from "" to "abc" = 3
    assert(nfa_distance(nfa, "abc") == 3);

    std::printf("  PASS: test_empty_query\n");
}

// ── Main ────────────────────────────────────────────────────

void run_levenshtein_nfa_tests() {
    std::printf("=== Levenshtein NFA Tests ===\n");
    std::printf("Running step_vs_naive...\n"); test_step_vs_naive();
    std::printf("Running match_predicates...\n"); test_match_predicates();
    std::printf("Running query_too_long...\n"); test_query_too_long();
    std::printf("Running empty_query...\n"); test_empty_query();
    std::printf("=== All Levenshtein NFA tests passed ===\n");
}
