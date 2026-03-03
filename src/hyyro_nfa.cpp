#include "hyyro_nfa.h"

#include <cstring>

namespace fuzzyfst {
namespace internal {

std::expected<void, Error> HyyroNFA::init(std::string_view query,
                                           uint32_t max_distance) {
    if (query.size() > 64) {
        return std::unexpected(Error::QueryTooLong);
    }

    query_len = static_cast<uint32_t>(query.size());
    max_dist = max_distance;

    // Build char_mask: for each byte value, set bit i if query[i] == c.
    // Identical to Myers' precomputation.
    std::memset(char_mask, 0, sizeof(char_mask));
    for (uint32_t i = 0; i < query_len; ++i) {
        uint8_t c = static_cast<uint8_t>(query[i]);
        char_mask[c] |= (1ULL << i);
    }

    return {};
}

HyyroState HyyroNFA::start_state() const {
    // Initial state: same as Myers' with Xv = 0 (no previous Eq).
    // D[i] = i for i in [0..m], so Pv = all 1s, Mv = 0, dist = m.
    HyyroState s;
    if (query_len == 0) {
        s.Pv = 0;
    } else if (query_len == 64) {
        s.Pv = ~0ULL;
    } else {
        s.Pv = (1ULL << query_len) - 1;
    }
    s.Mv = 0;
    s.Xv = 0;
    s.dist = query_len;
    return s;
}

HyyroState HyyroNFA::step(const HyyroState& state,
                           uint64_t eq_mask,
                           uint32_t m) {
    // Empty query: every target character is an insertion (+1 dist).
    if (m == 0) {
        return {0, 0, 0, state.dist + 1};
    }

    uint64_t Pv = state.Pv;
    uint64_t Mv = state.Mv;
    uint64_t Eq = eq_mask;

    // Hyyro's Damerau-Levenshtein bit-parallel algorithm.
    //
    // First compute Myers' standard Levenshtein column, then fold in
    // the transposition improvement.  The transposition row uses the
    // previous column's Eq mask (state.Xv) to detect positions where
    // query[i]==target[j-1] AND query[i-1]==target[j], which corresponds
    // to a swap of two adjacent characters.
    //
    // The transposition bitvector Tb marks positions where a transposition
    // is possible: both the current Eq and the previous Eq (shifted by 1)
    // agree on swapped positions.

    // Standard Myers' computation (Levenshtein part):
    uint64_t Xv = Eq | Mv;
    uint64_t Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;
    uint64_t Ph = Mv | ~(Xh | Pv);
    uint64_t Mh = Pv & Xh;

    // Transposition detection:
    // Tb[i] is set when query[i] matches target[j-1] (state.Xv bit i)
    // AND query[i-1] matches target[j] (Eq bit i-1, i.e. (Eq << 1) bit i).
    // This means swapping target[j-1] and target[j] would match
    // query[i-1..i], costing 1 edit instead of 2 substitutions.
    //
    // When a transposition is possible at position i, the horizontal
    // delta at i can be improved: it becomes at most +1 from the
    // D[i-2][j-2] diagonal (the transposition cost).  In bitvector
    // terms, we clear Ph bits where transposition helps (making the
    // horizontal delta 0 or negative instead of positive).
    uint64_t Tb = (state.Xv >> 1) & Eq;

    // A transposition at position i means D[i][j] = D[i-2][j-2] + 1.
    // In delta terms, this improves the result when Ph is set at position i.
    // We clear Ph where transposition helps and set Mh there instead
    // (unless Mh is already set).
    Ph = Ph & ~Tb;
    Mh = Mh | Tb;

    // Update dist based on the last row bit.
    uint32_t dist = state.dist;
    uint64_t last_bit = 1ULL << (m - 1);
    if (Ph & last_bit) dist += 1;
    if (Mh & last_bit) dist -= 1;

    // Shift for next column.
    Ph <<= 1;
    Mh <<= 1;
    Ph |= 1;  // Insertion edit: bit 0 of Ph is set.

    uint64_t new_Pv = Mh | ~(Xv | Ph);
    uint64_t new_Mv = Ph & Xv;

    // Mask to query_len bits.
    uint64_t mask = (m == 64) ? ~0ULL : (1ULL << m) - 1;
    new_Pv &= mask;
    new_Mv &= mask;

    return {new_Pv, new_Mv, Eq, dist};
}

bool HyyroNFA::can_match(const HyyroState& state) const {
    // Same logic as LevenshteinNFA::can_match — reconstruct the full
    // DP column from the bitvector deltas and check if any position
    // is within max_dist.
    if (state.dist <= max_dist) return true;

    uint32_t val = state.dist;
    uint64_t pv = state.Pv;
    uint64_t mv = state.Mv;

    for (uint32_t j = query_len; j > 0; --j) {
        uint32_t bit = j - 1;
        if (pv & (1ULL << bit)) {
            if (--val <= max_dist) return true;
        } else if (mv & (1ULL << bit)) {
            val++;
        }
    }

    return false;
}

bool HyyroNFA::is_match(const HyyroState& state) const {
    return state.dist <= max_dist;
}

}  // namespace internal
}  // namespace fuzzyfst
