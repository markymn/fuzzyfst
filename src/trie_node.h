#pragma once

#include <cstdint>

namespace fuzzyfst {
namespace internal {

// Intermediate trie node — used only during construction.
// Destroyed after serialization to the binary FST format.
struct TrieNode {
    struct Trans {
        uint8_t   label;          //  1 byte  — transition character
        uint8_t   _pad[3];        //  3 bytes — padding
        uint32_t  child_idx;      //  4 bytes — index into node pool
    };  // 8 bytes, fits in a register

    Trans*    transitions;        // Pointer into arena (contiguous array)
    uint8_t   num_transitions;    // 0-255 outgoing edges
    bool      is_final;           // Accepts a word
    uint16_t  _pad;
    uint32_t  hash_cache;         // Cached hash for dedup (0 = not yet computed)
    uint32_t  id;                 // Unique node id (index in node pool)
};  // 24 bytes total with packing

}  // namespace internal
}  // namespace fuzzyfst
