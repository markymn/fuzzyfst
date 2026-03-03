#pragma once

#include "fst_reader.h"
#include "levenshtein_nfa.h"

#include <fuzzyfst/fuzzyfst.h>

#include <cstddef>
#include <cstdint>

namespace fuzzyfst {
namespace internal {

// Lazy FST x Levenshtein intersection iterator.
// Simultaneously walks the FST and the Levenshtein NFA, yielding all
// words within edit distance d of the query.
//
// Zero heap allocation: stack, result buffer, and word buffer are all
// caller-provided or fixed-size.
class FuzzyIterator {
public:
    FuzzyIterator(const FstReader& fst,
                  const LevenshteinNFA& nfa,
                  char* word_buf,
                  size_t word_buf_size,
                  FuzzyResult* result_buf,
                  size_t result_buf_cap);

    // Collect up to result_buf_cap results.  Returns count found.
    // Can be called repeatedly (resumes from where it left off).
    size_t collect();

    bool done() const { return done_; }

private:
    struct Frame {
        uint32_t          node_offset;
        LevenshteinState  lev_state;
        uint8_t           depth;
        uint8_t           num_transitions;
        uint8_t           next_trans_idx;
        uint8_t           _pad;
    };

    const FstReader&      fst_;
    const LevenshteinNFA& nfa_;
    char*                 word_buf_;       // Output: stores emitted result words
    size_t                word_buf_size_;
    size_t                word_buf_used_;  // Write cursor into word_buf
    FuzzyResult*          result_buf_;
    size_t                result_buf_cap_;

    char                  path_buf_[256];  // DFS scratch: current word path
    Frame                 stack_[256];
    uint16_t              stack_top_;
    bool                  done_;
};

}  // namespace internal
}  // namespace fuzzyfst
