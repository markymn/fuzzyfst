#pragma once

#include "fst_reader.h"
#include "damerau_nfa.h"

#include <fuzzyfst/fuzzyfst.h>

#include <cstddef>
#include <cstdint>

namespace fuzzyfst {
namespace internal {

// FST x Damerau-Levenshtein DFA intersection iterator.
// Same iterative DFS as FuzzyIterator but uses the compiled DFA
// (DamerauNFA) instead of the bit-parallel Levenshtein NFA.
//
// Zero heap allocation: stack, result buffer, and word buffer are all
// caller-provided or fixed-size.
class DamerauIterator {
public:
    DamerauIterator(const FstReader& fst,
                    const DamerauNFA& dfa,
                    char* word_buf,
                    size_t word_buf_size,
                    FuzzyResult* result_buf,
                    size_t result_buf_cap);

    size_t collect();
    bool done() const { return done_; }

private:
    struct DamerauFrame {
        uint32_t node_offset;
        uint32_t dfa_state;
        uint8_t  depth;
        uint8_t  num_transitions;
        uint8_t  next_trans_idx;
        uint8_t  _pad;
    };  // 12 bytes

    const FstReader&  fst_;
    const DamerauNFA& dfa_;
    char*             word_buf_;
    size_t            word_buf_size_;
    size_t            word_buf_used_;
    FuzzyResult*      result_buf_;
    size_t            result_buf_cap_;

    char              path_buf_[256];
    DamerauFrame      stack_[256];
    uint16_t          stack_top_;
    bool              done_;
};

}  // namespace internal
}  // namespace fuzzyfst
