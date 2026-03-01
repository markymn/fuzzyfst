# FuzzyFST

Minimal acyclic finite state transducer (FST) with sub-millisecond fuzzy search via bit-parallel Levenshtein NFA intersection. Zero-copy mmap reader. Zero heap allocation on the query path.

## Building

```bash
# GCC / MinGW
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/fst-tests    # Run tests (49 tests including 370K-word oracle)
./build/fst-cli      # CLI tool

# MSVC (Visual Studio)
cmake -B build
cmake --build build --config Release
./build/Release/fst-tests
./build/Release/fst-cli
```

Requires C++23 (GCC 13+, Clang 17+, or MSVC 17.10+).

## Usage

```bash
# Build an FST index from a sorted or unsorted word list
./build/fst-cli build words.txt index.fst

# Fuzzy search (default distance = 1)
./build/fst-cli query index.fst "algoritm" --distance 1
```

Example output:

```
Query: "algoritm" (distance <= 1)
Found 2 results in 148 us:
  d=1  algorism
  d=1  algorithm
```

### C++ API

```cpp
#include <fuzzyfst/fuzzyfst.h>

// Open a previously-built FST (zero-copy mmap).
auto fst = fuzzyfst::Fst::open("index.fst");

// Exact lookup.
bool found = fst->contains("algorithm");

// Fuzzy search — zero heap allocation overload.
fuzzyfst::FuzzyResult results[1024];
char word_buf[8192];
size_t count = fst->fuzzy_search("algoritm", 1, results, word_buf);

// Convenience overload (allocates a vector).
auto matches = fst->fuzzy_search("algoritm", 1);
for (auto& m : matches)
    printf("%.*s (distance %u)\n", (int)m.word.size(), m.word.data(), m.distance);
```

## Benchmarks

Measured on 370,105-word English dictionary ([dwyl/english-words](https://github.com/dwyl/english-words)). Hardware: Intel i5-9600K @ 3.70 GHz, 6 cores, 8 GB RAM, Windows 11, GCC 13.2, `-O3`.

### Build

| Metric | Value |
|--------|-------|
| Total build time | 95 ms |
| FST file size | 1.9 MB (5.5 bytes/word) |
| Unique FST nodes | 160,303 |

Raw dictionary text: ~3.5 MB. The minimized FST achieves ~1.7x compression via shared suffix elimination.

### Query

Per-node cost: ~22 ns. Average over 50 queries per distance:

| Distance | Avg latency | P99 latency | Avg results |
|----------|-------------|-------------|-------------|
| d=1 | 50 µs | 75 µs | 1.9 |
| d=2 | 415 µs | 571 µs | 39.2 |
| d=3 | 2,040 µs | 2,751 µs | 471.9 |
| d=4 | 5,963 µs | 7,684 µs | 3,320.4 |

### Comparison: FuzzyFST vs SymSpell vs Brute Force

All three methods tested on the same 370,105-word dictionary and the same 50 queries at edit distances 1 through 4. Queries span five categories: 1-char typos, 2-char typos, 3-char typos, common misspellings, and words not in dictionary. Full query set in [data/benchmark_queries.txt](data/benchmark_queries.txt). Same hardware and compiler as above.

| Metric | FuzzyFST | SymSpell | Brute Force |
|--------|----------|----------|-------------|
| Index size | 1.9 MB | 215 / 593 / 1,110 / 2,534 MB (d=1/2/3/4) | 0 (no index) |
| Build time | 93 ms | 1,270 / 4,411 / 9,147 / 22,393 ms (d=1/2/3/4) | 0 ms |
| Avg latency d=1 | 50 µs | 10 µs | 72,218 µs |
| Avg latency d=2 | 415 µs | 143 µs | 76,592 µs |
| Avg latency d=3 | 2,040 µs | 2,089 µs | 83,989 µs |
| Avg latency d=4 | 5,963 µs | 11,988 µs | 75,996 µs |
| P99 latency d=1 | 75 µs | 27 µs | 84,920 µs |
| P99 latency d=2 | 571 µs | 429 µs | 95,691 µs |
| P99 latency d=3 | 2,751 µs | 5,325 µs | 182,169 µs |
| P99 latency d=4 | 7,684 µs | 27,151 µs | 105,438 µs |
| Avg results d=1 | 1.9 | 1.9 | 1.9 |
| Avg results d=2 | 39.2 | 39.2 | 39.2 |
| Avg results d=3 | 471.9 | 471.9 | 471.9 |
| Avg results d=4 | 3,320.4 | 3,221.4 | 3,320.4 |

**SymSpell** uses the symmetric delete algorithm: it precomputes all deletion variants up to max edit distance at build time, trading memory for query speed. At d=1 and d=2, SymSpell is 3-5x faster than FuzzyFST because its lookups are hash table probes with no graph traversal. However, this comes at massive memory cost: 215 MB at d=1 (113x FuzzyFST), 593 MB at d=2 (312x), 1.1 GB at d=3 (584x), and 2.5 GB at d=4 (1,334x). Build times are 14-241x slower. At d=3, SymSpell's query speed advantage disappears — FuzzyFST matches it on average latency with 1.9x better P99. At d=4, FuzzyFST is 2x faster on average and has 3.5x better P99, because the deletion variant space explodes combinatorially.

**FuzzyFST** stores the entire dictionary in a 1.9 MB minimized FST that can be memory-mapped from disk. Query speed scales with edit distance (more nodes to traverse) but memory usage is constant regardless of the edit distance used at query time. Build time is 93 ms — a single build supports all edit distances.

**Brute force** computes Levenshtein distance against every dictionary word. At ~75 ms per query, it is 1,400x slower than FuzzyFST at d=1 and 13x slower at d=4. Its latency is nearly constant across distances because it always scans the full dictionary.

**Distance 3+** is supported but produces increasingly large result sets (~472 at d=3, ~3,320 at d=4). At these distances, downstream ranking/filtering becomes the bottleneck rather than the search itself.

## Algorithm

The query engine intersects a Levenshtein NFA with the FST via iterative DFS. At each FST node, Myers' bit-parallel algorithm advances the NFA state in O(1) using two 64-bit bitvectors (Pv, Mv) that encode the entire DP column implicitly. The pruning function `can_match()` reconstructs the column minimum from these bitvectors and prunes subtrees where no extension can yield a match.

Key implementation details are documented in [INTERNALS.md](INTERNALS.md), including:
- Assembly codegen analysis (`step()` compiles to branchless sbb/adc)
- Why full DP column reconstruction in `can_match()` is a correctness requirement
- Per-function profiling breakdown (decode/step/can_match split ~33% each)
- Cache behavior analysis

### Why distance 2 is 8-10x slower than distance 1

Distance 2 queries visit 8-10x more FST nodes than distance 1. This is not a pruning deficiency — it is an inherent property of Levenshtein distance on dense automata. The per-node cost is constant at both distances, confirming no algorithmic inefficiency. The slowdown comes entirely from visiting more nodes.

**Node visit profile (query "algoritm" on 370K-word FST):**

| Depth | Visited (d=1) | Pruned (d=1) | Visited (d=2) | Pruned (d=2) |
|-------|---------------|--------------|----------------|--------------|
| 0 | 26 | 0 | 26 | 0 |
| 1 | 560 | 485 (87%) | 560 | 0 (0%) |
| 2 | 1,082 | 1,000 (92%) | 4,799 | 3,142 (65%) |
| 3 | 589 | 551 (94%) | 10,139 | 8,642 (85%) |
| 4+ | 216 | 172 | 5,893 | 5,144 |

The critical row is **depth 1**: at d=1, 87% of transitions are pruned; at d=2, **0% are pruned**. After consuming one non-matching FST character (e.g., 'z' for query "algoritm"), the DP column is D[0]=1, D[1]=1, D[2]=2, ..., D[8]=8, with minimum value 1. At d=1, the minimum equals the budget so the state barely survives — but the second character pushes most branches above budget. At d=2, the minimum (1) is strictly below budget (2), so all 560 transitions survive and fan out into 4,799 depth-2 nodes. This 60x amplification at depth 2 is the source of the 8-10x total slowdown.

This behavior is fundamental to Levenshtein distance on real-language FSTs and would appear in any implementation. A DFA-compiled approach (like the Rust `fst` crate) would visit the same nodes but with lower per-node cost (O(1) state lookup vs O(m) bitvector step). Our bit-parallel NFA achieves ~22 ns/node at `-O3`, which is practical for interactive use (sub-500us for d=2 on 370K words).

### Correctness of can_match() pruning

The `can_match()` pruning function reconstructs the minimum value across the entire implicit DP column D[0..m] from the Pv/Mv bitvectors. This full-column reconstruction is a **correctness requirement**, not an optimization.

A naive implementation that only checks the bottom cell (`D[m] <= max_dist`) silently drops valid results. When D[m] > max_dist but some interior cell D[j] <= max_dist (j < m), the target prefix is within distance of a query prefix. Future target characters can still match the remaining query suffix, bringing D[m] within budget for a longer target word. Bottom-cell-only checking prunes these paths, discarding reachable valid matches.

We measured this experimentally with 100 representative queries against the full 370K-word FST:

| Pruning strategy | d=1 visited | d=1 results | d=1 miss rate | d=2 visited | d=2 results | d=2 miss rate |
|-----------------|-------------|-------------|---------------|-------------|-------------|---------------|
| Full column (correct) | 215,737 | 499 | — | 1,829,090 | 8,007 | — |
| Bottom cell only (incorrect) | 4,489 | 157 | **69%** | 48,971 | 3,910 | **51%** |

Bottom-cell-only misses 69% of valid d=1 results and 51% of valid d=2 results. All result counts were verified against a brute-force Levenshtein oracle over the full dictionary.
