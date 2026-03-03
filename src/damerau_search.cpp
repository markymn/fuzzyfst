#include "damerau_search.h"
#include "fst_decode.h"

#include <cassert>
#include <cstring>

namespace fuzzyfst {
namespace internal {

DamerauIterator::DamerauIterator(const FstReader& fst,
                                  const DamerauNFA& dfa,
                                  char* word_buf,
                                  size_t word_buf_size,
                                  FuzzyResult* result_buf,
                                  size_t result_buf_cap)
    : fst_(fst), dfa_(dfa),
      word_buf_(word_buf), word_buf_size_(word_buf_size), word_buf_used_(0),
      result_buf_(result_buf), result_buf_cap_(result_buf_cap),
      stack_top_(0), done_(false) {
    uint32_t root_off = fst.root_offset();
    bool root_final;
    uint8_t root_ntrans;
    const uint8_t* unused;
    read_node(fst.data(), root_off, root_final, root_ntrans, unused);

    DamerauFrame& f = stack_[0];
    f.node_offset = root_off;
    f.dfa_state = dfa.start_state();
    f.depth = 0;
    f.num_transitions = root_ntrans;
    f.next_trans_idx = 0;
    stack_top_ = 1;
}

size_t DamerauIterator::collect() {
    if (done_) return 0;

    const uint8_t* data = fst_.data();
    size_t count = 0;
    word_buf_used_ = 0;

    while (stack_top_ > 0 && count < result_buf_cap_) {
        DamerauFrame& top = stack_[stack_top_ - 1];

        if (top.next_trans_idx >= top.num_transitions) {
            --stack_top_;
            continue;
        }

        bool is_final_unused;
        uint8_t num_trans_unused;
        const uint8_t* trans_ptr;
        read_node(data, top.node_offset, is_final_unused,
                  num_trans_unused, trans_ptr);

        trans_ptr += static_cast<size_t>(top.next_trans_idx) * 5;
        uint8_t label = *trans_ptr;
        uint32_t target_offset;
        std::memcpy(&target_offset, trans_ptr + 1, 4);

        top.next_trans_idx++;

        // DFA transition: O(1) table lookup.
        uint32_t new_dfa_state = dfa_.step(top.dfa_state, label);

        // Prune if this state can't reach any accepting state.
        if (!dfa_.can_reach_accept(new_dfa_state)) {
            continue;
        }

        uint8_t depth = top.depth;
        if (depth < 255) {
            path_buf_[depth] = static_cast<char>(label);
        }

        bool target_final;
        uint8_t target_ntrans;
        const uint8_t* target_trans;
        read_node(data, target_offset, target_final, target_ntrans, target_trans);

        if (target_final && dfa_.is_match(new_dfa_state)) {
            size_t word_len = depth + 1;
            if (count < result_buf_cap_ &&
                word_buf_used_ + word_len <= word_buf_size_) {
                std::memcpy(word_buf_ + word_buf_used_, path_buf_, word_len);
                result_buf_[count].word = std::string_view(
                    word_buf_ + word_buf_used_, word_len);
                result_buf_[count].distance = dfa_.distance(new_dfa_state);
                word_buf_used_ += word_len;
                ++count;
            }
        }

        if (target_ntrans > 0 && stack_top_ < 256 && depth + 1 < 255) {
            DamerauFrame& child = stack_[stack_top_];
            child.node_offset = target_offset;
            child.dfa_state = new_dfa_state;
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
