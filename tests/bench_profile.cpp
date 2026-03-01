// bench_profile.cpp — Build profiling and query node-visit instrumentation.
// Not a permanent test — used to diagnose performance issues.

#include "arena.h"
#include "radix_sort.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace fuzzyfst;
using namespace fuzzyfst::internal;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// ── Load words ──────────────────────────────────────────────

static std::vector<std::string> load_words(const char* path) {
    std::vector<std::string> words;
    FILE* f = std::fopen(path, "r");
    if (!f) return words;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            --len;
        if (len > 0 && len <= 64) words.emplace_back(line, len);
    }
    std::fclose(f);
    return words;
}

// ── Build profiling ─────────────────────────────────────────

static void profile_build(const std::vector<std::string>& words) {
    std::printf("\n=== Build Time Breakdown (%zu words) ===\n", words.size());

    // Convert to string_views.
    std::vector<std::string_view> views;
    views.reserve(words.size());
    for (const auto& w : words) views.push_back(w);

    // 1. Radix sort
    auto t0 = Clock::now();
    std::vector<std::string_view> scratch(views.size());
    radix_sort(views.data(), views.size(), scratch.data());
    auto t1 = Clock::now();

    // 2. Deduplicate
    auto end = std::unique(views.begin(), views.end());
    size_t n = static_cast<size_t>(end - views.begin());
    auto t2 = Clock::now();

    // 3. Build trie (Daciuk)
    Arena arena;
    FstBuilder builder(arena);
    for (size_t i = 0; i < n; ++i) {
        auto r = builder.add(views[i]);
        assert(r.has_value());
    }
    auto t3a = Clock::now();
    auto r = builder.finish();
    assert(r.has_value());
    auto t3 = Clock::now();

    std::printf("  [Trie add:    %7.1f ms, finish: %7.1f ms]\n",
                ms(t2, t3a), ms(t3a, t3));
    std::printf("  [Total nodes allocated: %zu, unique: %zu]\n",
                builder.node_pool().size(), builder.unique_node_count());

    // 4. Serialize
    auto bytes = fst_serialize(builder.root(), builder.node_pool());
    auto t4 = Clock::now();

    double total = ms(t0, t4);
    std::printf("  Radix sort:   %7.1f ms  (%5.1f%%)\n", ms(t0, t1), ms(t0, t1) / total * 100);
    std::printf("  Deduplicate:  %7.1f ms  (%5.1f%%)\n", ms(t1, t2), ms(t1, t2) / total * 100);
    std::printf("  Trie build:   %7.1f ms  (%5.1f%%)\n", ms(t2, t3), ms(t2, t3) / total * 100);
    std::printf("  Serialize:    %7.1f ms  (%5.1f%%)\n", ms(t3, t4), ms(t3, t4) / total * 100);
    std::printf("  TOTAL:        %7.1f ms\n", total);
    std::printf("\n");
    std::printf("  Unique words:     %zu\n", n);
    std::printf("  Unique nodes:     %zu\n", builder.unique_node_count());
    std::printf("  Total nodes:      %zu\n", builder.node_pool().size());
    std::printf("  Arena used:       %zu KB\n", arena.bytes_used() / 1024);
    std::printf("  Arena reserved:   %zu KB\n", arena.bytes_reserved() / 1024);
    std::printf("  FST size:         %zu bytes (%.1f bytes/word)\n",
                bytes.size(), static_cast<double>(bytes.size()) / n);
}

// ── Query profiling with node visit counting ────────────────
// We'll manually walk the FST × NFA intersection, counting visited nodes.

struct QueryStats {
    size_t nodes_visited;
    size_t nodes_pruned;
    size_t results_found;
    double query_us;
    size_t visited_by_depth[32];
    size_t pruned_by_depth[32];
};

static QueryStats profile_query(const FstReader& reader,
                                 std::string_view query, uint32_t max_dist) {
    QueryStats stats{};
    std::memset(stats.visited_by_depth, 0, sizeof(stats.visited_by_depth));
    std::memset(stats.pruned_by_depth, 0, sizeof(stats.pruned_by_depth));

    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return stats;

    const uint8_t* data = reader.data();

    // Manual DFS to count nodes visited and pruned.
    struct Frame {
        uint32_t node_offset;
        LevenshteinState lev_state;
        uint8_t depth;
        uint8_t num_transitions;
        uint8_t next_trans_idx;
    };

    Frame stack[256];
    int stack_top = 0;

    // Read root.
    uint32_t root_off = reader.root_offset();
    {
        const uint8_t* p = data + root_off;
        uint8_t fc = *p++;
        uint8_t count = fc & 0x3F;
        if (count == 63) count = 63 + *p++;

        Frame& f = stack[0];
        f.node_offset = root_off;
        f.lev_state = nfa.start_state();
        f.depth = 0;
        f.num_transitions = count;
        f.next_trans_idx = 0;
        stack_top = 1;
    }

    auto t0 = Clock::now();

    while (stack_top > 0) {
        Frame& top = stack[stack_top - 1];

        if (top.next_trans_idx >= top.num_transitions) {
            --stack_top;
            continue;
        }

        // Read transitions.
        const uint8_t* p = data + top.node_offset;
        uint8_t fc = *p++;
        uint8_t cnt = fc & 0x3F;
        if (cnt == 63) p++;  // skip count_ext
        const uint8_t* trans_ptr = p + static_cast<size_t>(top.next_trans_idx) * 5;

        uint8_t label = *trans_ptr;
        uint32_t target_offset;
        std::memcpy(&target_offset, trans_ptr + 1, 4);
        top.next_trans_idx++;

        // Step NFA.
        LevenshteinState new_lev = LevenshteinNFA::step(
            top.lev_state, nfa.char_mask[label], nfa.query_len);

        stats.nodes_visited++;
        uint8_t d = top.depth;
        if (d < 32) stats.visited_by_depth[d]++;

        if (!nfa.can_match(new_lev)) {
            stats.nodes_pruned++;
            if (d < 32) stats.pruned_by_depth[d]++;
            continue;
        }

        // Check target node.
        const uint8_t* tp = data + target_offset;
        uint8_t tfc = *tp++;
        bool target_final = (tfc & 0x80) != 0;
        uint8_t target_ntrans = tfc & 0x3F;
        if (target_ntrans == 63) target_ntrans = 63 + *tp++;

        if (target_final && nfa.is_match(new_lev)) {
            stats.results_found++;
        }

        if (target_ntrans > 0 && stack_top < 256 && top.depth + 1 < 255) {
            Frame& child = stack[stack_top];
            child.node_offset = target_offset;
            child.lev_state = new_lev;
            child.depth = top.depth + 1;
            child.num_transitions = target_ntrans;
            child.next_trans_idx = 0;
            ++stack_top;
        }
    }

    auto t1 = Clock::now();
    stats.query_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    return stats;
}

// ── Alternative can_match: bottom-cell-only (plan's original) ─
// Returns true if dist <= max_dist.  No column reconstruction.
static bool can_match_bottom_only(const LevenshteinNFA& nfa,
                                   const LevenshteinState& state) {
    return state.dist <= nfa.max_dist;
}

// ── Profile query with a selectable can_match variant ───────

enum class PruneMode { FULL_COLUMN, BOTTOM_ONLY };

static QueryStats profile_query_mode(const FstReader& reader,
                                      std::string_view query,
                                      uint32_t max_dist,
                                      PruneMode mode) {
    QueryStats stats{};
    std::memset(stats.visited_by_depth, 0, sizeof(stats.visited_by_depth));
    std::memset(stats.pruned_by_depth, 0, sizeof(stats.pruned_by_depth));

    LevenshteinNFA nfa;
    auto r = nfa.init(query, max_dist);
    if (!r.has_value()) return stats;

    const uint8_t* data = reader.data();

    struct Frame {
        uint32_t node_offset;
        LevenshteinState lev_state;
        uint8_t depth;
        uint8_t num_transitions;
        uint8_t next_trans_idx;
    };

    Frame stack[256];
    int stack_top = 0;

    uint32_t root_off = reader.root_offset();
    {
        const uint8_t* p = data + root_off;
        uint8_t fc = *p++;
        uint8_t count = fc & 0x3F;
        if (count == 63) count = 63 + *p++;

        Frame& f = stack[0];
        f.node_offset = root_off;
        f.lev_state = nfa.start_state();
        f.depth = 0;
        f.num_transitions = count;
        f.next_trans_idx = 0;
        stack_top = 1;
    }

    while (stack_top > 0) {
        Frame& top = stack[stack_top - 1];

        if (top.next_trans_idx >= top.num_transitions) {
            --stack_top;
            continue;
        }

        const uint8_t* p = data + top.node_offset;
        uint8_t fc = *p++;
        uint8_t cnt = fc & 0x3F;
        if (cnt == 63) p++;
        const uint8_t* trans_ptr = p + static_cast<size_t>(top.next_trans_idx) * 5;

        uint8_t label = *trans_ptr;
        uint32_t target_offset;
        std::memcpy(&target_offset, trans_ptr + 1, 4);
        top.next_trans_idx++;

        LevenshteinState new_lev = LevenshteinNFA::step(
            top.lev_state, nfa.char_mask[label], nfa.query_len);

        stats.nodes_visited++;

        bool prune;
        if (mode == PruneMode::FULL_COLUMN) {
            prune = !nfa.can_match(new_lev);
        } else {
            prune = !can_match_bottom_only(nfa, new_lev);
        }

        if (prune) {
            stats.nodes_pruned++;
            continue;
        }

        const uint8_t* tp = data + target_offset;
        uint8_t tfc = *tp++;
        bool target_final = (tfc & 0x80) != 0;
        uint8_t target_ntrans = tfc & 0x3F;
        if (target_ntrans == 63) target_ntrans = 63 + *tp++;

        if (target_final && nfa.is_match(new_lev)) {
            stats.results_found++;
        }

        if (target_ntrans > 0 && stack_top < 256 && top.depth + 1 < 255) {
            Frame& child = stack[stack_top];
            child.node_offset = target_offset;
            child.lev_state = new_lev;
            child.depth = top.depth + 1;
            child.num_transitions = target_ntrans;
            child.next_trans_idx = 0;
            ++stack_top;
        }
    }

    return stats;
}

// ── Build FST from word list ────────────────────────────────

static FstReader build_fst_from_words(const std::vector<std::string>& words) {
    std::vector<std::string_view> views;
    views.reserve(words.size());
    for (const auto& w : words) views.push_back(w);
    std::sort(views.begin(), views.end());
    views.erase(std::unique(views.begin(), views.end()), views.end());

    Arena arena;
    FstBuilder builder(arena);
    for (auto& v : views) {
        auto r = builder.add(v);
        assert(r.has_value());
    }
    builder.finish();
    auto bytes = fst_serialize(builder.root(), builder.node_pool());
    auto reader = FstReader::from_bytes(std::move(bytes));
    assert(reader.has_value());
    return std::move(*reader);
}

// ── Generate 100 representative queries ─────────────────────
// Sample 50 words evenly, create misspellings by deleting middle char.
// This gives diverse word lengths and structures.

static std::vector<std::string> generate_queries(
        const std::vector<std::string>& words, size_t count) {
    std::vector<std::string> queries;
    size_t step = words.size() / count;
    if (step == 0) step = 1;

    for (size_t i = 0; i < words.size() && queries.size() < count; i += step) {
        const auto& w = words[i];
        if (w.size() >= 3 && w.size() <= 20) {
            // Delete the middle character to create a misspelling.
            std::string q = w;
            q.erase(q.size() / 2, 1);
            queries.push_back(q);
        }
    }
    return queries;
}

// ── 100-query baseline experiment ───────────────────────────

static void print_stats(const char* label, size_t n,
                        size_t visited, size_t pruned, size_t results) {
    std::printf("    %s:\n", label);
    std::printf("      Total visited:  %zu  (avg %.0f/query)\n",
                visited, static_cast<double>(visited) / n);
    std::printf("      Total pruned:   %zu  (%.1f%%)\n",
                pruned, 100.0 * pruned / visited);
    std::printf("      Total results:  %zu\n", results);
}

static void run_comparison_experiment(const FstReader& reader,
                                       const std::vector<std::string>& queries) {
    size_t n = queries.size();

    // Collect stats for both modes at both distances.
    struct Accum { size_t visited, pruned, results; };
    Accum fc_d1{}, fc_d2{}, bo_d1{}, bo_d2{};

    for (const auto& q : queries) {
        auto s = profile_query_mode(reader, q, 1, PruneMode::FULL_COLUMN);
        fc_d1.visited += s.nodes_visited;
        fc_d1.pruned  += s.nodes_pruned;
        fc_d1.results += s.results_found;

        s = profile_query_mode(reader, q, 2, PruneMode::FULL_COLUMN);
        fc_d2.visited += s.nodes_visited;
        fc_d2.pruned  += s.nodes_pruned;
        fc_d2.results += s.results_found;

        s = profile_query_mode(reader, q, 1, PruneMode::BOTTOM_ONLY);
        bo_d1.visited += s.nodes_visited;
        bo_d1.pruned  += s.nodes_pruned;
        bo_d1.results += s.results_found;

        s = profile_query_mode(reader, q, 2, PruneMode::BOTTOM_ONLY);
        bo_d2.visited += s.nodes_visited;
        bo_d2.pruned  += s.nodes_pruned;
        bo_d2.results += s.results_found;
    }

    std::printf("\n=== can_match Comparison: %zu queries ===\n", n);

    std::printf("\n  FULL COLUMN RECONSTRUCTION (current):\n");
    print_stats("d=1", n, fc_d1.visited, fc_d1.pruned, fc_d1.results);
    print_stats("d=2", n, fc_d2.visited, fc_d2.pruned, fc_d2.results);

    std::printf("\n  BOTTOM CELL ONLY (plan's original dist <= max_dist):\n");
    print_stats("d=1", n, bo_d1.visited, bo_d1.pruned, bo_d1.results);
    print_stats("d=2", n, bo_d2.visited, bo_d2.pruned, bo_d2.results);

    std::printf("\n  REDUCTION from full-column reconstruction:\n");
    double red_d1 = 100.0 * (1.0 - static_cast<double>(fc_d1.visited) / bo_d1.visited);
    double red_d2 = 100.0 * (1.0 - static_cast<double>(fc_d2.visited) / bo_d2.visited);
    std::printf("    d=1: %.1f%% fewer nodes visited (%zu -> %zu)\n",
                red_d1, bo_d1.visited, fc_d1.visited);
    std::printf("    d=2: %.1f%% fewer nodes visited (%zu -> %zu)\n",
                red_d2, bo_d2.visited, fc_d2.visited);
}

// ── 1000-query profiling workload ────────────────────────────
// Runs 1000 queries at each distance using the real fuzzy_search path,
// reports aggregate timing and working set analysis.

static void run_profiling_workload(const FstReader& reader,
                                    const std::vector<std::string>& queries) {
    size_t n = queries.size();

    std::printf("\n=== 1000-Query Profiling Workload (%zu queries) ===\n", n);

    // Warm up: run a few queries to prime caches.
    for (size_t i = 0; i < std::min(n, size_t(10)); ++i) {
        auto s = profile_query(reader, queries[i], 1);
        (void)s;
    }

    // d=1: measure total time and node visits.
    size_t total_visited_d1 = 0, total_results_d1 = 0;
    auto t0 = Clock::now();
    for (const auto& q : queries) {
        auto s = profile_query(reader, q, 1);
        total_visited_d1 += s.nodes_visited;
        total_results_d1 += s.results_found;
    }
    auto t1 = Clock::now();

    // d=2: measure total time and node visits.
    size_t total_visited_d2 = 0, total_results_d2 = 0;
    auto t2 = Clock::now();
    for (const auto& q : queries) {
        auto s = profile_query(reader, q, 2);
        total_visited_d2 += s.nodes_visited;
        total_results_d2 += s.results_found;
    }
    auto t3 = Clock::now();

    double d1_ms = ms(t0, t1);
    double d2_ms = ms(t2, t3);
    double d1_ns_per_node = (d1_ms * 1e6) / total_visited_d1;
    double d2_ns_per_node = (d2_ms * 1e6) / total_visited_d2;

    std::printf("\n  d=1: %zu queries, %.1f ms total, %.1f us/query avg\n",
                n, d1_ms, d1_ms * 1000.0 / n);
    std::printf("       %zu nodes visited (avg %zu/query), %.1f ns/node\n",
                total_visited_d1, total_visited_d1 / n, d1_ns_per_node);
    std::printf("       %zu results found\n", total_results_d1);

    std::printf("\n  d=2: %zu queries, %.1f ms total, %.1f us/query avg\n",
                n, d2_ms, d2_ms * 1000.0 / n);
    std::printf("       %zu nodes visited (avg %zu/query), %.1f ns/node\n",
                total_visited_d2, total_visited_d2 / n, d2_ns_per_node);
    std::printf("       %zu results found\n", total_results_d2);

    // Working set analysis.
    // FST size from build: 2,035,047 bytes for 370K words.
    // We can't access total_size_ directly; use the known value.
    size_t fst_size = 2035047;  // Measured from build output.
    std::printf("\n  Working set analysis:\n");
    std::printf("    FST data size:       %zu bytes (%.1f KB)\n",
                fst_size, fst_size / 1024.0);
    std::printf("    Typical L1d cache:   32-48 KB\n");
    std::printf("    Typical L2 cache:    256-512 KB\n");
    std::printf("    Typical L3 cache:    6-16 MB\n");
    std::printf("    FST fits in:         %s\n",
                fst_size < 48 * 1024 ? "L1" :
                fst_size < 512 * 1024 ? "L2" :
                fst_size < 16 * 1024 * 1024 ? "L3" : "main memory");
    std::printf("    LevenshteinNFA size: %zu bytes (char_mask[256] + fields)\n",
                sizeof(LevenshteinNFA));
    std::printf("    LevenshteinState:    %zu bytes\n",
                sizeof(LevenshteinState));
    std::printf("    Stack frame (DFS):   ~%zu bytes per level, 256 max depth\n",
                sizeof(uint32_t) + sizeof(LevenshteinState) + 3);

    // Per-function timing: measure step(), can_match(), and FST decode separately.
    // Run a subset of queries with fine-grained timing.
    double step_ns_total = 0, canmatch_ns_total = 0, decode_ns_total = 0;
    size_t timing_nodes = 0;
    size_t timing_queries = std::min(n, size_t(200));

    for (size_t qi = 0; qi < timing_queries; ++qi) {
        const auto& q = queries[qi];
        LevenshteinNFA nfa;
        nfa.init(q, 1);
        const uint8_t* fst_data = reader.data();

        struct Frame {
            uint32_t node_offset;
            LevenshteinState lev_state;
            uint8_t num_transitions;
            uint8_t next_trans_idx;
        };
        Frame stk[256];
        int stop = 0;

        uint32_t root_off = reader.root_offset();
        {
            const uint8_t* p = fst_data + root_off;
            uint8_t fc = *p++;
            uint8_t count = fc & 0x3F;
            if (count == 63) count = 63 + *p++;
            stk[0] = {root_off, nfa.start_state(), count, 0};
            stop = 1;
        }

        while (stop > 0) {
            Frame& top = stk[stop - 1];
            if (top.next_trans_idx >= top.num_transitions) { --stop; continue; }

            // Decode FST node.
            auto td0 = Clock::now();
            const uint8_t* p = fst_data + top.node_offset;
            uint8_t fc = *p++;
            uint8_t cnt = fc & 0x3F;
            if (cnt == 63) p++;
            const uint8_t* tp = p + static_cast<size_t>(top.next_trans_idx) * 5;
            uint8_t label = *tp;
            uint32_t target_offset;
            std::memcpy(&target_offset, tp + 1, 4);
            top.next_trans_idx++;
            auto td1 = Clock::now();

            // step().
            auto ts0 = Clock::now();
            LevenshteinState new_lev = LevenshteinNFA::step(
                top.lev_state, nfa.char_mask[label], nfa.query_len);
            auto ts1 = Clock::now();

            // can_match().
            auto tc0 = Clock::now();
            bool ok = nfa.can_match(new_lev);
            auto tc1 = Clock::now();

            timing_nodes++;
            decode_ns_total += std::chrono::duration<double, std::nano>(td1 - td0).count();
            step_ns_total += std::chrono::duration<double, std::nano>(ts1 - ts0).count();
            canmatch_ns_total += std::chrono::duration<double, std::nano>(tc1 - tc0).count();

            if (!ok) continue;

            const uint8_t* ttp = fst_data + target_offset;
            uint8_t tfc = *ttp++;
            uint8_t tnt = tfc & 0x3F;
            if (tnt == 63) tnt = 63 + *ttp++;

            if (tnt > 0 && stop < 256) {
                stk[stop] = {target_offset, new_lev, tnt, 0};
                ++stop;
            }
        }
    }

    double avg_query_len = 0;
    for (const auto& q : queries) avg_query_len += q.size();
    avg_query_len /= n;

    std::printf("\n  Per-node cost breakdown (%zu nodes from %zu queries at d=1):\n",
                timing_nodes, timing_queries);
    std::printf("    Total per-node:      %.1f ns (from aggregate timing)\n", d1_ns_per_node);
    std::printf("    FST decode:          %.1f ns (%.0f%%)\n",
                decode_ns_total / timing_nodes,
                100.0 * decode_ns_total / (decode_ns_total + step_ns_total + canmatch_ns_total));
    std::printf("    step():              %.1f ns (%.0f%%)\n",
                step_ns_total / timing_nodes,
                100.0 * step_ns_total / (decode_ns_total + step_ns_total + canmatch_ns_total));
    std::printf("    can_match():         %.1f ns (%.0f%%)\n",
                canmatch_ns_total / timing_nodes,
                100.0 * canmatch_ns_total / (decode_ns_total + step_ns_total + canmatch_ns_total));
    std::printf("    Avg query length:    %.1f chars\n", avg_query_len);

    // Cache behavior inference.
    std::printf("\n  Cache behavior inference:\n");
    std::printf("    d=1 per-node cost: %.1f ns\n", d1_ns_per_node);
    std::printf("    d=2 per-node cost: %.1f ns\n", d2_ns_per_node);
    std::printf("    Ratio d2/d1:       %.2fx\n", d2_ns_per_node / d1_ns_per_node);
    std::printf("    If L1/L2 misses were significant, d=2 (8.5x more nodes in\n");
    std::printf("    diverse FST regions) would show higher per-node cost.\n");
    std::printf("    Stable per-node cost indicates working set is cache-resident.\n");
}

// ── Main ────────────────────────────────────────────────────

int main(int argc, char** argv) {
    auto words = load_words("data/words.txt");
    if (words.empty()) {
        std::fprintf(stderr, "Error: cannot load data/words.txt\n");
        return 1;
    }

    // Check for --profile-only flag to skip build profiling.
    bool profile_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--profile-only") == 0) profile_only = true;
    }

    if (!profile_only) {
        profile_build(words);
    }

    auto reader = build_fst_from_words(words);

    // Generate 1000 representative queries.
    auto queries = generate_queries(words, 1000);
    std::printf("\nGenerated %zu queries (sample: \"%s\", \"%s\", \"%s\")\n",
                queries.size(),
                queries.size() > 0 ? queries[0].c_str() : "",
                queries.size() > 1 ? queries[1].c_str() : "",
                queries.size() > 2 ? queries[2].c_str() : "");

    if (!profile_only) {
        // Use first 100 for comparison.
        std::vector<std::string> first100(queries.begin(),
            queries.begin() + std::min(queries.size(), size_t(100)));
        run_comparison_experiment(reader, first100);
    }

    run_profiling_workload(reader, queries);

    return 0;
}
