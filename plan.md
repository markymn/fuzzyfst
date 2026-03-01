# FuzzyFST — High-Performance FST + Levenshtein Fuzzy Search Library

A from-scratch C++20 library that builds minimal acyclic FSTs via Daciuk's algorithm, serializes to a zero-copy mmap-friendly binary format, and supports sub-millisecond fuzzy search via bit-parallel Levenshtein NFA intersection.

---

## 1. Directory & File Structure

```
d:\fuzzyfst\
├── CMakeLists.txt                   # Top-level CMake (C++20, no RTTI, no exceptions on hot paths)
├── include\
│   └── fuzzyfst\
│       └── fuzzyfst.h               # Single public API header
├── src\
│   ├── arena.h                      # Arena allocator (internal)
│   ├── arena.cpp
│   ├── radix_sort.h                 # MSD radix sort for byte strings
│   ├── radix_sort.cpp
│   ├── trie_builder.h               # Intermediate trie + Daciuk minimization
│   ├── trie_builder.cpp
│   ├── state_map.h                  # Hash map for state deduplication
│   ├── state_map.cpp
│   ├── fst_writer.h                 # Serialize minimal FST → binary format
│   ├── fst_writer.cpp
│   ├── fst_reader.h                 # mmap-backed zero-copy reader
│   ├── fst_reader.cpp
│   ├── levenshtein_nfa.h            # Bit-parallel Levenshtein NFA
│   ├── levenshtein_nfa.cpp
│   ├── fuzzy_search.h               # Lazy FST × Levenshtein intersection
│   ├── fuzzy_search.cpp
│   └── simd_scan.h                  # SIMD transition scanning (header-only, SSE4.2/AVX2)
├── cli\
│   ├── CMakeLists.txt
│   └── main.cpp                     # fst-build / fst-query CLI
├── tests\
│   ├── CMakeLists.txt
│   ├── test_arena.cpp
│   ├── test_radix_sort.cpp
│   ├── test_state_map.cpp
│   ├── test_fst_builder.cpp
│   ├── test_fst_roundtrip.cpp       # build → serialize → mmap → query
│   ├── test_levenshtein_nfa.cpp
│   ├── test_fuzzy_search.cpp
│   └── test_fuzzy_correctness.cpp   # Cross-validate vs brute-force Levenshtein
├── bench\
│   ├── CMakeLists.txt
│   ├── bench_radix_sort.cpp
│   ├── bench_state_map.cpp
│   ├── bench_fst_build.cpp
│   ├── bench_fst_lookup.cpp
│   ├── bench_levenshtein_nfa.cpp
│   └── bench_fuzzy_search.cpp
└── data\
    └── words.txt                    # Sample word list for testing
```

---

## 2. Core Abstractions

### 2.1 Arena Allocator — `arena.h / arena.cpp`

**Purpose:** Eliminate per-node `malloc` during trie construction. All intermediate trie nodes are born and die together, so a bump allocator is ideal.

```cpp
class Arena {
public:
    explicit Arena(size_t block_size = 1 << 20);  // 1 MiB blocks
    ~Arena();

    // Allocate `size` bytes aligned to `align`.  Never fails (aborts on OOM).
    void* alloc(size_t size, size_t align = alignof(std::max_align_t));

    // Typed helper
    template <typename T, typename... Args>
    T* create(Args&&... args);

    void reset();          // Reuse memory (invalidates all pointers)
    size_t bytes_used() const;

private:
    struct Block {
        uint8_t* data;
        size_t   capacity;
        size_t   used;
    };
    std::vector<Block> blocks_;    // Owned blocks
    size_t             block_size_;
};
```

**Data members:**
| Field | Type | Purpose |
|---|---|---|
| `blocks_` | `std::vector<Block>` | List of allocated memory blocks |
| `block_size_` | `size_t` | Default block capacity (1 MiB) |
| `Block::data` | `uint8_t*` | Raw memory |
| `Block::capacity` | `size_t` | Block total size |
| `Block::used` | `size_t` | Bump pointer offset |

**Memory layout rationale:** Linear bump allocation gives O(1) alloc with zero fragmentation. All nodes are packed contiguously within blocks improving cache hit rate during construction. The entire arena is freed in one shot when construction finishes.

**Invariants:**
- `used <= capacity` for every block
- All returned pointers are aligned to the requested alignment
- No individual deallocation — only bulk `reset()` or destructor

---

### 2.2 Radix Sort — `radix_sort.h / radix_sort.cpp`

**Purpose:** Sort the input word list in O(n·k) time (n words, k max length) rather than O(n·k·log n) for comparison sort.

```cpp
// Sort an array of string_views in-place using MSD radix sort.
// `scratch` must be at least `count` elements.  Zero heap allocation.
void radix_sort(std::string_view* data, size_t count,
                std::string_view* scratch);
```

**Algorithm:** Most-Significant-Digit radix sort, 256-way on raw bytes. Recurses on each bucket at the next character position. Falls back to insertion sort for buckets ≤ 32 elements to avoid recursion overhead on small partitions.

**Memory layout rationale:** Works on `string_view` (16 bytes each: pointer + length). The scratch buffer is caller-provided so zero heap allocation occurs inside the sort.

---

### 2.3 Intermediate Trie Node — Construction Phase Only

**Purpose:** Temporary node used during Daciuk's incremental construction. Destroyed after serialization.

```cpp
struct TrieNode {
    struct Trans {
        uint8_t   label;          //  1 byte  — transition character
        uint8_t   _pad[3];        //  3 bytes — padding
        uint32_t  child_idx;      //  4 bytes — index into node pool
    };  // 8 bytes, fits in a register

    Trans*    transitions;        // Pointer into arena (contiguous array)
    uint8_t   num_transitions;    // 0-255 outgoing edges
    bool      is_final;           // Accepts a word
    uint16_t  _pad;
    uint32_t  hash_cache;         // Cached hash for dedup (0 = not yet computed)
    uint32_t  id;                 // Unique node id (index in node pool)
};  // 24 bytes total with packing
```

**Data members:**
| Field | Type | Size | Purpose |
|---|---|---|---|
| `transitions` | `Trans*` | 8 bytes | Pointer to contiguous transition array in arena |
| `num_transitions` | `uint8_t` | 1 byte | Outgoing edge count |
| `is_final` | `bool` | 1 byte | Word-ending flag |
| `_pad` | `uint16_t` | 2 bytes | Padding for alignment |
| `hash_cache` | `uint32_t` | 4 bytes | Memoized structural hash |
| `id` | `uint32_t` | 4 bytes | Pool index |

**Memory layout rationale:** Transitions are stored as a contiguous array in the arena, not a `std::vector`, to avoid per-node heap allocations. The `Trans` struct is exactly 8 bytes so a scan of 8 transitions fits in one cache line. `hash_cache` avoids recomputing the hash on every dedup check.

**Invariants:**
- `transitions` array is sorted by `label`
- `num_transitions ∈ [0, 255]`
- `hash_cache` is invalidated (set to 0) whenever transitions are mutated

---

### 2.4 State Deduplication Hash Map — `state_map.h / state_map.cpp`

**Purpose:** Daciuk's algorithm requires identifying when two trie subtrees are structurally identical so they can be merged. This is the core bottleneck.

```cpp
class StateMap {
public:
    explicit StateMap(size_t initial_capacity = 1 << 16);

    // PRECONDITION: `node` must have all children already minimized with
    // stable canonical IDs assigned.  Daciuk's bottom-up minimization order
    // guarantees this — do NOT call on a node whose children are still mutable.
    //
    // Returns existing node ID if a structurally-equal node exists,
    // otherwise inserts `node` and returns its own ID.
    uint32_t find_or_insert(const TrieNode* node);

    size_t size() const;

private:
    // Open-addressing, Robin Hood hashing, power-of-2 capacity
    struct Slot {
        uint32_t  hash;       // Full hash (0 = empty)
        uint32_t  node_id;    // Index into node pool
        uint16_t  psl;        // Probe sequence length
        uint16_t  _pad;
    };  // 12 bytes

    std::vector<Slot> slots_;
    size_t            size_;
    size_t            capacity_;      // Always a power of two
    size_t            max_load_;      // capacity_ * 0.8

    static uint32_t hash_node(const TrieNode* node);
    static bool nodes_equal(const TrieNode* a, const TrieNode* b);
    void grow();
};
```

**Hash function:** Hash over `(is_final, num_transitions, label[0..n], child_id[0..n])` where each `child_id` is the **post-minimization canonical ID**. Uses a fast multiplicative hash (similar to wyhash/xxHash3 mixing) — multiply-shift with 64-bit intermediates.

**Why Robin Hood hashing:** Guarantees bounded probe lengths, which keeps worst-case lookup constant and cache-friendly. Open addressing avoids pointer chasing.

**Why power-of-2 capacity:** Allows `hash & (capacity - 1)` instead of modulo — a single AND instruction vs expensive division.

**Invariants:**
- Load factor ≤ 0.8
- `capacity_` is always a power of two
- `hash == 0` means slot is empty (reserve 0-hash maps by remapping to 1)
- `find_or_insert()` is only called on nodes whose children all have final canonical IDs (see precondition)

**Equality check:** Compare `(is_final, num_transitions)` then compare each transition's `(label, child_id)` pairs. Both hash and equality use post-minimization `child_id` values, never arena pointers or pre-minimization indices.

---

### 2.5 FST Builder — `trie_builder.h / trie_builder.cpp`

**Purpose:** Implements Daciuk's incremental minimization algorithm in a single pass over sorted input.

```cpp
class FstBuilder {
public:
    explicit FstBuilder(Arena& arena);

    // Add a word.  Words MUST be added in strict lexicographic order.
    // Returns Error::InputNotSorted if `word` <= prev_word_.
    // This is a runtime check, NOT a debug assert — it fires in release builds.
    [[nodiscard]] std::expected<void, Error> add(std::string_view word);

    // Finalize: freeze the last path, minimize remaining suffixes.
    // Returns the serialized FST bytes, or an error on failure.
    [[nodiscard]] std::expected<std::vector<uint8_t>, Error> finish();

private:
    Arena&              arena_;
    StateMap            state_map_;
    std::vector<TrieNode*> temp_path_;    // Current "last word" path
    std::string         prev_word_;       // Previous word (for sort check)

    // Minimize (freeze + dedup) nodes on temp_path_ from index `down_to`
    // down to the end.
    void minimize(size_t down_to);

    // Allocate a fresh trie node from the arena
    TrieNode* alloc_node();
};
```

**Algorithm walkthrough (Daciuk's):**
1. For each new word, find the longest common prefix with the previous word.
2. `minimize()` all nodes below the common prefix point — these suffixes are finalized and can be deduplicated.
3. Extend the trie with the remaining characters of the new word.
4. On `finish()`, minimize the entire remaining path from root.

**Memory layout rationale:** `temp_path_` is a small vector (max word length, typically ≤ 64) reused across all words. All `TrieNode` memory comes from the arena. The only heap allocation is `prev_word_` and `temp_path_` (both small and reused).

**Invariants:**
- Words must be added in strict lexicographic order — `add()` returns `std::expected` with `Error::InputNotSorted` if violated (runtime check, not assert)
- After `minimize()`, deduplicated subtrees share the same node ID

---

### 2.6 Serialized FST Binary Format

The binary format IS the in-memory query format. No parsing, no deserialization — `mmap` the file and cast a pointer.

#### Header (64 bytes, cache-line aligned)

```
Offset  Size   Field              Description
──────  ─────  ─────────────────  ──────────────────────────────────────
0x00    4      magic              0x46535431 ("FST1")
0x04    4      version            uint32_t, currently 1
0x08    4      num_nodes          uint32_t, total node count
0x0C    4      num_transitions    uint32_t, total transition count
0x10    8      data_size          uint64_t, total size of data section in bytes
0x18    4      root_offset        uint32_t, byte offset of root node from data start
0x1C    4      flags              uint32_t, bitfield (bit 0: has outputs)
0x20    32     _reserved          Zero-padded for future use / alignment
```

Total header: **64 bytes** (one cache line).

#### Node Encoding

Nodes are packed contiguously after the header. Each node is encoded as:

```
Offset  Size        Field              Description
──────  ──────────  ─────────────────  ────────────────────────────────────
+0      1           flags_and_count    bits [7]: is_final
                                       bits [6]: has_output (reserved, always 0 for v1)
                                       bits [5:0]: num_transitions if ≤ 63
+1      1           count_ext          num_transitions high byte if count > 63
                                       (only present if bits[5:0] == 63)
+N      N*5 or N*6  transitions[]      Transition array (see below)
```

If `flags_and_count & 0x3F == 63` (escape value), the true count is `63 + count_ext` (max 318 transitions — more than enough for byte alphabet). Otherwise `count_ext` is omitted and the node is 1 byte + transitions.

#### Transition Encoding (5 bytes each, or 6 with output)

```
Offset  Size   Field          Description
──────  ─────  ───────────    ─────────────────────────────────────────
+0      1      label          uint8_t, transition byte
+1      4      target_offset  uint32_t, byte offset of target node from data start
```

5 bytes per transition. For a node with 4 transitions = 20 bytes of transitions + 1 byte header = 21 bytes — fits in half a cache line.

> **Why uint32_t offsets, not pointers:** 4 bytes vs 8 bytes, and offsets are position-independent — the mmap base can be anywhere.

> **Why 5-byte transitions, not 8:** Packing saves ~37% memory. The 1-byte misalignment is acceptable because transition scanning is sequential and the CPU prefetcher handles it. For SIMD scanning, labels are extracted into a 32-byte register.

#### Data Layout in File

```
[ Header: 64 bytes ]
[ Node 0: flags + transitions... ]
[ Node 1: flags + transitions... ]
[ ...                            ]
[ Node N-1                       ]
```

Nodes are written in **reverse topological order** (leaves first, root last). This means the root is at the highest offset, stored in `header.root_offset`. Children always have lower offsets than parents, which enables forward-only reads during construction and natural cache prefetching during top-down traversal.

---

### 2.7 FST Reader — `fst_reader.h / fst_reader.cpp`

**Purpose:** Memory-map the FST file and provide zero-copy query access.

```cpp
class FstReader {
public:
    // Open FST file via mmap.  Returns error code on failure.
    static std::expected<FstReader, int> open(const char* path);
    ~FstReader();

    FstReader(FstReader&&) noexcept;
    FstReader& operator=(FstReader&&) noexcept;

    // Exact lookup: returns true if `key` is in the FST.
    bool contains(std::string_view key) const;

    // Access the raw data pointer (for intersection iterator)
    const uint8_t* data() const;
    uint32_t root_offset() const;
    uint32_t num_nodes() const;

private:
    const uint8_t* base_;       // mmap base pointer
    size_t         file_size_;  // Total mapped size
#ifdef _WIN32
    void*          file_handle_;
    void*          map_handle_;
#else
    int            fd_;
#endif
};
```

**`contains()` implementation (hot path):**

```
1. cursor = base_ + sizeof(Header) + header->root_offset
2. For each byte `c` in key:
   a. Read node header: flags_and_count
   b. Scan transitions for label == c:
      - If num_transitions ≤ 8: linear scan (likely a single cache line)
      - If num_transitions ≤ 32: SIMD scan with SSE4.2
      - If num_transitions > 32: binary search on labels
   c. If found: cursor = base_ + sizeof(Header) + target_offset
   d. If not found: return false
3. At end of key: return (cursor_node->flags & IS_FINAL) != 0
```

**Platform considerations:**
- **Linux/macOS:** `mmap` + `MAP_PRIVATE | MAP_POPULATE`. Use `madvise(MADV_SEQUENTIAL)` during build verification, `madvise(MADV_RANDOM)` for query workloads.
- **Windows (build/write path):** `CreateFile` with `FILE_FLAG_SEQUENTIAL_SCAN` (equivalent to `MADV_SEQUENTIAL` — tells the OS to optimize readahead for sequential I/O). Then `CreateFileMapping` + `MapViewOfFile`.
- **Windows (query path):** `CreateFile` with `FILE_FLAG_RANDOM_ACCESS` (equivalent to `MADV_RANDOM` — disables readahead, optimizes for random access patterns). Then `CreateFileMapping` + `MapViewOfFile`.
- On both platforms, `UnmapViewOfFile` / `munmap` in the destructor.

**Invariants:**
- `base_` is always a valid, fully-mapped pointer or null
- File is read-only mapped — thread-safe for concurrent reads by design
- No heap allocation on any query path

---

### 2.8 Bit-Parallel Levenshtein NFA — `levenshtein_nfa.h / levenshtein_nfa.cpp`

**Purpose:** Represent the Levenshtein automaton for a query word at distance ≤ d as a bitwise NFA where each transition is computed with O(1) bit operations instead of O(m) DP row updates.

#### State Representation

For a query word of length `m` and max edit distance `d`, the Levenshtein NFA has `(m+1) × (d+1)` states arranged in a grid. State `(i, e)` means "consumed `i` characters of the query with `e` edits so far."

**Bit-parallel encoding (Hyyro/Myers):**

We encode the DP computation as horizontal deltas. For each column (character processed), we maintain:

```cpp
struct LevenshteinState {
    uint64_t Pv;    // Positive vertical delta bitvector
    uint64_t Mv;    // Negative vertical delta bitvector
    uint32_t dist;  // Current distance at the bottom of the column
};  // 20 bytes
```

**Bitvector semantics (Myers' algorithm):**
- Bit `i` of `Pv` is set if `D[i] - D[i-1] = +1` (distance increases going down)
- Bit `i` of `Mv` is set if `D[i] - D[i-1] = -1` (distance decreases going down)
- From these two bitvectors, the entire DP column is implicitly recoverable
- `dist` tracks `D[m]`, the distance at the bottom of the column

> **Hard limit:** This encodes up to 64 query characters in a single `uint64_t`. Queries longer than 64 characters are **not supported**. `init()` returns `Error::QueryTooLong` if `query.size() > 64`. The longest English word is 45 characters, so this limit is sufficient for all practical natural language use cases.

#### NFA Transition (processing one FST transition label)

```cpp
struct LevenshteinNFA {
    // Precomputed: for each byte value, which query positions match
    uint64_t char_mask[256];   // char_mask[c] has bit `i` set if query[i] == c
    uint32_t query_len;        // m
    uint32_t max_dist;         // d

    // Initialize from query string.  Returns Error::QueryTooLong if
    // query.size() > 64 (bitvector representation limit).
    [[nodiscard]] std::expected<void, Error> init(std::string_view query, uint32_t max_distance);

    // Advance the NFA by one character `c`.  Pure bitwise, no branches.
    // Returns the new state.  O(1) time, ~12 bitwise instructions.
    static LevenshteinState step(const LevenshteinState& state,
                                  uint64_t eq_mask,  // = char_mask[c]
                                  uint32_t query_len);

    // Can this state possibly reach an accepting state within max_dist?
    bool can_match(const LevenshteinState& state) const;

    // Is this state accepting? (distance at end ≤ max_dist)
    bool is_match(const LevenshteinState& state) const;
};
```

#### The `step()` function — Myers' bit-parallel algorithm

```
Input:  Pv, Mv (current column deltas), Eq (char_mask[c])
Output: Pv', Mv' (next column deltas), dist' (updated bottom distance)

    Xv = Eq | Mv
    Xh = (((Eq & Pv) + Pv) ^ Pv) | Eq
    Ph = Mv | ~(Xh | Pv)
    Mh = Pv & Xh

    // Update dist
    if (Ph & (1ULL << (m-1)))  dist += 1
    if (Mh & (1ULL << (m-1)))  dist -= 1

    // Shift for next column
    Ph <<= 1
    Mh <<= 1
    Pv' = Mh | ~(Xv | Ph)
    Mv' = Ph & Xv
```

**~12 bitwise operations** (AND, OR, XOR, NOT, ADD, SHIFT) per character processed. No loops, no branches (the dist update can be made branchless with conditional moves).

**Why this matters for FST intersection:** At each FST node, for each outgoing transition label `c`, we compute one `step()`. This tells us the new Levenshtein state. If `can_match()` is false, we prune that branch entirely. This is orders of magnitude faster than recomputing a full DP row.

---

### 2.9 Lazy FST × Levenshtein Intersection Iterator — `fuzzy_search.h / fuzzy_search.cpp`

**Purpose:** Simultaneously traverse the FST and the Levenshtein NFA, yielding all words within edit distance `d` of the query, without ever materializing the full product automaton.

> **No duplicate types:** `FuzzyResult` is defined **only** in the public header `fuzzyfst.h`. Internal code (`fuzzy_search.h`) includes the public header and uses `fuzzyfst::FuzzyResult` directly. No separate internal definition exists.

```cpp
// fuzzy_search.h includes <fuzzyfst/fuzzyfst.h> for FuzzyResult definition.

class FuzzyIterator {
public:
    // Initialize the iterator.  `result_buf` and `word_buf` are caller-provided.
    FuzzyIterator(const FstReader& fst,
                  const LevenshteinNFA& nfa,
                  char* word_buf,           // Scratch for building result words
                  size_t word_buf_size,
                  FuzzyResult* result_buf,  // Output buffer
                  size_t result_buf_cap);

    // Collect up to `result_buf_cap` results, returns count found.
    // Can be called repeatedly (resumes from where it left off).
    size_t collect();

    bool done() const;

private:
    // Explicit stack frame for iterative DFS (no recursion)
    struct Frame {
        uint32_t          node_offset;      // FST node byte offset
        LevenshteinState  lev_state;        // NFA state at this node
        uint8_t           depth;            // Current word length
        uint8_t           next_trans_idx;   // Next transition to explore
        uint8_t           _pad[2];
    };  // 32 bytes

    const FstReader&      fst_;
    const LevenshteinNFA& nfa_;
    char*                 word_buf_;
    size_t                word_buf_size_;
    FuzzyResult*          result_buf_;
    size_t                result_buf_cap_;

    // Iterative DFS stack — pre-allocated, max depth = max word length
    Frame                 stack_[256];       // 8 KiB on the stack, no heap
    uint8_t               stack_top_;
};
```

**Algorithm:**
```
1. Push (root_offset, initial_lev_state, depth=0, next_trans_idx=0) onto stack
2. While stack is not empty:
   a. Peek top frame F
   b. If F.next_trans_idx == num_transitions at F.node_offset:
      - Pop F, continue
   c. Read transition[F.next_trans_idx] at F.node_offset: (label, target)
   d. F.next_trans_idx++   // advance for next iteration
   e. Compute new_lev = LevenshteinNFA::step(F.lev_state, char_mask[label])
   f. If !can_match(new_lev): continue  // PRUNE
   g. word_buf_[F.depth] = label
   h. If target node is_final AND is_match(new_lev):
      - Emit result: { word_buf_, depth+1, new_lev.dist }
   i. Push (target, new_lev, depth+1, 0) onto stack
3. Return count of emitted results
```

**Performance characteristics:**
- **Zero heap allocation:** Stack is a fixed-size array; result and word buffers are caller-provided
- **Iterative, not recursive:** No function call overhead; constant stack usage regardless of trie depth
- **Early pruning:** `can_match()` cuts entire subtrees. For distance 1 on a typical English dictionary, this prunes >99% of the FST
- **Resumable:** `collect()` can return a partial batch and be called again, enabling streaming results

---

### 2.10 SIMD Transition Scanning — `simd_scan.h` *(DEFERRED — Phase 6 optimization)*

> [!WARNING]
> **Do not implement the SIMD path until profiling proves transition scanning is a bottleneck.** For most nodes in a natural language dictionary, there are fewer than 8 transitions and scalar linear scan beats SIMD due to setup overhead. The initial implementation must use scalar linear scan (≤8 transitions) or binary search (>8 transitions) only. SIMD is a late-stage optimization pass, not a phase 1 concern.

**Known difficulty:** Labels are at stride-5 byte offsets (due to the 5-byte transition encoding). Standard SIMD gather instructions (`vpgatherdd`) operate on 4-byte aligned strides, not 5-byte. Gathering labels into a contiguous SIMD register requires a non-trivial shuffle or scalar gather loop, which partially negates the SIMD advantage. This is solvable but not trivial to implement correctly.

**When to revisit:** After all fuzzy search correctness tests pass and `bench_fst_lookup` is baselined. Profile with `perf record` / VTune. If transition scanning is ≥20% of `contains()` runtime on a large dictionary, implement the SIMD path. Otherwise, skip it entirely.

---

### 2.11 Public API — `include/fuzzyfst/fuzzyfst.h`

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <expected>
#include <string_view>
#include <span>
#include <vector>

namespace fuzzyfst {

// Error codes
enum class Error : int {
    Ok = 0,
    FileOpenFailed,
    MmapFailed,
    InvalidFormat,
    InputNotSorted,
    BufferTooSmall,
    QueryTooLong,      // Query exceeds 64 characters (bitvector limit)
};

// ── Types ──────────────────────────────────────────────────

// A single fuzzy search result.  `word` points into the caller-provided
// word buffer (zero-alloc overload) or into a heap-allocated string
// (convenience overload).
struct FuzzyResult {
    std::string_view word;
    uint32_t         distance;
};

// ── Build API ──────────────────────────────────────────────

struct BuildOptions {
    bool sort_input = true;    // If true, radix-sort the input
};

// Build an FST from a list of words and write to `output_path`.
// Words are provided as a contiguous buffer of newline-separated strings.
// Returns Error::InputNotSorted if sort_input is false and words are not
// in strict lexicographic order.
// build() is NOT thread-safe — do not call concurrently on the same output path.
[[nodiscard]]
std::expected<void, Error> build(const char* output_path,
                                  std::span<std::string_view> words,
                                  BuildOptions opts = {});

// ── Query API ──────────────────────────────────────────────

// Thread safety:
//   Fst is immutable after construction.  contains() and fuzzy_search()
//   are safe to call concurrently from any number of threads with no
//   synchronization required.  The underlying mmap'd memory is read-only
//   and shared.  Each thread must provide its own result/word buffers.
//
// Query length limit:
//   The bit-parallel Levenshtein NFA uses uint64_t bitvectors, so query
//   strings longer than 64 characters are not supported.  fuzzy_search()
//   returns 0 results (zero-alloc overload) or an empty vector (convenience
//   overload) if the query exceeds this limit.
//
// Distance recommendation:
//   max_distance > 3 is not recommended.  At distance ≥ 3 on a large
//   dictionary, the intersection explodes combinatorially and results
//   become meaningless (almost every short word matches everything).
//   A debug assert fires if max_distance > 3 in debug builds.
class Fst {
public:
    // Open a previously-built FST file (zero-copy mmap).
    [[nodiscard]]
    static std::expected<Fst, Error> open(const char* path);

    ~Fst();
    Fst(Fst&&) noexcept;
    Fst& operator=(Fst&&) noexcept;

    // Exact membership test.  O(key length).
    [[nodiscard]] bool contains(std::string_view key) const;

    // Fuzzy search: find all words within `max_distance` edits of `query`.
    // Results are written into `results`.  Returns number of matches found.
    // `word_buf` is scratch space for assembling result strings.
    // This function performs ZERO heap allocations.
    // Returns 0 if query.size() > 64.
    [[nodiscard]]
    size_t fuzzy_search(std::string_view query,
                        uint32_t max_distance,
                        std::span<FuzzyResult> results,
                        std::span<char> word_buf) const;

    // Convenience overload that heap-allocates results.
    // Returns empty vector if query.size() > 64.
    [[nodiscard]]
    std::vector<FuzzyResult> fuzzy_search(std::string_view query,
                                           uint32_t max_distance) const;

    uint32_t num_nodes() const;

private:
    struct Impl;
    Impl* impl_;    // Pimpl — hides FstReader + internals
};

}  // namespace fuzzyfst
```

**Design decisions:**
- **Pimpl idiom:** The public header exposes zero implementation details. All internal types (`FstReader`, `LevenshteinNFA`, etc.) are hidden behind `Impl*`.
- **Two `fuzzy_search` overloads:** The zero-alloc version for latency-critical code; the convenience version for casual use.
- **`std::expected` for errors:** No exceptions on construction. On the query hot path, there are no error conditions — `contains()` and `fuzzy_search()` always succeed.
- **Thread safety:** `Fst` is immutable after construction. `contains()` and `fuzzy_search()` are `const` and use no shared mutable state. Any number of threads can query concurrently with zero synchronization.

---

## 3. End-to-End Flows

> [!IMPORTANT]
> **Sorting happens exactly once, at build time**, on the dictionary words before they are fed to the FST builder. The radix sort in the indexing pipeline (step 3 below) handles this. **At query time there is no sorting of any kind** — the user’s query string is fed directly to the Levenshtein NFA as-is. These two contexts must never be conflated in the documentation or the implementation.

### 3.1 Indexing Flow (`fst-build words.txt index.fst`)

```
Step  Operation              Allocation     Notes
────  ─────────────────────  ─────────────  ──────────────────────────────────
 1    Read words.txt         1 × file size  Single read into buffer
 2    Parse into string_views  1 × n×16     Array of string_view (stack or vec)
 3    Radix sort             1 × n×16       Scratch buffer (same size as input)
 4    Create Arena           1 MiB initial  Bump allocator for trie nodes
 5    Create FstBuilder      ~64 KiB        StateMap initial capacity
 6    For each word:
        FstBuilder::add()    0              All nodes from arena; StateMap may
                                            grow (amortized, rare)
 7    FstBuilder::finish()   1 × output     Produces serialized byte vector
 8    Write to disk          0              Single write
 9    Drop Arena             0              Bulk free all construction memory
```

**Total heap allocations:** ~4 (file buffer, string_view array, scratch buffer, output buffer). Zero per-word allocations.

### 3.2 Query Flow (`fst-query index.fst "helo" --distance 2`)

```
Step  Operation              Allocation     Notes
────  ─────────────────────  ─────────────  ──────────────────────────────────
 1    Fst::open(path)        0              mmap only, no parsing
 2    Init LevenshteinNFA    0              char_mask[256] on stack (2 KiB)
 3    Init FuzzyIterator     0              stack_[256] on stack (8 KiB)
 4    collect() loop         0              Results written to caller buffer
 5    Print results          —              Stdout
```

**Total heap allocations on query path:** Zero (unless using the convenience overload).

---

## 4. Five Hardest Implementation Challenges

### Challenge 1: Correct State Deduplication Hashing

**The pitfall:** A naive hash that hashes only `(is_final, num_transitions, labels)` will produce collisions between nodes whose children differ. You MUST hash the children's canonical IDs (post-minimization), not their raw pointers or pre-minimization indices.

**Correct approach:** When a node is minimized, it receives a permanent ID. The hash for a not-yet-minimized node uses `child->id` for each transition target. This means you can only hash a node after all its children have been minimized — which Daciuk's bottom-up minimization guarantees.

**Performance pitfall:** Rehashing on every lookup (O(transitions) per hash) kills throughput. Cache the hash in `TrieNode::hash_cache` and invalidate only when the node is mutated.

---

### Challenge 2: Memory-Efficient Transition Packing with Unaligned Access

**The pitfall:** The 5-byte transition encoding means `target_offset` is not naturally aligned. On x86/x64 this works (unaligned loads are fast), but naive code may use `memcpy` per transition which the compiler may not optimize.

**Correct approach:** Use `memcpy` for correctness (the compiler will emit a single `mov` on x86), but ensure the hot loop is structured so the compiler can see it's a sequential scan and will auto-vectorize or at least pipeline the loads. Benchmark against a 6-byte or 8-byte aligned alternative — the memory savings of 5 bytes compound at scale.

---

### Challenge 3: Levenshtein NFA `can_match()` — Correct Pruning

**The pitfall:** Implementing a complex min-reachable-distance optimization before the basic algorithm is correct. Aggressive pruning that reconstructs DP column values from bitvectors is error-prone and a premature optimization.

**Correct approach (phase 1 — simplicity first):** After each `step()`, check `state.dist > max_dist`. If so, prune that branch. This is conservative (it only prunes when the *bottom* of the DP column exceeds the threshold, not the minimum entry), but it is trivially correct and requires zero additional computation.

```cpp
bool can_match(const LevenshteinState& state) const {
    return state.dist <= max_dist;
}
```

**Future optimization (phase 6, not before):** Tighter pruning is possible by computing the minimum value in the implicit DP column using `popcount` on `Pv`/`Mv`. This is a measurable improvement but must only be attempted after the basic fuzzy search passes all correctness tests against brute force. Get it right first, then make it faster.

---

### Challenge 4: Resumable Iterative DFS with Correct State Management

**The pitfall:** A recursive DFS is simple but uses O(depth) stack frames (~1-2 KiB each on x64 due to register spills), and for tries with max depth 64, this means 64-128 KiB of stack usage. Worse, recursive calls prevent the compiler from keeping `Pv`/`Mv` in registers across iterations.

**Correct approach:** Use an explicit `Frame` stack. Each frame stores the FST node offset, the Levenshtein state, the current depth, and which transition to explore next. When all transitions of a node are exhausted, pop. This keeps the hot state (`Pv`, `Mv`, `dist`) in the explicit stack where we control layout, and the function itself stays iterative — one loop, no calls.

**Subtlety:** The iterator must be resumable (for batched result collection). This means `stack_` and `stack_top_` persist between `collect()` calls. The state must accurately capture "where we left off" so the next call continues seamlessly.

---

### Challenge 5: Cross-Platform mmap with Correct Lifetime and Error Handling

**The pitfall:** On Windows, memory mapping requires `CreateFileMapping` + `MapViewOfFile` and the handles must be kept alive. Closing the file handle before unmapping on Windows is allowed, but closing the mapping handle before `UnmapViewOfFile` causes a crash. The lifetime management is different from POSIX where `close(fd)` after `mmap()` is fine.

**Correct approach:** Encapsulate all platform differences in `FstReader`. On Windows, store both `file_handle_` and `map_handle_` and close them in the correct order in the destructor. On POSIX, `close(fd_)` immediately after `mmap()` is valid. Use `MAP_POPULATE` on Linux to prefault pages and avoid page faults on first access. Use `madvise(MADV_RANDOM)` for query workloads to disable readahead. Move-only semantics prevent double-unmap.

---

## 5. Build & Test Order

> [!CAUTION]
> **This build order is a hard constraint, not a suggestion.** The implementation must proceed in this exact sequence. Do not begin any component until the previous one has passing tests. Do not write the fuzzy search code before the FST round-trip test passes. Skipping ahead is the primary way this project fails.

**Strict sequence:** Arena → Radix Sort → StateMap → Trie Builder → FST Writer → FST Reader → Round-trip test → Levenshtein NFA → **Correctness oracle (`test_fuzzy_correctness`)** → Fuzzy Iterator → CLI → Benchmarks.

### Phase 1: Foundation

| # | Component | Test | Micro-Benchmark |
|---|-----------|------|------------------|
| 1 | **Arena Allocator** | Alloc/dealloc correctness, alignment checks | `bench_arena`: 10M allocations of 8-64 byte objects, measure throughput vs `new/delete` |
| 2 | **Radix Sort** | Sort 1K, 100K, 1M random strings; verify sorted order; compare to `std::sort` | `bench_radix_sort`: Sort 100K / 1M / 10M words, measure wall time vs `std::sort` |

### Phase 2: Construction

| # | Component | Test | Micro-Benchmark |
|---|-----------|------|------------------|
| 3 | **StateMap** | Insert/find with known duplicates; verify dedup count matches expected | `bench_state_map`: Insert 100K / 1M / 10M synthetic node signatures, measure ops/sec and probe length distribution |
| 4 | **Trie Builder (Daciuk)** | Build from 10 words, verify node count matches hand-computed minimal FST | `bench_fst_build`: Build FST from 100K / 500K / 1M word dictionary, measure construction time and peak memory |

### Phase 3: Serialization & I/O

| # | Component | Test | Micro-Benchmark |
|---|-----------|------|------------------|
| 5 | **FST Writer** | Serialize → verify magic/header fields → verify node count | `bench_fst_write`: Serialize 1M-node FST, measure throughput (MB/s) |
| 6 | **FST Reader (mmap)** | Open serialized file → exact lookup all inserted words → verify membership | `bench_fst_lookup`: 1M random exact lookups on mmap'd FST, measure throughput (queries/sec) and latency distribution |
| 7 | **Round-trip test** | Build → serialize → mmap → verify every word present, no false positives from random probes | — |

### Phase 4: Fuzzy Search

| # | Component | Test | Micro-Benchmark |
|---|-----------|------|------------------|
| 8 | **Levenshtein NFA** | Verify `step()` against naive DP for all single-char transitions on 20 test strings × distance 1,2,3 | `bench_levenshtein_nfa`: 10M `step()` calls with random characters, measure throughput |
| 9 | **Correctness oracle** (`test_fuzzy_correctness`) | Write the brute-force Levenshtein distance function and comparison harness **immediately after the NFA is implemented, before the fuzzy iterator is considered complete.** For N=10K dictionary, Q=100 queries, D=1,2,3: assert fuzzy results == brute-force results (sets must be identical). **This is the most important test file in the project.** | — |
| 10 | **Fuzzy Iterator** | Query 100 words at distance 1,2 → all results must pass the correctness oracle from step 9 | `bench_fuzzy_search`: 1000 random queries at distance 1, 2, 3 on 500K-word FST, measure avg latency and p99 |

### Phase 5: Integration & CLI

| # | Component | Test | Benchmark |
|---|-----------|------|-----------|
| 11 | **CLI tool** | End-to-end: `fst-build words.txt index.fst` then `fst-query index.fst "test" --distance 1` → verify output | `bench_e2e`: Full pipeline on 500K dictionary, 1000 queries, measure total wall time |

### Phase 6: Optimization Passes

| # | Focus area | Approach |
|---|------------|----------|
| 12 | **SIMD scanning** | Profile `contains()` — if transition scanning is hot, enable SIMD path and re-benchmark |
| 13 | **Cache alignment** | Use `perf stat` / VTune to measure cache miss rates; experiment with `alignas(64)` on hot structures |
| 14 | **Branchless `step()`** | Ensure the dist update in `step()` compiles to `cmov` not a branch; inspect assembly |
| 15 | **Tighter `can_match()` pruning** | Implement min-reachable-distance via `popcount(Pv)`/`popcount(Mv)` column reconstruction; re-validate against correctness oracle |

---

## 6. CMake Configuration

```cmake
cmake_minimum_required(VERSION 3.20)
project(fuzzyfst LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Note: we still use exceptions in non-hot paths (file I/O errors).
# Hot path code uses std::expected.
# RTTI is disabled for the library only (see target_compile_options below).
# Tests and benchmarks retain RTTI for debugging convenience (e.g. dynamic_cast).

# Library
add_library(fuzzyfst
    src/arena.cpp
    src/radix_sort.cpp
    src/state_map.cpp
    src/trie_builder.cpp
    src/fst_writer.cpp
    src/fst_reader.cpp
    src/levenshtein_nfa.cpp
    src/fuzzy_search.cpp
)
target_include_directories(fuzzyfst PUBLIC include)
target_include_directories(fuzzyfst PRIVATE src)
target_compile_options(fuzzyfst PRIVATE -march=native -O3 -fno-rtti)

# CLI
add_executable(fst-cli cli/main.cpp)
target_link_libraries(fst-cli PRIVATE fuzzyfst)

# Tests (using a lightweight test framework or raw main())
enable_testing()
add_executable(fst-tests
    tests/test_arena.cpp
    tests/test_radix_sort.cpp
    tests/test_state_map.cpp
    tests/test_fst_builder.cpp
    tests/test_fst_roundtrip.cpp
    tests/test_levenshtein_nfa.cpp
    tests/test_fuzzy_search.cpp
    tests/test_fuzzy_correctness.cpp
)
target_link_libraries(fst-tests PRIVATE fuzzyfst)
add_test(NAME all_tests COMMAND fst-tests)

# Benchmarks
add_executable(fst-bench
    bench/bench_radix_sort.cpp
    bench/bench_state_map.cpp
    bench/bench_fst_build.cpp
    bench/bench_fst_lookup.cpp
    bench/bench_levenshtein_nfa.cpp
    bench/bench_fuzzy_search.cpp
)
target_link_libraries(fst-bench PRIVATE fuzzyfst)
target_compile_options(fst-bench PRIVATE -march=native -O3)
```

---

## 7. Platform-Specific Considerations

| Concern | x86-64 Linux | x86-64 Windows | ARM64 |
|---------|-------------|----------------|-------|
| **mmap** | `mmap/munmap` | `CreateFileMapping/MapViewOfFile` | `mmap/munmap` |
| **SIMD** | SSE4.2 / AVX2 via `immintrin.h` | Same (MSVC: `intrin.h`) | NEON — different intrinsics, separate code path needed |
| **Unaligned loads** | Fast (penalty-free since Nehalem) | Fast (MSVC guarantees) | Mostly fast on ARMv8, but verify |
| **Cache line** | 64 bytes | 64 bytes | 64 bytes (usually) |
| **Alignment** | `alignas(64)` | `alignas(64)` (MSVC supports) | `alignas(64)` |
| **Compiler hints** | `__attribute__((always_inline))`, `[[gnu::hot]]` | `__forceinline` (MSVC) | `__attribute__((always_inline))` |
| **Page size** | 4 KiB (default) | 4 KiB / 64 KiB (large pages) | 4 KiB / 16 KiB |

---

## 8. Word List Acquisition (Required Before Any Testing)

> [!IMPORTANT]
> A real English word list is required from day one — not just for final benchmarks, but for **all testing throughout development**. Using synthetic random strings as a substitute is **not acceptable**. The FST's minimization behavior, node count, and compression ratio are fundamentally different on real natural language data versus random strings, and bugs that only appear on real data will be missed.

**Setup:** Place a word list at `data/words.txt` (one word per line, UTF-8). Every test and benchmark that references "100K word dictionary" or "500K word dictionary" must load from this file (or a subset of it).

**Recommended sources:**
| Platform | Source | Notes |
|----------|--------|-------|
| Linux/macOS | `/usr/share/dict/words` | Pre-installed on most systems, ~100K-235K words |
| Windows | [ENABLE2K](https://www.wordgamedictionary.com/enable/) | ~173K words, public domain, tournament Scrabble dictionary |
| Windows | [12dicts](http://wordlist.aspell.net/12dicts/) | Multiple curated lists, public domain |
| Any | [dwyl/english-words](https://github.com/dwyl/english-words) | ~470K words, JSON and TXT formats, MIT license |

**CMake integration:** Add a check in `CMakeLists.txt` that warns if `data/words.txt` does not exist:
```cmake
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/data/words.txt")
    message(WARNING "data/words.txt not found. Tests and benchmarks require a word list. "
                    "See README for acquisition instructions.")
endif()
```

---

## Verification Plan

### Automated Tests

All tests are compiled into `fst-tests` and run via:
```bash
cmake --build build --target fst-tests
cd build && ctest --output-on-failure
```

Key test cases:
- **`test_fuzzy_correctness`**: For a 10K-word dictionary, run 100 random queries at distances 1, 2, 3. For each query, compute brute-force Levenshtein distance against all 10K words. Assert that the set of matches from `fuzzy_search()` is identical to the brute-force set. This is the primary correctness oracle.
- **Round-trip test**: Build FST from N words → serialize → mmap → `contains()` all N words → no false positives from 10K random non-words.

### Manual Verification

1. Build the CLI tool and run `fst-build` on a real word list (e.g. `/usr/share/dict/words`)
2. Run `fst-query index.fst "helo" --distance 2` and verify results include "hello", "help", "hero", etc.
3. Inspect binary file size — should be significantly smaller than the raw word list
