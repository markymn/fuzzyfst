// bench_four_modes.cpp — Benchmark three search modes (Hyyro disabled).
// Reports startup cost, per-query latency (avg/p99), and result counts
// for Levenshtein BitParallel, Levenshtein DFA, and Damerau DFA at d=1..4.

#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"
#include "levenshtein_dfa.h"
#include "levenshtein_dfa_search.h"
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

struct ModeResult {
    double startup_us;
    double query_us;
    size_t results;
};

static ModeResult bench_lev_bp(const FstReader& reader, const std::string& query, uint32_t max_d) {
    auto t0 = Clock::now();
    LevenshteinNFA nfa;
    nfa.init(query, max_d);
    auto t1 = Clock::now();

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    FuzzyIterator iter(reader, nfa, word_buf, sizeof(word_buf), result_buf, 8192);

    auto t2 = Clock::now();
    size_t n = 0;
    while (!iter.done()) n += iter.collect();
    auto t3 = Clock::now();

    return {to_us(t0, t1), to_us(t2, t3), n};
}

static ModeResult bench_lev_dfa(const FstReader& reader, const std::string& query, uint32_t max_d) {
    auto t0 = Clock::now();
    LevenshteinDFA dfa;
    dfa.init(query, max_d);
    auto t1 = Clock::now();

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    LevenshteinDFAIterator iter(reader, dfa, word_buf, sizeof(word_buf), result_buf, 8192);

    auto t2 = Clock::now();
    size_t n = 0;
    while (!iter.done()) n += iter.collect();
    auto t3 = Clock::now();

    return {to_us(t0, t1), to_us(t2, t3), n};
}

static ModeResult bench_dam_dfa(const FstReader& reader, const std::string& query, uint32_t max_d) {
    auto t0 = Clock::now();
    DamerauNFA dfa;
    dfa.init(query, max_d);
    auto t1 = Clock::now();

    char word_buf[65536];
    FuzzyResult result_buf[8192];
    DamerauIterator iter(reader, dfa, word_buf, sizeof(word_buf), result_buf, 8192);

    auto t2 = Clock::now();
    size_t n = 0;
    while (!iter.done()) n += iter.collect();
    auto t3 = Clock::now();

    return {to_us(t0, t1), to_us(t2, t3), n};
}

int main() {
    auto words = load_file("data/words.txt");
    if (words.empty()) {
        std::fprintf(stderr, "Error: cannot load data/words.txt\n");
        return 1;
    }
    std::printf("Loaded %zu words.\n\n", words.size());

    // Load queries or use defaults.
    std::vector<std::string> queries;
    auto loaded = load_file("data/benchmark_queries.txt");
    if (!loaded.empty()) {
        for (auto& q : loaded) {
            if (!q.empty() && q[0] != '#') queries.push_back(q);
        }
    }
    if (queries.empty()) {
        queries = {"teh", "helo", "freind", "wierd", "recieve",
                   "algoritm", "aple", "wrld", "becuase", "abotu"};
    }
    int nq = static_cast<int>(queries.size());
    std::printf("Using %d queries.\n\n", nq);

    auto reader = build_fst_from_words(words);

    const char* mode_names[] = {
        "Lev BitParallel",
        "Lev DFA        ",
        "Dam DFA        "
    };

    for (uint32_t max_d = 1; max_d <= 4; ++max_d) {
        std::printf("=== Distance d=%u ===\n", max_d);
        std::printf("%-18s %10s %10s %10s %10s %10s\n",
                    "Mode", "Startup", "Avg(us)", "P99(us)", "AvgResults", "Queries");

        for (int mode = 0; mode < 3; ++mode) {
            std::vector<double> startup_times, query_times;
            size_t total_results = 0;

            for (int qi = 0; qi < nq; ++qi) {
                ModeResult mr;
                switch (mode) {
                    case 0: mr = bench_lev_bp(reader, queries[qi], max_d); break;
                    case 1: mr = bench_lev_dfa(reader, queries[qi], max_d); break;
                    case 2: mr = bench_dam_dfa(reader, queries[qi], max_d); break;
                }
                startup_times.push_back(mr.startup_us);
                query_times.push_back(mr.query_us);
                total_results += mr.results;
            }

            double startup_avg = std::accumulate(startup_times.begin(), startup_times.end(), 0.0) / nq;
            double query_avg = std::accumulate(query_times.begin(), query_times.end(), 0.0) / nq;
            std::sort(query_times.begin(), query_times.end());
            double query_p99 = query_times[std::min(static_cast<size_t>(nq * 0.99), query_times.size() - 1)];
            double avg_results = static_cast<double>(total_results) / nq;

            std::printf("%-18s %8.0f us %8.0f us %8.0f us %10.1f %10d\n",
                        mode_names[mode], startup_avg, query_avg, query_p99,
                        avg_results, nq);
        }
        std::printf("\n");
    }

    return 0;
}
