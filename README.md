# FuzzyFST

Minimal acyclic finite state transducer (FST) with sub-millisecond fuzzy search via bit-parallel Levenshtein NFA intersection. Zero-copy mmap reader. Zero heap allocation on the query path.

## Building

```bash
cmake -B build
cmake --build build
./build/fst-tests    # Run tests (49 tests including 370K-word oracle)
./build/fst-cli      # CLI tool
```

Requires C++23 (GCC 13+ or Clang 17+).

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

Measured on 370,105-word English dictionary ([dwyl/english-words](https://github.com/dwyl/english-words)), Windows 11, GCC 13.2, default build (no `-O3`):

### Build

| Metric | Value |
|--------|-------|
| Total build time | 471 ms |
| Radix sort | 60 ms (13%) |
| Trie construction (Daciuk) | 335 ms (72%) |
| Serialization | 64 ms (14%) |
| FST file size | 2.0 MB (5.5 bytes/word) |
| Unique FST nodes | 160,303 |

Raw dictionary text: ~3.5 MB. The minimized FST achieves ~1.7x compression via shared suffix elimination.

### Query

| Query | Distance | Results | Latency |
|-------|----------|---------|---------|
| `"test"` | 1 | 31 | 91 us |
| `"helo"` | 1 | 16 | 91 us |
| `"algoritm"` | 1 | 2 | 148 us |
| `"cat"` | 2 | 1,108 | 694 us |
| `"algoritm"` | 2 | 8 | 1,404 us |
| `"search"` | 2 | 87 | 1,120 us |

With `-O3 -march=native`: ~22 ns/node (48 us/query avg at d=1, 437 us/query avg at d=2 over 998 queries).

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
