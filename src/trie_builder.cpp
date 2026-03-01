#include "trie_builder.h"

#include <cassert>
#include <cstring>

namespace fuzzyfst {
namespace internal {

FstBuilder::FstBuilder(Arena& arena)
    : arena_(arena),
      state_map_(1 << 18),
      root_(nullptr),
      finished_(false) {
    node_pool_.reserve(1 << 20);  // Pre-reserve for ~1M nodes.
    // Allocate the root node.
    temp_path_.push_back(alloc_node());
}

TrieNode* FstBuilder::alloc_node() {
    auto* node = arena_.create<TrieNode>();
    node->transitions = nullptr;
    node->num_transitions = 0;
    node->is_final = false;
    node->_pad = 0;
    node->hash_cache = 0;
    node->id = static_cast<uint32_t>(node_pool_.size());
    node_pool_.push_back(node);
    return node;
}

TrieNode* FstBuilder::add_child(TrieNode* parent, uint8_t label) {
    TrieNode* child = alloc_node();

    // Grow the transition array by 1.
    // Transitions are sorted by label — new labels are always appended
    // because words are added in sorted order (so labels at each depth
    // arrive in non-decreasing order for new suffixes).
    uint8_t old_count = parent->num_transitions;
    auto* new_trans = arena_.alloc_array<TrieNode::Trans>(old_count + 1);

    if (old_count > 0) {
        std::memcpy(new_trans, parent->transitions,
                    old_count * sizeof(TrieNode::Trans));
    }

    new_trans[old_count].label = label;
    std::memset(new_trans[old_count]._pad, 0, sizeof(new_trans[old_count]._pad));
    new_trans[old_count].child_idx = child->id;

    parent->transitions = new_trans;
    parent->num_transitions = old_count + 1;
    parent->hash_cache = 0;  // Invalidate hash.

    return child;
}

std::expected<void, Error> FstBuilder::add(std::string_view word) {
    assert(!finished_);

    // Check sorted order (runtime, not debug-only).
    if (!prev_word_.empty() && word <= prev_word_) {
        return std::unexpected(Error::InputNotSorted);
    }

    // Find longest common prefix with previous word.
    size_t prefix_len = 0;
    size_t max_prefix = std::min(word.size(), prev_word_.size());
    while (prefix_len < max_prefix &&
           word[prefix_len] == prev_word_[prefix_len]) {
        ++prefix_len;
    }

    // Minimize nodes below the common prefix — those suffixes are
    // finalized and can be deduplicated.
    minimize(prefix_len);

    // Extend the path with remaining characters of the new word.
    // temp_path_ currently has prefix_len + 1 nodes (root + prefix nodes).
    // We need to add nodes for word[prefix_len..].
    TrieNode* current = temp_path_[prefix_len];

    for (size_t i = prefix_len; i < word.size(); ++i) {
        uint8_t c = static_cast<uint8_t>(word[i]);
        TrieNode* child = add_child(current, c);
        temp_path_.resize(i + 2);  // Ensure space for depth i+1
        temp_path_[i + 1] = child;
        current = child;
    }

    // Mark the last node as final (this word ends here).
    current->is_final = true;

    prev_word_.assign(word.data(), word.size());
    return {};
}

void FstBuilder::minimize(size_t down_to) {
    // Process from the deepest node in temp_path_ back to down_to.
    // Each node's children have already been minimized (bottom-up order).
    for (size_t i = temp_path_.size() - 1; i > down_to; --i) {
        TrieNode* child = temp_path_[i];
        TrieNode* parent = temp_path_[i - 1];

        // Try to find a structurally equivalent node in the state map.
        uint32_t canonical_id = state_map_.find_or_insert(
            child, node_pool_.data());

        if (canonical_id != child->id) {
            // Dedup: replace the parent's last transition target with
            // the canonical node.
            assert(parent->num_transitions > 0);
            parent->transitions[parent->num_transitions - 1].child_idx =
                canonical_id;
            parent->hash_cache = 0;  // Invalidate parent's hash.
        }
    }

    // Trim temp_path_ to keep only down_to + 1 nodes.
    temp_path_.resize(down_to + 1);
}

std::expected<void, Error> FstBuilder::finish() {
    assert(!finished_);

    // Minimize the entire remaining path from root.
    minimize(0);

    // The root itself also needs to be registered/deduped,
    // though for the root it's mainly for consistent node counting.
    state_map_.find_or_insert(temp_path_[0], node_pool_.data());

    root_ = temp_path_[0];
    finished_ = true;
    return {};
}

}  // namespace internal
}  // namespace fuzzyfst
