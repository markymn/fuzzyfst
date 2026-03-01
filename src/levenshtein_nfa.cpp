#include "levenshtein_nfa.h"

#include <cstring>

namespace fuzzyfst {
namespace internal {

std::expected<void, Error> LevenshteinNFA::init(std::string_view query,
                                                 uint32_t max_distance) {
    if (query.size() > 64) {
        return std::unexpected(Error::QueryTooLong);
    }

    query_len = static_cast<uint32_t>(query.size());
    max_dist = max_distance;

    // Build char_mask: for each byte value, set bit i if query[i] == c.
    std::memset(char_mask, 0, sizeof(char_mask));
    for (uint32_t i = 0; i < query_len; ++i) {
        uint8_t c = static_cast<uint8_t>(query[i]);
        char_mask[c] |= (1ULL << i);
    }

    return {};
}

LevenshteinState LevenshteinNFA::start_state() const {
    // Initial state: D[i] = i for i in [0..m].
    // Deltas: D[i] - D[i-1] = 1 for all i, so Pv = all 1s, Mv = 0.
    // dist = D[m] = m (all characters unmatched).
    LevenshteinState s;
    if (query_len == 0) {
        s.Pv = 0;
    } else if (query_len == 64) {
        s.Pv = ~0ULL;
    } else {
        s.Pv = (1ULL << query_len) - 1;
    }
    s.Mv = 0;
    s.dist = query_len;
    return s;
}

LevenshteinState LevenshteinNFA::step(const LevenshteinState& state,
                                       uint64_t eq_mask,
                                       uint32_t m) {
    // Empty query: every target character is an insertion (+1 dist).
    if (m == 0) {
        return {0, 0, state.dist + 1};
    }

    // Myers' bit-parallel algorithm.
    uint64_t Pv = state.Pv;
    uint64_t Mv = state.Mv;
    uint64_t Eq = eq_mask;

    uint64_t Xv = Eq | Mv;
    uint64_t Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq;
    uint64_t Ph = Mv | ~(Xh | Pv);
    uint64_t Mh = Pv & Xh;

    // Update dist based on the last row bit.
    uint32_t dist = state.dist;
    uint64_t last_bit = 1ULL << (m - 1);
    if (Ph & last_bit) dist += 1;
    if (Mh & last_bit) dist -= 1;

    // Shift for next column.
    Ph <<= 1;
    Mh <<= 1;
    // The insertion edit: bit 0 of Ph is set (cost +1 for inserting).
    Ph |= 1;

    uint64_t new_Pv = Mh | ~(Xv | Ph);
    uint64_t new_Mv = Ph & Xv;

    // Mask to query_len bits.
    uint64_t mask = (m == 64) ? ~0ULL : (1ULL << m) - 1;
    new_Pv &= mask;
    new_Mv &= mask;

    return {new_Pv, new_Mv, dist};
}

bool LevenshteinNFA::can_match(const LevenshteinState& state) const {
    // Fast path: if D[m] already within distance, no column scan needed.
    if (state.dist <= max_dist) return true;

    // Compute the minimum value in the implicit DP column.
    // D[m] = dist. Walking backwards:
    //   D[j] = D[j+1] - 1  if Pv bit j set  (delta = +1 means D[j+1] > D[j])
    //   D[j] = D[j+1] + 1  if Mv bit j set
    //   D[j] = D[j+1]      otherwise
    // We track the running value and minimum.
    uint32_t val = state.dist;
    uint32_t min_val = val;
    uint64_t pv = state.Pv;
    uint64_t mv = state.Mv;

    for (uint32_t j = query_len; j > 0; --j) {
        uint32_t bit = j - 1;
        if (pv & (1ULL << bit)) {
            if (--val <= max_dist) return true;  // Early exit.
        } else if (mv & (1ULL << bit)) {
            val++;
        }
        // No need to track min_val — early exit handles it.
    }

    return false;  // min_val > max_dist throughout.
}

bool LevenshteinNFA::is_match(const LevenshteinState& state) const {
    return state.dist <= max_dist;
}

}  // namespace internal
}  // namespace fuzzyfst
