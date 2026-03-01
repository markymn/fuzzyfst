// test_main.cpp — Unified test runner

#include <cstdio>

// Defined in individual test files
void run_arena_tests();
void run_radix_sort_tests();
void run_state_map_tests();
void run_fst_builder_tests();
void run_fst_roundtrip_tests();
void run_levenshtein_nfa_tests();
void run_fuzzy_correctness_tests();

int main() {
    run_arena_tests();
    run_radix_sort_tests();
    run_state_map_tests();
    run_fst_builder_tests();
    run_fst_roundtrip_tests();
    run_levenshtein_nfa_tests();
    run_fuzzy_correctness_tests();
    std::printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
