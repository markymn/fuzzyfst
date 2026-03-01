#pragma once

#include "trie_node.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Serialize a minimized trie to the binary FST format.
//
// Nodes are written in reverse topological order (leaves first, root last).
// Returns the serialized byte buffer (header + packed nodes).
std::vector<uint8_t> fst_serialize(const TrieNode* root,
                                    const std::vector<TrieNode*>& node_pool);

}  // namespace internal
}  // namespace fuzzyfst
