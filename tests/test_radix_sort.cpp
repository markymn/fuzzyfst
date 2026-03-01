// test_radix_sort.cpp — Tests for MSD Radix Sort

#include "radix_sort.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst::internal;

static void verify_sorted(const std::vector<std::string_view>& v) {
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i - 1] > v[i]) {
            std::printf("SORT ERROR at index %zu: '%.*s' > '%.*s'\n",
                        i,
                        static_cast<int>(v[i-1].size()), v[i-1].data(),
                        static_cast<int>(v[i].size()), v[i].data());
            assert(false);
        }
    }
}

// ── Test: Empty and single element ──────────────────────────

static void test_trivial() {
    std::vector<std::string_view> data;
    std::vector<std::string_view> scratch;

    radix_sort(data.data(), data.size(), scratch.data());
    
    std::string_view single = "alone";
    data.push_back(single);
    scratch.resize(1);
    radix_sort(data.data(), data.size(), scratch.data());

    assert(data.size() == 1);
    assert(data[0] == single);
    
    std::printf("  PASS: test_trivial\n");
}

// ── Test: Small dataset (Insertion Sort fallback) ───────────

static void test_small() {
    std::vector<std::string> strings = {
        "zebra", "apple", "banana", "x-ray", "cat", "dog", "elephant",
        "apple", "app", "ap", "a", "zap", "z", "ze", "zee"
    };

    std::vector<std::string_view> data;
    for (const auto& s : strings) data.push_back(s);
    std::vector<std::string_view> scratch(data.size());

    radix_sort(data.data(), data.size(), scratch.data());
    verify_sorted(data);

    std::printf("  PASS: test_small\n");
}

// ── Test: Large identical prefixes ──────────────────────────

static void test_identical_prefixes() {
    std::vector<std::string> strings = {
        "prefix_one", "prefix_two", "prefix_three", "prefix_four",
        "prefix_five", "prefix_six", "prefix_seven", "prefix_eight",
        "prefix_nine", "prefix_ten"
    };

    std::vector<std::string_view> data;
    for (const auto& s : strings) data.push_back(s);
    
    // Duplicate them hundreds of times to jump past insertion sort threshold
    std::vector<std::string_view> large_data;
    for (int i = 0; i < 50; ++i) {
        for (const auto& sv : data) large_data.push_back(sv);
    }
    
    std::vector<std::string_view> scratch(large_data.size());

    radix_sort(large_data.data(), large_data.size(), scratch.data());
    verify_sorted(large_data);

    std::printf("  PASS: test_identical_prefixes\n");
}

// ── Test: Large dataset (random) ────────────────────────────

static void test_large() {
    std::vector<std::string> strings;
    // Generate ~10k synthetic random strings of various lengths
    auto fast_rand = []() -> uint32_t {
        static uint32_t x = 123456789;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };

    for (int i = 0; i < 10000; ++i) {
        size_t len = (fast_rand() % 30) + 1; // 1 to 30 chars
        std::string s(len, 'a');
        for (size_t j = 0; j < len; ++j) {
            s[j] = static_cast<char>('a' + (fast_rand() % 26));
        }
        strings.push_back(std::move(s));
    }

    std::vector<std::string_view> data;
    data.reserve(strings.size());
    for (const auto& s : strings) data.push_back(s);

    std::vector<std::string_view> std_sorted = data;
    std::sort(std_sorted.begin(), std_sorted.end());

    std::vector<std::string_view> scratch(data.size());
    radix_sort(data.data(), data.size(), scratch.data());

    verify_sorted(data);
    
    // Exact match to std::sort
    for (size_t i = 0; i < data.size(); ++i) {
        assert(data[i] == std_sorted[i]);
    }

    std::printf("  PASS: test_large\n");
}

void run_radix_sort_tests() {
    std::printf("=== Radix Sort Tests ===\n");
    std::printf("Running trivial...\n"); test_trivial();
    std::printf("Running small...\n"); test_small();
    std::printf("Running identical_prefixes...\n"); test_identical_prefixes();
    std::printf("Running large...\n"); test_large();
    std::printf("=== All radix sort tests passed ===\n");
}
