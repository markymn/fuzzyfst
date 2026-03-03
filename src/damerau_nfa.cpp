#include "damerau_nfa.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace fuzzyfst {
namespace internal {

// DFA state for Damerau-Levenshtein construction.
// Encodes two DP columns (current + previous) plus the last input byte.
// The previous column and prev_char are needed for the transposition
// recurrence: D(i,j) = D(i-2,j-2)+1 when query[i-1]==target[j-2]
// and query[i-2]==target[j-1].
struct DLFullState {
    std::vector<uint8_t> prev_col;  // D(i, j-1)
    std::vector<uint8_t> curr_col;  // D(i, j)
    uint8_t prev_char;              // target[j-1]

    bool operator==(const DLFullState& o) const {
        return prev_col == o.prev_col && curr_col == o.curr_col && prev_char == o.prev_char;
    }
};

struct DLFullStateHash {
    size_t operator()(const DLFullState& s) const {
        size_t h = 14695981039346656037ULL;
        for (uint8_t v : s.prev_col) {
            h ^= v; h *= 1099511628211ULL;
        }
        for (uint8_t v : s.curr_col) {
            h ^= v; h *= 1099511628211ULL;
        }
        h ^= s.prev_char;
        h *= 1099511628211ULL;
        return h;
    }
};

std::expected<void, Error> DamerauNFA::init(std::string_view query,
                                             uint32_t max_distance) {
    if (query.size() > 64) {
        return std::unexpected(Error::QueryTooLong);
    }

    const uint32_t m = static_cast<uint32_t>(query.size());
    const uint8_t clamp = static_cast<uint8_t>(std::min(max_distance + 1, 255u));

    uint8_t qchars[64];
    std::memcpy(qchars, query.data(), m);

    // Equivalence classes: each unique query byte is its own class;
    // all non-query bytes share one class (they never match any query
    // position and, as prev_char, never trigger transpositions).
    std::unordered_set<uint8_t> query_chars_set(qchars, qchars + m);

    std::vector<uint8_t> representatives;
    for (uint8_t c : query_chars_set) {
        representatives.push_back(c);
    }
    uint8_t other_rep = 0;
    for (int c = 0; c < 256; ++c) {
        if (!query_chars_set.count(static_cast<uint8_t>(c))) {
            other_rep = static_cast<uint8_t>(c);
            break;
        }
    }
    if (!query_chars_set.count(other_rep)) {
        representatives.push_back(other_rep);
    }

    // State enumeration via BFS.
    using StateMap = std::unordered_map<DLFullState, uint32_t, DLFullStateHash>;
    StateMap state_ids;
    std::vector<DLFullState> states;

    // State 0 = dead (absorbing sink).
    DLFullState dead;
    dead.prev_col.assign(m + 1, clamp);
    dead.curr_col.assign(m + 1, clamp);
    dead.prev_char = 0;
    state_ids[dead] = 0;
    states.push_back(dead);

    // Start state: D(i,0) = i, clamped.
    DLFullState start;
    start.prev_col.assign(m + 1, clamp);
    start.curr_col.resize(m + 1);
    for (uint32_t i = 0; i <= m; ++i) {
        start.curr_col[i] = static_cast<uint8_t>(std::min(i, (uint32_t)clamp));
    }
    start.prev_char = 0;

    uint32_t start_id;
    auto sit = state_ids.find(start);
    if (sit != state_ids.end()) {
        start_id = sit->second;
    } else {
        start_id = static_cast<uint32_t>(states.size());
        state_ids[start] = start_id;
        states.push_back(start);
    }

    // Temp transitions per state (only for representative bytes).
    struct TempTrans {
        std::vector<std::pair<uint8_t, uint32_t>> edges;
    };
    std::vector<TempTrans> temp_trans(states.size());

    // Compute next DFA state from (st, input byte c).
    auto compute_next = [&](const DLFullState& st, uint8_t c) -> DLFullState {
        DLFullState next;
        next.prev_col = st.curr_col;
        next.curr_col.resize(m + 1);
        next.prev_char = c;

        next.curr_col[0] = (st.curr_col[0] < clamp) ? st.curr_col[0] + 1 : clamp;

        for (uint32_t i = 1; i <= m; ++i) {
            uint8_t cost_sub = (qchars[i - 1] == c) ? 0 : 1;

            uint8_t del_cost = (next.curr_col[i - 1] < clamp) ? next.curr_col[i - 1] + 1 : clamp;
            uint8_t ins_cost = (st.curr_col[i] < clamp) ? st.curr_col[i] + 1 : clamp;
            uint8_t sub_cost = (st.curr_col[i - 1] < clamp) ? st.curr_col[i - 1] + cost_sub : clamp;

            uint8_t best = std::min({del_cost, ins_cost, sub_cost});

            // Transposition: swap query[i-2..i-1] matches target[j-1..j].
            if (i >= 2 && qchars[i - 1] == st.prev_char && qchars[i - 2] == c) {
                uint8_t trans_cost = (st.prev_col[i - 2] < clamp) ? st.prev_col[i - 2] + 1 : clamp;
                best = std::min(best, trans_cost);
            }

            next.curr_col[i] = std::min(best, clamp);
        }

        // Map to dead state if completely saturated.
        bool all_dead = true;
        for (uint32_t i = 0; i <= m; ++i) {
            if (next.curr_col[i] < clamp) { all_dead = false; break; }
        }
        if (all_dead) {
            bool prev_dead = true;
            for (uint32_t i = 0; i <= m; ++i) {
                if (next.prev_col[i] < clamp) { prev_dead = false; break; }
            }
            if (prev_dead) return dead;
        }

        return next;
    };

    std::queue<uint32_t> worklist;
    if (start_id != 0) worklist.push(start_id);

    while (!worklist.empty()) {
        uint32_t sid = worklist.front();
        worklist.pop();

        DLFullState st = states[sid];  // Copy — states vector may grow below.
        if (temp_trans.size() <= sid) temp_trans.resize(sid + 1);

        for (uint8_t c : representatives) {
            DLFullState next = compute_next(st, c);

            uint32_t next_id;
            auto nit = state_ids.find(next);
            if (nit != state_ids.end()) {
                next_id = nit->second;
            } else {
                next_id = static_cast<uint32_t>(states.size());
                state_ids[next] = next_id;
                states.push_back(next);
                temp_trans.resize(states.size());
                worklist.push(next_id);
            }

            temp_trans[sid].edges.push_back({c, next_id});
        }
    }

    // Build full 256-entry transition table.
    uint32_t num = static_cast<uint32_t>(states.size());
    table.resize(num);
    is_match_vec.resize(num);
    can_match_vec.resize(num);
    dist_vec.resize(num);

    table[0].fill(0);
    is_match_vec[0] = false;
    dist_vec[0] = clamp;

    for (uint32_t sid = 1; sid < num; ++sid) {
        table[sid].fill(0);
        for (auto [rep, target] : temp_trans[sid].edges) {
            if (query_chars_set.count(rep)) {
                table[sid][rep] = target;
            } else {
                for (int c = 0; c < 256; ++c) {
                    if (!query_chars_set.count(static_cast<uint8_t>(c))) {
                        table[sid][c] = target;
                    }
                }
            }
        }
    }

    // Accepting states and distance: D(m, j).
    for (uint32_t sid = 0; sid < num; ++sid) {
        uint8_t d = states[sid].curr_col[m];
        dist_vec[sid] = d;
        is_match_vec[sid] = d <= max_distance;
    }

    // Backward reachability from accepting states.
    std::vector<std::vector<uint32_t>> reverse_edges(num);
    for (uint32_t sid = 0; sid < num; ++sid) {
        for (int c = 0; c < 256; ++c) {
            uint32_t target = table[sid][c];
            if (target != sid && target != 0) {
                reverse_edges[target].push_back(sid);
            }
        }
    }

    std::vector<bool> reachable(num, false);
    std::queue<uint32_t> bfs;
    for (uint32_t sid = 0; sid < num; ++sid) {
        if (is_match_vec[sid]) {
            reachable[sid] = true;
            bfs.push(sid);
        }
    }
    while (!bfs.empty()) {
        uint32_t sid = bfs.front();
        bfs.pop();
        for (uint32_t pred : reverse_edges[sid]) {
            if (!reachable[pred]) {
                reachable[pred] = true;
                bfs.push(pred);
            }
        }
    }

    for (uint32_t sid = 0; sid < num; ++sid) {
        can_match_vec[sid] = reachable[sid];
    }

    start_state_ = start_id;
    dead_state_ = 0;

    return {};
}

}  // namespace internal
}  // namespace fuzzyfst
