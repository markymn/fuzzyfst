#pragma once

#include <fuzzyfst/fuzzyfst.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Compiled Levenshtein DFA (no transpositions).
//
// Same BFS DP-column enumeration approach as DamerauNFA, but uses the
// standard Levenshtein recurrence (insertions, deletions, substitutions
// only — no transposition case).  Each DFA state maps to a unique DP
// column; transitions are precomputed for all 256 byte values.
//
// step(state, label) is a single array lookup — O(1) per FST node.
// Pruning uses a precomputed can_match vector (backward reachability
// from accepting states).
struct LevenshteinDFA {
    // Transition table: table[state][byte] -> next state.
    std::vector<std::array<uint32_t, 256>> table;

    // Per-state data.
    std::vector<bool> is_match_vec;   // Accepting states (distance <= max_dist)
    std::vector<bool> can_match_vec;  // Can reach an accepting state
    std::vector<uint8_t> dist_vec;    // Edit distance at D(m,j) for this state

    uint32_t start_state_;
    uint32_t dead_state_;   // Sink: all transitions loop to self, not accepting

    // Build the DFA for the given query and max edit distance.
    // Returns Error::QueryTooLong if query.size() > 64.
    [[nodiscard]]
    std::expected<void, Error> init(std::string_view query,
                                     uint32_t max_distance);

    // Advance one character.  O(1) table lookup.
    uint32_t step(uint32_t state, uint8_t label) const {
        return table[state][label];
    }

    // Is this an accepting state?
    bool is_match(uint32_t state) const {
        return is_match_vec[state];
    }

    // Can an accepting state be reached from here?
    bool can_reach_accept(uint32_t state) const {
        return can_match_vec[state];
    }

    // Edit distance for an accepting state (D(m,j)).
    uint32_t distance(uint32_t state) const {
        return dist_vec[state];
    }

    uint32_t start_state() const { return start_state_; }
    uint32_t dead_state() const { return dead_state_; }
    uint32_t num_states() const { return static_cast<uint32_t>(table.size()); }
};

}  // namespace internal
}  // namespace fuzzyfst
