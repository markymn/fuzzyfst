#pragma once

#include <fuzzyfst/fuzzyfst.h>

#include <cstdint>
#include <expected>
#include <string_view>

namespace fuzzyfst {
namespace internal {

struct LevenshteinState {
    uint64_t Pv;    // Positive vertical delta bitvector
    uint64_t Mv;    // Negative vertical delta bitvector
    uint32_t dist;  // Current distance at the bottom of the column
};

struct LevenshteinNFA {
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
    LevenshteinState start_state() const;

    // Advance the NFA by one character with equality mask `eq`.
    // Pure bitwise, ~12 instructions.
    static LevenshteinState step(const LevenshteinState& state,
                                  uint64_t eq_mask,
                                  uint32_t query_len);

    // Can this state possibly reach an accepting state within max_dist?
    bool can_match(const LevenshteinState& state) const;

    // Is this state accepting? (distance at end <= max_dist)
    bool is_match(const LevenshteinState& state) const;
};

}  // namespace internal
}  // namespace fuzzyfst
