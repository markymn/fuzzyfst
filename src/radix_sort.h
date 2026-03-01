#pragma once

#include <string_view>
#include <cstddef>

namespace fuzzyfst {
namespace internal {

// Sort an array of string_views in-place using MSD radix sort.
// `data` is the array to sort, with `count` elements.
// `scratch` is a pre-allocated buffer of at least `count` elements
// used for copying during bucket distribution.
//
// This function performs ZERO heap allocations. All sorting is
// done using the provided arrays and O(max_word_length) stack space.
void radix_sort(std::string_view* data, size_t count, std::string_view* scratch);

}  // namespace internal
}  // namespace fuzzyfst
