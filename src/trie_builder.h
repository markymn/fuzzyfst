#pragma once

#include "arena.h"
#include "state_map.h"
#include "trie_node.h"

#include <fuzzyfst/fuzzyfst.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Implements Daciuk's incremental minimization algorithm in a single
// pass over sorted input.
//
// Thread safety: NOT thread-safe.  Used only in single-threaded build path.
class FstBuilder {
public:
    explicit FstBuilder(Arena& arena);

    // Add a word.  Words MUST be added in strict lexicographic order.
    // Returns Error::InputNotSorted if `word` <= prev_word_.
    // This is a runtime check, NOT a debug assert — it fires in release builds.
    [[nodiscard]] std::expected<void, Error> add(std::string_view word);

    // Finalize: freeze the last path, minimize remaining suffixes.
    // After this call, the trie is fully minimized and ready for serialization.
    [[nodiscard]] std::expected<void, Error> finish();

    // Access the minimized trie (valid only after finish()).
    TrieNode* root() const { return root_; }
    const std::vector<TrieNode*>& node_pool() const { return node_pool_; }
    size_t unique_node_count() const { return state_map_.size(); }

private:
    Arena&                  arena_;
    StateMap                state_map_;
    std::vector<TrieNode*>  temp_path_;     // Current "last word" path
    std::vector<TrieNode*>  node_pool_;     // All allocated nodes, indexed by ID
    std::string             prev_word_;     // Previous word (for sort check)
    TrieNode*               root_;          // Root node (set after finish)
    bool                    finished_;

    // Minimize (freeze + dedup) nodes on temp_path_ from the end
    // down to index `down_to`.
    void minimize(size_t down_to);

    // Allocate a fresh trie node from the arena.
    TrieNode* alloc_node();

    // Add a transition from `parent` to a new child with the given label.
    // Returns the new child node.
    TrieNode* add_child(TrieNode* parent, uint8_t label);
};

}  // namespace internal
}  // namespace fuzzyfst
