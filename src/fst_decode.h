#pragma once

// Shared FST node decoding used by both FuzzyIterator and DamerauIterator.

#include <cstdint>

namespace fuzzyfst {
namespace internal {

// Decode an FST node at `offset` in the serialized data.
// Sets `is_final`, `num_trans`, and `trans_ptr` (pointer to first transition).
// Each transition is 5 bytes: 1 byte label + 4 bytes child offset.
inline void read_node(const uint8_t* data, uint32_t offset,
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

}  // namespace internal
}  // namespace fuzzyfst
