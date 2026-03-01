#include "fst_reader.h"

#include <cassert>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fuzzyfst {
namespace internal {

// Binary format constants (must match fst_writer.cpp)
static constexpr uint32_t FST_MAGIC   = 0x46535431;
static constexpr size_t   HEADER_SIZE = 64;

// Read a uint32_t from the header at a given byte offset.
static uint32_t read_u32(const uint8_t* base, size_t offset) {
    uint32_t v;
    std::memcpy(&v, base + offset, 4);
    return v;
}

static uint64_t read_u64(const uint8_t* base, size_t offset) {
    uint64_t v;
    std::memcpy(&v, base + offset, 8);
    return v;
}

// Validate the FST header.
static bool validate_header(const uint8_t* base, size_t total_size) {
    if (total_size < HEADER_SIZE) return false;
    if (read_u32(base, 0) != FST_MAGIC) return false;
    uint64_t data_size = read_u64(base, 0x10);
    if (HEADER_SIZE + data_size > total_size) return false;
    return true;
}

// ── from_bytes (in-memory, for testing) ──────────────────────

std::expected<FstReader, Error> FstReader::from_bytes(std::vector<uint8_t>&& data) {
    if (!validate_header(data.data(), data.size())) {
        return std::unexpected(Error::InvalidFormat);
    }

    FstReader reader;
    reader.buffer_ = std::move(data);
    reader.base_ = reader.buffer_.data();
    reader.total_size_ = reader.buffer_.size();
    reader.owns_buffer_ = true;
    return reader;
}

// ── open (mmap) ──────────────────────────────────────────────

#ifdef _WIN32

std::expected<FstReader, Error> FstReader::open(const char* path) {
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        return std::unexpected(Error::FileOpenFailed);
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(fh, &file_size) || file_size.QuadPart == 0) {
        CloseHandle(fh);
        return std::unexpected(Error::FileOpenFailed);
    }

    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mh) {
        CloseHandle(fh);
        return std::unexpected(Error::MmapFailed);
    }

    void* view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mh);
        CloseHandle(fh);
        return std::unexpected(Error::MmapFailed);
    }

    auto* base = static_cast<const uint8_t*>(view);
    auto total = static_cast<size_t>(file_size.QuadPart);

    if (!validate_header(base, total)) {
        UnmapViewOfFile(view);
        CloseHandle(mh);
        CloseHandle(fh);
        return std::unexpected(Error::InvalidFormat);
    }

    FstReader reader;
    reader.base_ = base;
    reader.total_size_ = total;
    reader.owns_buffer_ = false;
    reader.file_handle_ = fh;
    reader.map_handle_ = mh;
    return reader;
}

#else  // POSIX

std::expected<FstReader, Error> FstReader::open(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return std::unexpected(Error::FileOpenFailed);
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        ::close(fd);
        return std::unexpected(Error::FileOpenFailed);
    }

    void* addr = mmap(nullptr, static_cast<size_t>(st.st_size),
                       PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        ::close(fd);
        return std::unexpected(Error::MmapFailed);
    }

    ::close(fd);  // Safe on POSIX after mmap.

    auto* base = static_cast<const uint8_t*>(addr);
    auto total = static_cast<size_t>(st.st_size);

    if (!validate_header(base, total)) {
        munmap(addr, total);
        return std::unexpected(Error::InvalidFormat);
    }

    FstReader reader;
    reader.base_ = base;
    reader.total_size_ = total;
    reader.owns_buffer_ = false;
    reader.fd_ = -1;  // Already closed.
    return reader;
}

#endif

// ── Destructor & move ────────────────────────────────────────

void FstReader::cleanup() {
    if (!base_) return;

    if (owns_buffer_) {
        // buffer_ handles its own memory.
        buffer_.clear();
    } else {
#ifdef _WIN32
        UnmapViewOfFile(const_cast<uint8_t*>(base_));
        if (map_handle_) CloseHandle(map_handle_);
        if (file_handle_) CloseHandle(file_handle_);
        map_handle_ = nullptr;
        file_handle_ = nullptr;
#else
        munmap(const_cast<uint8_t*>(base_), total_size_);
#endif
    }
    base_ = nullptr;
    total_size_ = 0;
}

FstReader::~FstReader() {
    cleanup();
}

FstReader::FstReader(FstReader&& other) noexcept
    : base_(other.base_),
      total_size_(other.total_size_),
      owns_buffer_(other.owns_buffer_),
      buffer_(std::move(other.buffer_)) {
#ifdef _WIN32
    file_handle_ = other.file_handle_;
    map_handle_ = other.map_handle_;
    other.file_handle_ = nullptr;
    other.map_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    if (owns_buffer_) {
        // buffer_ was moved; update base_ to point to our copy.
        base_ = buffer_.data();
    }
    other.base_ = nullptr;
    other.total_size_ = 0;
    other.owns_buffer_ = false;
}

FstReader& FstReader::operator=(FstReader&& other) noexcept {
    if (this != &other) {
        cleanup();
        base_ = other.base_;
        total_size_ = other.total_size_;
        owns_buffer_ = other.owns_buffer_;
        buffer_ = std::move(other.buffer_);
#ifdef _WIN32
        file_handle_ = other.file_handle_;
        map_handle_ = other.map_handle_;
        other.file_handle_ = nullptr;
        other.map_handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        if (owns_buffer_) {
            base_ = buffer_.data();
        }
        other.base_ = nullptr;
        other.total_size_ = 0;
        other.owns_buffer_ = false;
    }
    return *this;
}

// ── Query methods ────────────────────────────────────────────

uint32_t FstReader::root_offset() const {
    return read_u32(base_, 0x18);
}

uint32_t FstReader::num_nodes() const {
    return read_u32(base_, 0x08);
}

bool FstReader::contains(std::string_view key) const {
    assert(base_);
    const uint8_t* data_start = base_ + HEADER_SIZE;
    uint32_t cursor_off = root_offset();

    for (size_t ki = 0; ki < key.size(); ++ki) {
        uint8_t c = static_cast<uint8_t>(key[ki]);
        const uint8_t* node_ptr = data_start + cursor_off;

        // Read flags_and_count
        uint8_t flags_and_count = *node_ptr++;
        uint8_t count = flags_and_count & 0x3F;
        if (count == 63) {
            count = 63 + *node_ptr++;
        }

        // Scan transitions for matching label.
        bool found = false;
        for (uint8_t i = 0; i < count; ++i) {
            uint8_t label = *node_ptr;
            uint32_t target;
            std::memcpy(&target, node_ptr + 1, 4);
            node_ptr += 5;

            if (label == c) {
                cursor_off = target;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    // Check if final node.
    const uint8_t* node_ptr = data_start + cursor_off;
    uint8_t flags_and_count = *node_ptr;
    return (flags_and_count & 0x80) != 0;
}

}  // namespace internal
}  // namespace fuzzyfst
