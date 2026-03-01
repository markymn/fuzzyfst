#include "fst_writer.h"

#include <cassert>
#include <cstring>
#include <vector>

namespace fuzzyfst {
namespace internal {

// Binary format constants
static constexpr uint32_t FST_MAGIC   = 0x46535431;  // "FST1"
static constexpr uint32_t FST_VERSION = 1;
static constexpr size_t   HEADER_SIZE = 64;

// Header layout (64 bytes total)
struct FstHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t num_nodes;
    uint32_t num_transitions;
    uint64_t data_size;
    uint32_t root_offset;
    uint32_t flags;
    uint8_t  _reserved[32];
};
static_assert(sizeof(FstHeader) == 64, "Header must be exactly 64 bytes");

std::vector<uint8_t> fst_serialize(const TrieNode* root,
                                    const std::vector<TrieNode*>& node_pool) {
    // Phase 1: Compute reverse topological order via iterative post-order DFS.
    // Children are written before parents. Deduplicated nodes (multiple parents
    // referencing the same canonical child) are visited only once.
    std::vector<uint32_t> order;  // Node IDs in write order.
    order.reserve(node_pool.size());

    {
        // Flat bitset for O(1) visited checks (no heap allocation per insert).
        std::vector<bool> visited(node_pool.size(), false);
        // Iterative post-order DFS using a flat vector as stack.
        struct Frame { uint32_t id; bool processed; };
        std::vector<Frame> stk;
        stk.reserve(256);
        stk.push_back({root->id, false});

        while (!stk.empty()) {
            auto& top = stk.back();
            if (top.processed) {
                order.push_back(top.id);
                stk.pop_back();
                continue;
            }
            top.processed = true;

            if (visited[top.id]) {
                stk.pop_back();
                continue;
            }
            visited[top.id] = true;

            const TrieNode* node = node_pool[top.id];
            // Push children in reverse order so leftmost child is processed first.
            for (int i = static_cast<int>(node->num_transitions) - 1; i >= 0; --i) {
                uint32_t child_id = node->transitions[i].child_idx;
                if (!visited[child_id]) {
                    stk.push_back({child_id, false});
                }
            }
        }
    }

    // Phase 2: Compute byte offset for each node.
    // Map node_id → byte offset within the data section (after header).
    std::vector<uint32_t> offsets(node_pool.size(), UINT32_MAX);
    uint32_t write_pos = 0;
    uint32_t total_nodes = 0;
    uint32_t total_transitions = 0;

    for (uint32_t id : order) {
        const TrieNode* node = node_pool[id];
        offsets[id] = write_pos;

        // Node size: 1 byte header + optional 1 byte count_ext + num_trans * 5
        uint8_t n = node->num_transitions;
        size_t node_size = 1;  // flags_and_count
        if (n >= 63) {
            node_size += 1;  // count_ext
        }
        node_size += static_cast<size_t>(n) * 5;

        write_pos += static_cast<uint32_t>(node_size);
        ++total_nodes;
        total_transitions += n;
    }

    // Phase 3: Write the binary data.
    size_t data_size = write_pos;
    std::vector<uint8_t> buf(HEADER_SIZE + data_size, 0);

    // Write header.
    FstHeader header{};
    header.magic = FST_MAGIC;
    header.version = FST_VERSION;
    header.num_nodes = total_nodes;
    header.num_transitions = total_transitions;
    header.data_size = data_size;
    header.root_offset = offsets[root->id];
    header.flags = 0;
    std::memset(header._reserved, 0, sizeof(header._reserved));
    std::memcpy(buf.data(), &header, sizeof(header));

    // Write nodes in order.
    for (uint32_t id : order) {
        const TrieNode* node = node_pool[id];
        uint8_t* dst = buf.data() + HEADER_SIZE + offsets[id];

        // flags_and_count byte
        uint8_t flags_and_count = 0;
        if (node->is_final) {
            flags_and_count |= 0x80;  // bit 7
        }
        uint8_t n = node->num_transitions;
        if (n < 63) {
            flags_and_count |= n;
            *dst++ = flags_and_count;
        } else {
            flags_and_count |= 63;  // escape value
            *dst++ = flags_and_count;
            *dst++ = static_cast<uint8_t>(n - 63);  // count_ext
        }

        // Transitions: 5 bytes each (1 label + 4 target_offset)
        for (uint8_t i = 0; i < n; ++i) {
            uint8_t label = node->transitions[i].label;
            uint32_t child_id = node->transitions[i].child_idx;
            assert(offsets[child_id] != UINT32_MAX);
            uint32_t target_offset = offsets[child_id];

            *dst++ = label;
            std::memcpy(dst, &target_offset, 4);
            dst += 4;
        }
    }

    return buf;
}

}  // namespace internal
}  // namespace fuzzyfst
