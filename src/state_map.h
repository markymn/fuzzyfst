#pragma once

#include "trie_node.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Open-addressing Robin Hood hash map for Daciuk state deduplication.
//
// Maps structural node signatures → canonical node IDs.
// Two nodes are "equal" if they have the same is_final flag,
// the same number of transitions, and identical (label, child_id)
// pairs — where child_id is the post-minimization canonical ID.
//
// Thread safety: NOT thread-safe. Used only in single-threaded build path.
class StateMap {
public:
    explicit StateMap(size_t initial_capacity = 1 << 16);

    // Returns existing node ID if a structurally-equal node exists,
    // otherwise inserts `node` and returns its own ID.
    //
    // PRECONDITION: `node` must have all children already minimized
    // with stable canonical IDs assigned.
    uint32_t find_or_insert(const TrieNode* node,
                            TrieNode* const* node_pool);

    size_t size() const { return size_; }

private:
    struct Slot {
        uint32_t  hash;       // Full hash (0 = empty sentinel)
        uint32_t  node_id;    // Index into node pool
        uint16_t  psl;        // Probe sequence length
        uint16_t  _pad;
    };  // 12 bytes

    std::vector<Slot> slots_;
    size_t            size_;
    size_t            capacity_;      // Always a power of two
    size_t            max_load_;      // capacity_ * 0.8

    static uint32_t hash_node(const TrieNode* node);
    static bool nodes_equal(const TrieNode* a, const TrieNode* b);
    void grow(TrieNode* const* node_pool);
};

}  // namespace internal
}  // namespace fuzzyfst
