#include "state_map.h"

#include <cassert>
#include <cstring>

namespace fuzzyfst {
namespace internal {

StateMap::StateMap(size_t initial_capacity)
    : size_(0) {
    // Round up to power of two.
    capacity_ = 1;
    while (capacity_ < initial_capacity) {
        capacity_ <<= 1;
    }
    max_load_ = capacity_ * 4 / 5;  // 0.8 load factor
    slots_.resize(capacity_);
    std::memset(slots_.data(), 0, capacity_ * sizeof(Slot));
}

// FNV-1a style hash: one multiply per input element, final avalanche.
uint32_t StateMap::hash_node(const TrieNode* node) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

    h ^= static_cast<uint64_t>(node->is_final);
    h *= FNV_PRIME;
    h ^= static_cast<uint64_t>(node->num_transitions);
    h *= FNV_PRIME;
    for (uint8_t i = 0; i < node->num_transitions; ++i) {
        h ^= static_cast<uint64_t>(node->transitions[i].label);
        h *= FNV_PRIME;
        h ^= static_cast<uint64_t>(node->transitions[i].child_idx);
        h *= FNV_PRIME;
    }

    // Final avalanche to improve distribution in lower bits.
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;

    auto h32 = static_cast<uint32_t>(h);
    return h32 == 0 ? 1 : h32;
}

bool StateMap::nodes_equal(const TrieNode* a, const TrieNode* b) {
    if (a->is_final != b->is_final) return false;
    if (a->num_transitions != b->num_transitions) return false;
    for (uint8_t i = 0; i < a->num_transitions; ++i) {
        if (a->transitions[i].label != b->transitions[i].label) return false;
        if (a->transitions[i].child_idx != b->transitions[i].child_idx) return false;
    }
    return true;
}

uint32_t StateMap::find_or_insert(const TrieNode* node,
                                   TrieNode* const* node_pool) {
    if (size_ >= max_load_) {
        grow(node_pool);
    }

    uint32_t h = hash_node(node);
    size_t idx = h & (capacity_ - 1);
    uint16_t psl = 0;

    for (;;) {
        Slot& slot = slots_[idx];

        // Empty slot — insert here.
        if (slot.hash == 0) {
            slot.hash = h;
            slot.node_id = node->id;
            slot.psl = psl;
            ++size_;
            return node->id;
        }

        // Check for match: same hash, then full equality.
        if (slot.hash == h) {
            const TrieNode* existing = node_pool[slot.node_id];
            if (nodes_equal(node, existing)) {
                return slot.node_id;  // Dedup: return existing canonical ID.
            }
        }

        // Robin Hood: if current slot's PSL < ours, steal it.
        if (slot.psl < psl) {
            // Swap our entry into this slot, then continue inserting
            // the displaced entry.
            Slot displaced = slot;
            slot.hash = h;
            slot.node_id = node->id;
            slot.psl = psl;

            // Now re-insert the displaced entry.
            h = displaced.hash;
            // We need to continue probing for the displaced entry.
            // Its new PSL starts at displaced.psl + 1.
            psl = displaced.psl;

            // We'll use a separate loop to place the displaced node_id.
            // But we need to track the displaced node_id too.
            uint32_t displaced_id = displaced.node_id;

            // Continue probing to place the displaced entry.
            for (;;) {
                idx = (idx + 1) & (capacity_ - 1);
                ++psl;
                Slot& s = slots_[idx];

                if (s.hash == 0) {
                    s.hash = h;
                    s.node_id = displaced_id;
                    s.psl = psl;
                    ++size_;
                    return node->id;  // Original insert succeeded.
                }

                if (s.psl < psl) {
                    Slot tmp = s;
                    s.hash = h;
                    s.node_id = displaced_id;
                    s.psl = psl;
                    h = tmp.hash;
                    displaced_id = tmp.node_id;
                    psl = tmp.psl;
                }
            }
        }

        idx = (idx + 1) & (capacity_ - 1);
        ++psl;
    }
}

void StateMap::grow(TrieNode* const* node_pool) {
    size_t new_cap = capacity_ * 2;
    std::vector<Slot> new_slots(new_cap);
    std::memset(new_slots.data(), 0, new_cap * sizeof(Slot));

    size_t mask = new_cap - 1;

    // Re-insert all existing entries.
    for (size_t i = 0; i < capacity_; ++i) {
        Slot& old = slots_[i];
        if (old.hash == 0) continue;

        size_t idx = old.hash & mask;
        uint16_t psl = 0;
        uint32_t h = old.hash;
        uint32_t nid = old.node_id;

        for (;;) {
            Slot& s = new_slots[idx];
            if (s.hash == 0) {
                s.hash = h;
                s.node_id = nid;
                s.psl = psl;
                break;
            }
            if (s.psl < psl) {
                Slot tmp = s;
                s.hash = h;
                s.node_id = nid;
                s.psl = psl;
                h = tmp.hash;
                nid = tmp.node_id;
                psl = tmp.psl;
            }
            idx = (idx + 1) & mask;
            ++psl;
        }
    }

    slots_ = std::move(new_slots);
    capacity_ = new_cap;
    max_load_ = capacity_ * 4 / 5;
}

}  // namespace internal
}  // namespace fuzzyfst
