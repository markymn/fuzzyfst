#pragma once

#include <fuzzyfst/fuzzyfst.h>

#include <cstdint>
#include <expected>
#include <string_view>

namespace fuzzyfst {
namespace internal {

// Hyyro's bit-parallel Damerau-Levenshtein NFA state.
// Extends Myers' state with Xv to track the previous column's Eq mask,
// enabling transposition detection.
struct HyyroState {
    uint64_t Pv;      // Positive vertical deltas (same semantics as Myers')
    uint64_t Mv;      // Negative vertical deltas (same semantics as Myers')
    uint64_t Xv;      // Previous column's Eq mask — used for transposition detection
    uint32_t dist;    // Distance at bottom of column
};  // 28 bytes

struct HyyroNFA {
    // Precomputed: for each byte value, which query positions match.
    uint64_t char_mask[256];
    uint32_t query_len;    // m
    uint32_t max_dist;     // d

    // Initialize from query string.  Returns Error::QueryTooLong if
    // query.size() > 64 (bitvector representation limit).
    [[nodiscard]]
    std::expected<void, Error> init(std::string_view query,
                                     uint32_t max_distance);

    // Return the initial state (before any characters consumed).
    HyyroState start_state() const;

    // Advance the NFA by one character with equality mask `eq`.
    // Implements Hyyro's recurrence extending Myers' with transpositions.
    // ~18-20 bitwise ops, O(1), no branches in the hot path.
    static HyyroState step(const HyyroState& state,
                           uint64_t eq_mask,
                           uint32_t query_len);

    // Can this state possibly reach an accepting state within max_dist?
    bool can_match(const HyyroState& state) const;

    // Is this state accepting? (distance at end <= max_dist)
    bool is_match(const HyyroState& state) const;
};

}  // namespace internal
}  // namespace fuzzyfst
