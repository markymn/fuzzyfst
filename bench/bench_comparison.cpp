// bench_comparison.cpp — Compare FuzzyFST vs SymSpell vs brute-force linear scan.
// All three are tested on the same 370K-word dictionary and the same query set.

#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"
#include "arena.h"
#include "radix_sort.h"
#include "trie_builder.h"
#include "fst_writer.h"

// SymSpell (yams-symspell, header-only in-memory mode)
#include "symspell/symspell.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

static size_t get_rss_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize;
#endif
    return 0;
}

using namespace fuzzyfst::internal;
using Clock = std::chrono::steady_clock;

// ── Utilities ────────────────────────────────────────────────

static double to_us(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

static double to_ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
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
        if (len > 0) lines.emplace_back(buf, len);
    }
    std::fclose(f);
    return lines;
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
        if (len == 0 || buf[0] == '#') continue;  // Skip comments and blanks.
        queries.emplace_back(buf, len);
    }
    std::fclose(f);
    return queries;
}

// ── Brute-force Levenshtein ──────────────────────────────────

static uint32_t levenshtein(std::string_view a, std::string_view b) {
    size_t m = a.size(), n = b.size();
    std::vector<uint32_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = static_cast<uint32_t>(j);
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = static_cast<uint32_t>(i);
        for (size_t j = 1; j <= n; ++j) {
            uint32_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1,
                                 prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

struct BruteResult {
    size_t count;
    double latency_us;
};

static BruteResult brute_force_query(const std::vector<std::string>& dict,
                                      std::string_view query,
                                      uint32_t max_dist) {
    auto t0 = Clock::now();
    size_t count = 0;
    for (const auto& word : dict) {
        if (levenshtein(query, word) <= max_dist) ++count;
    }
    auto t1 = Clock::now();
    return {count, to_us(t0, t1)};
}

// ── FuzzyFST builder ────────────────────────────────────────

static FstReader build_fst(const std::vector<std::string>& words,
                            double& build_ms, size_t& fst_bytes) {
    std::vector<std::string_view> views;
    views.reserve(words.size());
    for (const auto& w : words) views.push_back(w);
    std::sort(views.begin(), views.end());
    views.erase(std::unique(views.begin(), views.end()), views.end());

    auto t0 = Clock::now();
    Arena arena;
    FstBuilder builder(arena);
    for (auto& v : views) {
        auto r = builder.add(v);
        assert(r.has_value());
    }
    auto fr = builder.finish();
    assert(fr.has_value());
    auto bytes = fst_serialize(builder.root(), builder.node_pool());
    auto t1 = Clock::now();
    build_ms = to_ms(t0, t1);
    fst_bytes = bytes.size();

    auto reader = FstReader::from_bytes(std::move(bytes));
    assert(reader.has_value());
    return std::move(*reader);
}

// ── FuzzyFST query ──────────────────────────────────────────

struct FstQueryResult {
    size_t count;
    double latency_us;
};

static fuzzyfst::FuzzyResult s_results[16384];
static char s_word_buf[262144];

static FstQueryResult fst_query(const FstReader& reader,
                                 std::string_view query,
                                 uint32_t max_dist) {
    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return {0, 0};

    auto t0 = Clock::now();
    FuzzyIterator iter(reader, nfa, s_word_buf, sizeof(s_word_buf),
                       s_results, 16384);
    size_t count = 0;
    while (!iter.done() && count < 16384) {
        count += iter.collect();
    }
    auto t1 = Clock::now();
    return {count, to_us(t0, t1)};
}

// ── SymSpell wrapper (yams-symspell) ─────────────────────────

struct SymSpellBench {
    std::unique_ptr<yams::symspell::SymSpell> ss;
    double build_ms = 0;
    size_t memory_bytes = 0;

    void build(const std::vector<std::string>& words, int max_edit_dist) {
        size_t rss_before = get_rss_bytes();
        auto t0 = Clock::now();
        int prefix_len = std::max(max_edit_dist + 4, 7);
        auto store = std::make_unique<yams::symspell::MemoryStore>(max_edit_dist, prefix_len);
        ss = std::make_unique<yams::symspell::SymSpell>(std::move(store), max_edit_dist, prefix_len);
        for (const auto& w : words) {
            ss->createDictionaryEntry(w, 1);
        }
        auto t1 = Clock::now();
        build_ms = to_ms(t0, t1);
        size_t rss_after = get_rss_bytes();
        memory_bytes = (rss_after > rss_before) ? (rss_after - rss_before) : 0;
    }

    size_t query(std::string_view input, int max_edit_dist, double& latency_us) {
        auto t0 = Clock::now();
        auto suggestions = ss->lookup(input, yams::symspell::Verbosity::All, max_edit_dist);
        auto t1 = Clock::now();
        latency_us = to_us(t0, t1);
        return suggestions.size();
    }
};

// ── Percentile helper ────────────────────────────────────────

static double percentile(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p / 100.0 * (v.size() - 1));
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

// ── Main ─────────────────────────────────────────────────────

int main() {
    // Load data.
    auto words = load_file("data/words.txt");
    if (words.empty()) {
        std::fprintf(stderr, "Error: cannot load data/words.txt\n");
        return 1;
    }
    auto queries = load_queries("data/benchmark_queries.txt");
    if (queries.empty()) {
        std::fprintf(stderr, "Error: cannot load data/benchmark_queries.txt\n");
        return 1;
    }
    std::printf("Loaded %zu words, %zu queries\n\n", words.size(), queries.size());

    // ── Build phase ──────────────────────────────────────────
    std::printf("=== BUILD PHASE ===\n");

    double fst_build_ms;
    size_t fst_bytes;
    auto reader = build_fst(words, fst_build_ms, fst_bytes);
    std::printf("FuzzyFST:   build %.0f ms, index %.1f MB\n",
                fst_build_ms, fst_bytes / (1024.0 * 1024.0));

    std::printf("BruteForce: no index (linear scan)\n");

    // ── Query phase ──────────────────────────────────────────
    uint32_t distances[] = {1, 2, 3};
    size_t nq = queries.size();

    struct DistStats {
        double avg_us;
        double p99_us;
        double avg_results;
    };

    DistStats fst_stats[3], sym_stats[3], bf_stats[3];
    double sym_build_ms[3] = {};
    size_t sym_memory[3] = {};

    for (int di = 0; di < 3; ++di) {
        uint32_t d = distances[di];
        std::printf("\n=== DISTANCE %u ===\n", d);

        // FuzzyFST queries.
        {
            std::vector<double> latencies;
            size_t total_results = 0;
            for (size_t qi = 0; qi < nq; ++qi) {
                auto r = fst_query(reader, queries[qi], d);
                latencies.push_back(r.latency_us);
                total_results += r.count;
            }
            double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / nq;
            double p99 = percentile(latencies, 99);
            fst_stats[di] = {avg, p99, static_cast<double>(total_results) / nq};
            std::printf("  FuzzyFST:   avg %6.0f us, p99 %6.0f us, avg results %6.1f\n",
                        avg, p99, fst_stats[di].avg_results);
        }

        // SymSpell queries — build per-distance to control memory.
        {
            std::fprintf(stderr, "  Building SymSpell d=%u...\r", d);
            SymSpellBench sym_d;
            sym_d.build(words, static_cast<int>(d));
            sym_build_ms[di] = sym_d.build_ms;
            sym_memory[di] = sym_d.memory_bytes;
            std::vector<double> latencies;
            size_t total_results = 0;
            for (size_t qi = 0; qi < nq; ++qi) {
                double lat;
                size_t cnt = sym_d.query(queries[qi], static_cast<int>(d), lat);
                latencies.push_back(lat);
                total_results += cnt;
            }
            double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / nq;
            double p99 = percentile(latencies, 99);
            sym_stats[di] = {avg, p99, static_cast<double>(total_results) / nq};
            std::printf("  SymSpell:   avg %6.0f us, p99 %6.0f us, avg results %6.1f  (build %.0f ms, %.0f MB)\n",
                        avg, p99, sym_stats[di].avg_results, sym_d.build_ms,
                        sym_d.memory_bytes / (1024.0 * 1024.0));
        }

        // Brute force queries.
        {
            std::vector<double> latencies;
            size_t total_results = 0;
            for (size_t i = 0; i < nq; ++i) {
                auto r = brute_force_query(words, queries[i], d);
                latencies.push_back(r.latency_us);
                total_results += r.count;
            }
            double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0) / nq;
            double p99 = percentile(latencies, 99);
            bf_stats[di] = {avg, p99, static_cast<double>(total_results) / nq};
            std::printf("  BruteForce: avg %6.0f us, p99 %6.0f us, avg results %6.1f\n",
                        avg, p99, bf_stats[di].avg_results);
        }
    }

    // ── Summary table ────────────────────────────────────────
    std::printf("\n=== SUMMARY TABLE ===\n\n");

    std::printf("| Metric | FuzzyFST | SymSpell | Brute Force |\n");
    std::printf("|--------|----------|----------|-------------|\n");
    std::printf("| Index size | %.1f MB | %.0f / %.0f / %.0f MB (d=1/2/3) | 0 (no index) |\n",
                fst_bytes / (1024.0 * 1024.0),
                sym_memory[0] / (1024.0 * 1024.0),
                sym_memory[1] / (1024.0 * 1024.0),
                sym_memory[2] / (1024.0 * 1024.0));
    std::printf("| Build time | %.0f ms | %.0f / %.0f / %.0f ms (d=1/2/3) | 0 ms |\n",
                fst_build_ms, sym_build_ms[0], sym_build_ms[1], sym_build_ms[2]);
    for (int di = 0; di < 3; ++di) {
        std::printf("| Avg latency d=%d | %.0f us | %.0f us | %.0f us |\n",
                    di + 1, fst_stats[di].avg_us, sym_stats[di].avg_us, bf_stats[di].avg_us);
    }
    for (int di = 0; di < 3; ++di) {
        std::printf("| P99 latency d=%d | %.0f us | %.0f us | %.0f us |\n",
                    di + 1, fst_stats[di].p99_us, sym_stats[di].p99_us, bf_stats[di].p99_us);
    }
    for (int di = 0; di < 3; ++di) {
        std::printf("| Avg results d=%d | %.1f | %.1f | %.1f |\n",
                    di + 1, fst_stats[di].avg_results, sym_stats[di].avg_results, bf_stats[di].avg_results);
    }

    return 0;
}
