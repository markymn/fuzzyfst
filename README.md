# FuzzyFST

Minimal acyclic finite state transducer (FST) with sub-millisecond fuzzy search. Three search modes: Levenshtein BitParallel (Myers' NFA), Levenshtein DFA, and Damerau-Levenshtein DFA. Zero-copy mmap reader. Zero heap allocation on the query path.

## Building

```bash
# GCC / MinGW
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/fst-tests    # Run tests (65 tests including 370K-word oracle)
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

# Fuzzy search (default: distance=1, Levenshtein, BitParallel)
./build/fst-cli query index.fst "algoritm" --distance 1

# Use compiled DFA for faster per-query traversal
./build/fst-cli query index.fst "algoritm" --distance 2 --algorithm dfa

# Damerau-Levenshtein (transpositions count as one edit)
./build/fst-cli query index.fst "teh" --distance 1 --metric damerau
```

Example output:

```
Query: "algoritm" (distance <= 1, Levenshtein, BitParallel)
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

// Fuzzy search — zero heap allocation overload (Levenshtein BitParallel).
fuzzyfst::FuzzyResult results[1024];
char word_buf[8192];
size_t count = fst->fuzzy_search("algoritm", 1, results, word_buf);

// Convenience overload (allocates a vector).
auto matches = fst->fuzzy_search("algoritm", 1);
for (auto& m : matches)
    printf("%.*s (distance %u)\n", (int)m.word.size(), m.word.data(), m.distance);

// Levenshtein DFA: higher startup, faster per-query traversal.
fuzzyfst::SearchOptions opts{.max_distance = 2, .algorithm = fuzzyfst::Algorithm::DFA};
auto dfa_matches = fst->fuzzy_search("algoritm", opts);

// Damerau-Levenshtein DFA: transpositions count as one edit.
fuzzyfst::SearchOptions dam_opts{1, fuzzyfst::DistanceMetric::DamerauLevenshtein};
auto dam_matches = fst->fuzzy_search("teh", dam_opts);
// Finds "the" at distance 1 (transposition), which Levenshtein sees as distance 2.
```

## Benchmarks

Measured on 370,105-word English dictionary ([dwyl/english-words](https://github.com/dwyl/english-words)). Hardware: Intel i5-9600K @ 3.70 GHz, 6 cores, 8 GB RAM, Windows 11, GCC 13.2, `-O3`.

### Build

| Metric | Value |
|--------|-------|
| Total build time | 93 ms |
| FST file size | 1.9 MB (5.5 bytes/word) |
| Unique FST nodes | 160,303 |

Raw dictionary text: ~3.5 MB. The minimized FST achieves ~1.7x compression via shared suffix elimination.

### Query — three search modes

All three modes benchmarked on the same 50 queries, same FST, same hardware. **Startup** is per-query DFA compilation cost (BitParallel has zero startup). **Avg/P99** are traversal-only latencies.

| Distance | Mode | Startup | Avg latency | P99 latency | Avg results |
|----------|------|---------|-------------|-------------|-------------|
| d=1 | Lev BitParallel | 0 µs | 53 µs | 76 µs | 1.9 |
| d=1 | Lev DFA | 135 µs | 17 µs | 30 µs | 1.9 |
| d=1 | Damerau DFA | 1,447 µs | 21 µs | 34 µs | 2.1 |
| d=2 | Lev BitParallel | 0 µs | 498 µs | 735 µs | 39.2 |
| d=2 | Lev DFA | 438 µs | 168 µs | 253 µs | 39.2 |
| d=2 | Damerau DFA | 4,275 µs | 209 µs | 314 µs | 40.9 |
| d=3 | Lev BitParallel | 0 µs | 2,525 µs | 3,497 µs | 471.9 |
| d=3 | Lev DFA | 1,200 µs | 924 µs | 1,206 µs | 471.9 |
| d=3 | Damerau DFA | 13,648 µs | 989 µs | 1,401 µs | 487.7 |
| d=4 | Lev BitParallel | 0 µs | 6,928 µs | 9,380 µs | 3,320.4 |
| d=4 | Lev DFA | 3,479 µs | 2,707 µs | 4,121 µs | 3,320.4 |
| d=4 | Damerau DFA | 41,501 µs | 2,791 µs | 3,934 µs | 3,395.6 |

**Lev BitParallel** (default) — Myers' bit-parallel NFA. Zero startup, ~12 bitwise ops per FST node. Best for single-shot queries and interactive per-keystroke use.

**Lev DFA** — Compiled Levenshtein DFA with O(1) table lookup per node. 3x faster traversal than BitParallel, but DFA compilation adds upfront cost (135 µs at d=1, 3,479 µs at d=4). Breaks even after ~3 queries at d=1. Best for batch workloads searching the same (query, distance) pair across multiple indices.

**Damerau DFA** — Compiled Damerau-Levenshtein DFA. Same fast O(1) traversal as Lev DFA, but DFA compilation is ~10x more expensive because the transposition recurrence expands the state space. Use when transposition handling is needed (e.g., "teh" → "the" at d=1).

### Comparison: FuzzyFST vs SymSpell vs Brute Force

All three methods tested on the same 370,105-word dictionary and the same 50 queries at edit distances 1 through 4. Queries span five categories: 1-char typos, 2-char typos, 3-char typos, common misspellings, and words not in dictionary. Full query set in [data/benchmark_queries.txt](data/benchmark_queries.txt). Same hardware and compiler as above.

| Metric | FuzzyFST (Lev BP) | FuzzyFST (Lev DFA) | FuzzyFST (Dam DFA)† | SymSpell | Brute Force |
|--------|-------------------|--------------------|--------------------|----------|-------------|
| Index size | 1.9 MB | 1.9 MB | 1.9 MB | 215 / 593 / 1,110 / 2,534 MB | 0 |
| Build time | 93 ms | 93 ms | 93 ms | 1,270 / 4,411 / 9,147 / 22,393 ms | 0 ms |
| Avg latency d=1 | 53 µs | 17 µs | 21 µs | 10 µs | 72,218 µs |
| Avg latency d=2 | 498 µs | 168 µs | 209 µs | 143 µs | 76,592 µs |
| Avg latency d=3 | 2,525 µs | 924 µs | 989 µs | 2,089 µs | 83,989 µs |
| Avg latency d=4 | 6,928 µs | 2,707 µs | 2,791 µs | 11,988 µs | 75,996 µs |
| P99 latency d=1 | 76 µs | 30 µs | 34 µs | 27 µs | 84,920 µs |
| P99 latency d=2 | 735 µs | 253 µs | 314 µs | 429 µs | 95,691 µs |
| P99 latency d=3 | 3,497 µs | 1,206 µs | 1,401 µs | 5,325 µs | 182,169 µs |
| P99 latency d=4 | 9,380 µs | 4,121 µs | 3,934 µs | 27,151 µs | 105,438 µs |
| Avg results d=1 | 1.9 | 1.9 | 2.1 | 1.9* | 1.9 |
| Avg results d=2 | 39.2 | 39.2 | 40.9 | 39.2* | 39.2 |
| Avg results d=3 | 471.9 | 471.9 | 487.7 | 471.9* | 471.9 |
| Avg results d=4 | 3,320.4 | 3,320.4 | 3,395.6 | 3,221.4* | 3,320.4 |

† Damerau DFA latencies are traversal-only. DFA compilation adds a one-time cost per (query, distance) pair: 1,447 / 4,275 / 13,648 / 41,501 µs at d=1/2/3/4. Lev DFA compilation is much cheaper: 135 / 438 / 1,200 / 3,479 µs. The DFA should be compiled once and reused across multiple lookups.

\* SymSpell uses Damerau-Levenshtein distance (transpositions count as one edit), while the Lev BP/DFA and brute force columns use pure Levenshtein distance. At d=1-3 the result counts agree because transpositions are rare in this query set. At d=4, the small difference (~3%) reflects words where a transposition changes which side of the distance boundary they fall on.

**SymSpell** uses the symmetric delete algorithm: it precomputes all deletion variants up to max edit distance at build time, trading memory for query speed. At d=1 and d=2, SymSpell is faster than Lev BitParallel because its lookups are hash table probes with no graph traversal. However, using the Lev DFA mode narrows the gap significantly (17 µs vs 10 µs at d=1). Memory cost remains massive: 215 MB at d=1 (113x FuzzyFST), 593 MB at d=2 (312x), 1.1 GB at d=3 (584x), and 2.5 GB at d=4 (1,334x). Build times are 14-241x slower. At d=3, FuzzyFST's DFA modes are faster than SymSpell on both average and P99 latency. At d=4, FuzzyFST DFA is 4.4x faster on average and 6.6x better P99.

**FuzzyFST** stores the entire dictionary in a 1.9 MB minimized FST that can be memory-mapped from disk. Query speed scales with edit distance (more nodes to traverse) but memory usage is constant regardless of the edit distance, metric, or algorithm used at query time. Build time is 93 ms — a single build supports all edit distances, both metrics, and all algorithm modes. The DFA modes provide ~3x faster traversal than BitParallel at the cost of per-query DFA compilation, which must be amortized across multiple lookups.

**Brute force** computes Levenshtein distance against every dictionary word. At ~75 ms per query, it is 1,400x slower than FuzzyFST at d=1 and 13x slower at d=4. Its latency is nearly constant across distances because it always scans the full dictionary.

**Distance 3+** is supported but produces increasingly large result sets (~472 at d=3, ~3,320 at d=4). FuzzyFST is a candidate generation library — it returns all dictionary words within the specified edit distance. Ranking results by likelihood or relevance is left to the caller. At higher distances, downstream ranking becomes the bottleneck rather than the search itself.

### Choosing a mode

Three axes of choice: **metric** (Levenshtein vs Damerau), **algorithm** (BitParallel vs DFA), and **edit distance**.

**Levenshtein BitParallel** (default) — Zero startup, ~12 bitwise ops per node. Best for single-shot queries, per-keystroke interactive use, and any workload where each query string is different. Use this unless you have a specific reason to switch.

**Levenshtein DFA** — 3x faster traversal than BitParallel, but DFA compilation adds 135-3,479 µs (d=1-4). Breaks even after ~3 queries at d=1. Best for batch workloads where the same (query, distance) pair is searched across multiple indices, or when amortizing DFA cost over many lookups in a session.

**Damerau-Levenshtein DFA** — Same fast traversal as Lev DFA, but counts adjacent transpositions as a single edit (e.g., "teh" → "the" at d=1). DFA compilation is ~10x more expensive than Lev DFA because transpositions expand the state space. Use for spell checking and user input correction where transpositions are a common error class.

## Algorithm

The query engine intersects an automaton (NFA or DFA) with the FST via iterative DFS.

**BitParallel mode** uses Myers' bit-parallel algorithm, advancing the NFA state in O(1) per FST node using two 64-bit bitvectors (Pv, Mv) that encode the entire DP column implicitly. The pruning function `can_match()` reconstructs the column minimum from these bitvectors and prunes subtrees where no extension can yield a match.

**DFA modes** precompile a deterministic finite automaton via BFS over reachable DP states. Each (state, byte) transition is a single array lookup — O(1) with lower constant than the bitvector computation. The DFA also precomputes `can_reach_accept` for every state via backward BFS from accepting states, enabling O(1) pruning. The Damerau DFA tracks two DP columns plus the previous input character to implement the transposition recurrence `D[i][j] = D[i-2][j-2] + 1`.

Key implementation details are documented in [INTERNALS.md](INTERNALS.md), including:
- Assembly codegen analysis (`step()` compiles to branchless sbb/adc)
- Why full DP column reconstruction in `can_match()` is a correctness requirement
- Per-function profiling breakdown (decode/step/can_match split ~33% each)
- Cache behavior analysis
- DFA compilation details and state space analysis

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
