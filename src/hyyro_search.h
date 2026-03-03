#pragma once

#include "fst_reader.h"
#include "hyyro_nfa.h"

#include <fuzzyfst/fuzzyfst.h>

#include <cstddef>
#include <cstdint>

namespace fuzzyfst {
namespace internal {

// FST x Hyyro Damerau-Levenshtein NFA intersection iterator.
// Same iterative DFS structure as FuzzyIterator and DamerauIterator.
// Frame carries HyyroState (28 bytes).
//
// Zero heap allocation: stack, result buffer, and word buffer are all
// caller-provided or fixed-size.
class HyyroIterator {
public:
    HyyroIterator(const FstReader& fst,
                  const HyyroNFA& nfa,
                  char* word_buf,
                  size_t word_buf_size,
                  FuzzyResult* result_buf,
                  size_t result_buf_cap);

    size_t collect();
    bool done() const { return done_; }

private:
    struct HyyroFrame {
        uint32_t    node_offset;
        HyyroState  hyyro_state;
        uint8_t     depth;
        uint8_t     num_transitions;
        uint8_t     next_trans_idx;
        uint8_t     _pad;
    };  // 36 bytes

    const FstReader&  fst_;
    const HyyroNFA&   nfa_;
    char*             word_buf_;
    size_t            word_buf_size_;
    size_t            word_buf_used_;
    FuzzyResult*      result_buf_;
    size_t            result_buf_cap_;

    char              path_buf_[256];
    HyyroFrame        stack_[256];
    uint16_t          stack_top_;
    bool              done_;
};

}  // namespace internal
}  // namespace fuzzyfst
