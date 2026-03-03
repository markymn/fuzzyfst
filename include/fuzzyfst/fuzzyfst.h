#pragma once
#include <cstdint>
#include <cstddef>
#include <expected>
#include <string_view>
#include <span>
#include <vector>

namespace fuzzyfst {

// Error codes
enum class Error : int {
    Ok = 0,
    FileOpenFailed,
    MmapFailed,
    InvalidFormat,
    InputNotSorted,
    BufferTooSmall,
    QueryTooLong,      // Query exceeds 64 characters (bitvector limit)
};

// ── Types ──────────────────────────────────────────────────

// A single fuzzy search result.  `word` points into the caller-provided
// word buffer (zero-alloc overload) or into a heap-allocated string
// (convenience overload).
struct FuzzyResult {
    std::string_view word;
    uint32_t         distance;
};

// Distance metric for fuzzy search.
enum class DistanceMetric {
    Levenshtein,          // Standard Levenshtein (insertions, deletions, substitutions)
    DamerauLevenshtein,   // Damerau-Levenshtein (+ adjacent transpositions)
};

// Algorithm hint for fuzzy search.
enum class Algorithm {
    BitParallel,   // Default — zero startup cost, register-resident state.
    DFA,           // Precompiled DFA — higher startup cost, faster per-node.
                   // Best for batch workloads reusing same (query, distance) pair.
};

// Options for fuzzy_search.
struct SearchOptions {
    uint32_t       max_distance = 1;
    DistanceMetric metric       = DistanceMetric::Levenshtein;
    Algorithm      algorithm    = Algorithm::BitParallel;
};

// ── Build API ──────────────────────────────────────────────

struct BuildOptions {
    bool sort_input = true;    // If true, radix-sort the input
};

// Build an FST from a list of words and write to `output_path`.
// Words are provided as a contiguous buffer of newline-separated strings.
// Returns Error::InputNotSorted if sort_input is false and words are not
// in strict lexicographic order.
// build() is NOT thread-safe — do not call concurrently on the same output path.
[[nodiscard]]
std::expected<void, Error> build(const char* output_path,
                                  std::span<std::string_view> words,
                                  BuildOptions opts = {});

// ── Query API ──────────────────────────────────────────────

// Thread safety:
//   Fst is immutable after construction.  contains() and fuzzy_search()
//   are safe to call concurrently from any number of threads with no
//   synchronization required.  The underlying mmap'd memory is read-only
//   and shared.  Each thread must provide its own result/word buffers.
//
// Query length limit:
//   The bit-parallel Levenshtein NFA uses uint64_t bitvectors, so query
//   strings longer than 64 characters are not supported.  fuzzy_search()
//   returns 0 results (zero-alloc overload) or an empty vector (convenience
//   overload) if the query exceeds this limit.
//
// Distance recommendation:
//   max_distance > 3 is not recommended.  At distance >= 3 on a large
//   dictionary, the intersection explodes combinatorially and results
//   become meaningless (almost every short word matches everything).
//   A debug assert fires if max_distance > 3 in debug builds.
class Fst {
public:
    // Open a previously-built FST file (zero-copy mmap).
    [[nodiscard]]
    static std::expected<Fst, Error> open(const char* path);

    ~Fst();
    Fst(Fst&&) noexcept;
    Fst& operator=(Fst&&) noexcept;

    // Exact membership test.  O(key length).
    [[nodiscard]] bool contains(std::string_view key) const;

    // Fuzzy search: find all words within `max_distance` edits of `query`.
    // Results are written into `results`.  Returns number of matches found.
    // `word_buf` is scratch space for assembling result strings.
    // This function performs ZERO heap allocations.
    // Returns 0 if query.size() > 64.
    [[nodiscard]]
    size_t fuzzy_search(std::string_view query,
                        uint32_t max_distance,
                        std::span<FuzzyResult> results,
                        std::span<char> word_buf) const;

    // Convenience overload that heap-allocates results.
    // Returns empty vector if query.size() > 64.
    [[nodiscard]]
    std::vector<FuzzyResult> fuzzy_search(std::string_view query,
                                           uint32_t max_distance) const;

    // SearchOptions overloads — supports Damerau-Levenshtein metric.
    [[nodiscard]]
    size_t fuzzy_search(std::string_view query,
                        SearchOptions opts,
                        std::span<FuzzyResult> results,
                        std::span<char> word_buf) const;

    [[nodiscard]]
    std::vector<FuzzyResult> fuzzy_search(std::string_view query,
                                           SearchOptions opts) const;

    uint32_t num_nodes() const;

private:
    Fst() = default;
    struct Impl;
    Impl* impl_ = nullptr;    // Pimpl — hides FstReader + internals
};

}  // namespace fuzzyfst
