#pragma once

#include <fuzzyfst/fuzzyfst.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace fuzzyfst {
namespace internal {

// Memory-map an FST file and provide zero-copy query access.
//
// Thread safety: all const methods are safe for concurrent use.
// The underlying mmap'd memory is read-only.
class FstReader {
public:
    // Open FST file via mmap.  Returns Error on failure.
    static std::expected<FstReader, Error> open(const char* path);

    // Open from an in-memory buffer (takes ownership via move).
    // Useful for testing without writing to disk.
    static std::expected<FstReader, Error> from_bytes(std::vector<uint8_t>&& data);

    ~FstReader();

    FstReader(FstReader&&) noexcept;
    FstReader& operator=(FstReader&&) noexcept;

    // Non-copyable.
    FstReader(const FstReader&) = delete;
    FstReader& operator=(const FstReader&) = delete;

    // Exact lookup: returns true if `key` is in the FST.
    bool contains(std::string_view key) const;

    // Access raw data for intersection iterator.
    const uint8_t* data() const { return base_ + 64; }  // Skip header
    uint32_t root_offset() const;
    uint32_t num_nodes() const;

private:
    FstReader() = default;

    const uint8_t* base_ = nullptr;       // Start of data (header or mmap base)
    size_t         total_size_ = 0;        // Total mapped/buffer size

    // Ownership mode: either mmap or owned buffer.
    bool owns_buffer_ = false;
    std::vector<uint8_t> buffer_;          // Owned buffer (from_bytes mode)

#ifdef _WIN32
    void*          file_handle_ = nullptr;
    void*          map_handle_ = nullptr;
#else
    int            fd_ = -1;
#endif

    void cleanup();
};

}  // namespace internal
}  // namespace fuzzyfst
