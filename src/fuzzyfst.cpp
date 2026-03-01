// fuzzyfst.cpp — Public API implementation (wraps internal types via Pimpl)

#include <fuzzyfst/fuzzyfst.h>

#include "arena.h"
#include "radix_sort.h"
#include "trie_builder.h"
#include "fst_writer.h"
#include "fst_reader.h"
#include "levenshtein_nfa.h"
#include "fuzzy_search.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace fuzzyfst {

// ── build() ─────────────────────────────────────────────────

std::expected<void, Error> build(const char* output_path,
                                  std::span<std::string_view> words,
                                  BuildOptions opts) {
    // Sort if requested.
    if (opts.sort_input && words.size() > 1) {
        std::vector<std::string_view> scratch(words.size());
        internal::radix_sort(words.data(), words.size(), scratch.data());
    }

    // Deduplicate.
    auto end = std::unique(words.begin(), words.end());
    size_t n = static_cast<size_t>(end - words.begin());

    // Build trie.
    internal::Arena arena;
    internal::FstBuilder builder(arena);
    for (size_t i = 0; i < n; ++i) {
        auto r = builder.add(words[i]);
        if (!r.has_value()) return std::unexpected(r.error());
    }
    auto r = builder.finish();
    if (!r.has_value()) return std::unexpected(r.error());

    // Serialize.
    auto bytes = internal::fst_serialize(builder.root(), builder.node_pool());

    // Write to file.
    FILE* f = std::fopen(output_path, "wb");
    if (!f) return std::unexpected(Error::FileOpenFailed);
    size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (written != bytes.size()) return std::unexpected(Error::FileOpenFailed);

    return {};
}

// ── Fst (Pimpl) ─────────────────────────────────────────────

struct Fst::Impl {
    internal::FstReader reader;

    explicit Impl(internal::FstReader&& r) : reader(std::move(r)) {}
};

std::expected<Fst, Error> Fst::open(const char* path) {
    auto reader = internal::FstReader::open(path);
    if (!reader.has_value()) return std::unexpected(reader.error());

    Fst fst;
    fst.impl_ = new Impl(std::move(*reader));
    return fst;
}

Fst::~Fst() {
    delete impl_;
}

Fst::Fst(Fst&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Fst& Fst::operator=(Fst&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool Fst::contains(std::string_view key) const {
    assert(impl_);
    return impl_->reader.contains(key);
}

size_t Fst::fuzzy_search(std::string_view query,
                          uint32_t max_distance,
                          std::span<FuzzyResult> results,
                          std::span<char> word_buf) const {
    assert(impl_);
    if (query.size() > 64) return 0;

    internal::LevenshteinNFA nfa;
    auto r = nfa.init(query, max_distance);
    if (!r.has_value()) return 0;

    internal::FuzzyIterator iter(impl_->reader, nfa,
                                  word_buf.data(), word_buf.size(),
                                  results.data(), results.size());
    size_t total = 0;
    while (!iter.done() && total < results.size()) {
        size_t n = iter.collect();
        total += n;
    }
    return total;
}

std::vector<FuzzyResult> Fst::fuzzy_search(std::string_view query,
                                             uint32_t max_distance) const {
    assert(impl_);
    if (query.size() > 64) return {};

    // Thread-local backing store for result word data.
    // Invalidated on each call — caller must consume before next call.
    thread_local std::string word_backing;
    word_backing.clear();

    std::vector<char> word_buf(65536);
    std::vector<FuzzyResult> result_buf(8192);
    std::vector<std::pair<size_t, size_t>> offsets;  // (offset, length)
    std::vector<uint32_t> distances;

    internal::LevenshteinNFA nfa;
    auto r = nfa.init(query, max_distance);
    if (!r.has_value()) return {};

    internal::FuzzyIterator iter(impl_->reader, nfa,
                                  word_buf.data(), word_buf.size(),
                                  result_buf.data(), result_buf.size());

    while (!iter.done()) {
        size_t n = iter.collect();
        for (size_t i = 0; i < n; ++i) {
            offsets.push_back({word_backing.size(),
                               result_buf[i].word.size()});
            word_backing.append(result_buf[i].word);
            distances.push_back(result_buf[i].distance);
        }
    }

    // Build result vector with string_views into the thread_local buffer.
    std::vector<FuzzyResult> results;
    results.reserve(offsets.size());
    const char* base = word_backing.data();
    for (size_t i = 0; i < offsets.size(); ++i) {
        results.push_back({std::string_view(base + offsets[i].first,
                                             offsets[i].second),
                           distances[i]});
    }
    return results;
}

uint32_t Fst::num_nodes() const {
    assert(impl_);
    return impl_->reader.num_nodes();
}

}  // namespace fuzzyfst
