#include "fuzzy_search.h"

#include <cassert>
#include <cstring>

namespace fuzzyfst {
namespace internal {

void FuzzyIterator::read_node(const uint8_t* data, uint32_t offset,
                               bool& is_final, uint8_t& num_trans,
                               const uint8_t*& trans_ptr) {
    const uint8_t* p = data + offset;
    uint8_t flags_and_count = *p++;
    is_final = (flags_and_count & 0x80) != 0;
    uint8_t count = flags_and_count & 0x3F;
    if (count == 63) {
        count = 63 + *p++;
    }
    num_trans = count;
    trans_ptr = p;
}

FuzzyIterator::FuzzyIterator(const FstReader& fst,
                              const LevenshteinNFA& nfa,
                              char* word_buf,
                              size_t word_buf_size,
                              FuzzyResult* result_buf,
                              size_t result_buf_cap)
    : fst_(fst), nfa_(nfa),
      word_buf_(word_buf), word_buf_size_(word_buf_size), word_buf_used_(0),
      result_buf_(result_buf), result_buf_cap_(result_buf_cap),
      stack_top_(0), done_(false) {
    // Read root node info.
    uint32_t root_off = fst.root_offset();
    bool root_final;
    uint8_t root_ntrans;
    const uint8_t* unused;
    read_node(fst.data(), root_off, root_final, root_ntrans, unused);

    LevenshteinState start = nfa.start_state();

    // Push root frame.
    Frame& f = stack_[0];
    f.node_offset = root_off;
    f.lev_state = start;
    f.depth = 0;
    f.num_transitions = root_ntrans;
    f.next_trans_idx = 0;
    stack_top_ = 1;
}

size_t FuzzyIterator::collect() {
    if (done_) return 0;

    const uint8_t* data = fst_.data();
    size_t count = 0;
    word_buf_used_ = 0;  // Reset for this batch — caller must consume before next call.

    while (stack_top_ > 0 && count < result_buf_cap_) {
        Frame& top = stack_[stack_top_ - 1];

        if (top.next_trans_idx >= top.num_transitions) {
            --stack_top_;
            continue;
        }

        // Read transitions at current node.
        bool is_final_unused;
        uint8_t num_trans_unused;
        const uint8_t* trans_ptr;
        read_node(data, top.node_offset, is_final_unused,
                  num_trans_unused, trans_ptr);

        // Advance to the current transition.
        trans_ptr += static_cast<size_t>(top.next_trans_idx) * 5;
        uint8_t label = *trans_ptr;
        uint32_t target_offset;
        std::memcpy(&target_offset, trans_ptr + 1, 4);

        top.next_trans_idx++;

        // Compute new Levenshtein state.
        LevenshteinState new_lev = LevenshteinNFA::step(
            top.lev_state, nfa_.char_mask[label], nfa_.query_len);

        // Prune if this state can't possibly match.
        if (!nfa_.can_match(new_lev)) {
            continue;
        }

        // Write label into DFS path buffer.
        uint8_t depth = top.depth;
        if (depth < 255) {
            path_buf_[depth] = static_cast<char>(label);
        }

        // Check if target node is final and this is a match.
        bool target_final;
        uint8_t target_ntrans;
        const uint8_t* target_trans;
        read_node(data, target_offset, target_final, target_ntrans, target_trans);

        if (target_final && nfa_.is_match(new_lev)) {
            // Copy the word from path_buf into word_buf for stable storage.
            size_t word_len = depth + 1;
            if (count < result_buf_cap_ &&
                word_buf_used_ + word_len <= word_buf_size_) {
                std::memcpy(word_buf_ + word_buf_used_, path_buf_, word_len);
                result_buf_[count].word = std::string_view(
                    word_buf_ + word_buf_used_, word_len);
                result_buf_[count].distance = new_lev.dist;
                word_buf_used_ += word_len;
                ++count;
            }
        }

        // Push child frame to explore target's transitions.
        if (target_ntrans > 0 && stack_top_ < 256 && depth + 1 < 255) {
            Frame& child = stack_[stack_top_];
            child.node_offset = target_offset;
            child.lev_state = new_lev;
            child.depth = depth + 1;
            child.num_transitions = target_ntrans;
            child.next_trans_idx = 0;
            ++stack_top_;
        }
    }

    if (stack_top_ == 0) {
        done_ = true;
    }

    return count;
}

}  // namespace internal
}  // namespace fuzzyfst
