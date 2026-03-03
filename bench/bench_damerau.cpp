// bench_damerau.cpp — Benchmark Damerau-Levenshtein DFA compilation and query.
// Uses the same 370K-word dictionary and reports DFA compilation time,
// state count, and per-query latency at d=1..4.

#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"
#include "damerau_nfa.h"
#include "damerau_search.h"
#include "arena.h"
#include "radix_sort.h"
#include "trie_builder.h"
#include "fst_writer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;
using Clock = std::chrono::steady_clock;

static double to_us(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

static std::vector<std::string> load_file(const char* path) {
    std::vector<std::string> lines;
    FILE* f = std::fopen(path, "r");
    if (!f) return lines;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            --len;
        if (len > 0 && len <= 64) lines.emplace_back(buf, len);
    }
    std::fclose(f);
    return lines;
}

static FstReader build_fst_from_words(const std::vector<std::string>& words) {
    std::vector<std::string_view> views;
    views.reserve(words.size());
    for (auto& w : words) views.push_back(w);

    std::vector<std::string_view> scratch(views.size());
    radix_sort(views.data(), views.size(), scratch.data());
    auto end = std::unique(views.begin(), views.end());
    size_t n = static_cast<size_t>(end - views.begin());

    Arena arena;
    FstBuilder builder(arena);
    for (size_t i = 0; i < n; ++i) builder.add(views[i]);
    builder.finish();

    auto bytes = fst_serialize(builder.root(), builder.node_pool());
    auto reader = FstReader::from_bytes(std::move(bytes));
    assert(reader.has_value());
    return std::move(*reader);
}

static std::vector<std::string> load_queries(const char* path) {
    std::vector<std::string> queries;
    FILE* f = std::fopen(path, "r");
    if (!f) return queries;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            --len;
        if (len == 0 || buf[0] == '#') continue;
        queries.emplace_back(buf, len);
    }
    std::fclose(f);
    return queries;
}

int main() {
    auto words = load_file("data/words.txt");
    if (words.empty()) {
        std::fprintf(stderr, "Error: cannot load data/words.txt\n");
        return 1;
    }
    std::printf("Loaded %zu words.\n\n", words.size());

    auto queries = load_queries("data/benchmark_queries.txt");
    if (queries.empty()) {
        std::fprintf(stderr, "Error: cannot load data/benchmark_queries.txt\n");
        return 1;
    }
    int num_queries = static_cast<int>(queries.size());
    std::printf("Loaded %d queries.\n\n", num_queries);

    auto reader = build_fst_from_words(words);

    for (uint32_t max_d = 1; max_d <= 4; ++max_d) {
        std::printf("=== Distance d=%u ===\n", max_d);

        // --- Levenshtein ---
        std::vector<double> lev_times;
        size_t lev_total_results = 0;
        for (int qi = 0; qi < num_queries; ++qi) {
            LevenshteinNFA nfa;
            nfa.init(queries[qi], max_d);

            char word_buf[65536];
            FuzzyResult result_buf[8192];
            FuzzyIterator iter(reader, nfa, word_buf, sizeof(word_buf),
                               result_buf, 8192);

            auto t0 = Clock::now();
            size_t n = 0;
            while (!iter.done()) n += iter.collect();
            auto t1 = Clock::now();

            lev_times.push_back(to_us(t0, t1));
            lev_total_results += n;
        }

        double lev_avg = std::accumulate(lev_times.begin(), lev_times.end(), 0.0) / num_queries;
        std::sort(lev_times.begin(), lev_times.end());
        double lev_p99 = lev_times[static_cast<size_t>(num_queries * 0.99)];
        double lev_avg_results = static_cast<double>(lev_total_results) / num_queries;

        // --- Damerau ---
        std::vector<double> dam_times;
        std::vector<double> dfa_compile_times;
        size_t dam_total_results = 0;
        size_t total_states = 0;

        for (int qi = 0; qi < num_queries; ++qi) {
            DamerauNFA dfa;
            auto tc0 = Clock::now();
            dfa.init(queries[qi], max_d);
            auto tc1 = Clock::now();
            dfa_compile_times.push_back(to_us(tc0, tc1));
            total_states += dfa.num_states();

            char word_buf[65536];
            FuzzyResult result_buf[8192];
            DamerauIterator iter(reader, dfa, word_buf, sizeof(word_buf),
                                 result_buf, 8192);

            auto t0 = Clock::now();
            size_t n = 0;
            while (!iter.done()) n += iter.collect();
            auto t1 = Clock::now();

            dam_times.push_back(to_us(t0, t1));
            dam_total_results += n;
        }

        double dam_avg = std::accumulate(dam_times.begin(), dam_times.end(), 0.0) / num_queries;
        std::sort(dam_times.begin(), dam_times.end());
        double dam_p99 = dam_times[static_cast<size_t>(num_queries * 0.99)];
        double dam_avg_results = static_cast<double>(dam_total_results) / num_queries;
        double compile_avg = std::accumulate(dfa_compile_times.begin(), dfa_compile_times.end(), 0.0) / num_queries;
        double avg_states = static_cast<double>(total_states) / num_queries;

        std::printf("  Levenshtein:  avg=%7.0f us  p99=%7.0f us  avg_results=%.1f\n",
                    lev_avg, lev_p99, lev_avg_results);
        std::printf("  Damerau:      avg=%7.0f us  p99=%7.0f us  avg_results=%.1f\n",
                    dam_avg, dam_p99, dam_avg_results);
        std::printf("  DFA compile:  avg=%7.0f us  avg_states=%.0f\n",
                    compile_avg, avg_states);
        std::printf("\n");
    }

    return 0;
}
