#include "radix_sort.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fuzzyfst {
namespace internal {

constexpr size_t INSERTION_SORT_THRESHOLD = 32;

// Standard insertion sort fallback for small partitions.
static void insertion_sort(std::string_view* data, size_t count, size_t depth) {
    for (size_t i = 1; i < count; ++i) {
        std::string_view key = data[i];
        size_t j = i;
        
        // Custom string_view comparison starting from `depth`.
        while (j > 0) {
            std::string_view prev = data[j - 1];
            
            // Compare suffixes
            std::string_view prev_suffix = prev.substr(std::min(prev.size(), depth));
            std::string_view key_suffix = key.substr(std::min(key.size(), depth));
            
            if (prev_suffix <= key_suffix) {
                break;
            }
            
            data[j] = prev;
            j--;
        }
        data[j] = key;
    }
}

// MSD Radix Sort recursive step.
// `data` is the current partition to sort.
// `scratch` is the corresponding scratch space region.
// `depth` is the character index we are currently bucketing by.
static void radix_sort_impl(std::string_view* data, size_t count, std::string_view* scratch, size_t depth) {
    if (count <= INSERTION_SORT_THRESHOLD) {
        insertion_sort(data, count, depth);
        return;
    }

    // 256 buckets for bytes plus 1 bucket for strings that end at this depth.
    // count[0] is for strings of length == depth.
    // count[c + 1] is for strings where char at `depth` is `c`.
    size_t counts[257] = {0};

    // 1. Count frequencies.
    for (size_t i = 0; i < count; ++i) {
        if (depth == data[i].size()) {
            counts[0]++;
        } else {
            uint8_t c = static_cast<uint8_t>(data[i][depth]);
            counts[c + 1]++;
        }
    }

    // 2. Compute prefix sums (offsets).
    size_t offsets[257];
    offsets[0] = 0;
    for (int i = 1; i < 257; ++i) {
        offsets[i] = offsets[i - 1] + counts[i - 1];
    }

    // Keep a copy of the offsets to know the exact boundaries for recursion.
    size_t boundaries[257];
    std::memcpy(boundaries, offsets, sizeof(offsets));

    // 3. Distribute to scratch array.
    for (size_t i = 0; i < count; ++i) {
        if (depth == data[i].size()) {
            scratch[offsets[0]++] = data[i];
        } else {
            uint8_t c = static_cast<uint8_t>(data[i][depth]);
            scratch[offsets[c + 1]++] = data[i];
        }
    }

    // 4. Copy back to original array.
    std::memcpy(data, scratch, count * sizeof(std::string_view));

    // 5. Recurse.
    // Bucket 0 (strings ending here) is fully sorted, no need to recurse.
    // For buckets 1..256, recurse if they have > 1 element.
    for (int i = 1; i < 257; ++i) {
        size_t bucket_size = counts[i];
        if (bucket_size > 1) {
            size_t bucket_start = boundaries[i];
            radix_sort_impl(data + bucket_start, bucket_size, scratch + bucket_start, depth + 1);
        }
    }
}

void radix_sort(std::string_view* data, size_t count, std::string_view* scratch) {
    if (count <= 1) return;
    radix_sort_impl(data, count, scratch, 0);
}

}  // namespace internal
}  // namespace fuzzyfst
