# FuzzyFST

Fast fuzzy string search using finite state transducers. Sub-millisecond approximate string matching on dictionaries with hundreds of thousands of words.

## Install

```bash
pip install fuzzyfst
```

Prebuilt wheels for Python 3.10-3.13 on Windows, macOS (Intel + ARM), and Linux.

## Quick start

```python
import fuzzyfst

# Build an index from a word list.
words = ["algorithm", "algorism", "algebra", "cat", "car", "cart", "card"]
fuzzyfst.build("index.fst", words)

# Open the index (zero-copy memory-mapped).
fst = fuzzyfst.Fst.open("index.fst")

# Exact lookup.
fst.contains("algorithm")   # True
fst.contains("algoritm")    # False

# Fuzzy search — find all words within edit distance 1.
results = fst.fuzzy_search("algoritm", 1)
# [("algorism", 1), ("algorithm", 1)]

# Higher edit distance finds more results.
results = fst.fuzzy_search("car", 2)
# [("car", 0), ("card", 1), ("cart", 1), ("cat", 1)]

# Compiled DFA — faster traversal, higher startup cost.
results = fst.fuzzy_search("algoritm", 2, algorithm="dfa")

# Damerau-Levenshtein: transpositions count as one edit.
results = fst.fuzzy_search("teh", 1, metric="damerau")
# Finds "the" at distance 1 (transposition).
```

## Use cases

- **Spell checking**: look up a word with `contains()`, suggest corrections with `fuzzy_search()`.
- **Autocomplete with typo tolerance**: match user input against a dictionary even when misspelled.
- **Search-as-you-type**: find candidate matches in sub-millisecond time for interactive UIs.
- **Data deduplication**: find near-duplicate strings in datasets.
- **Bioinformatics**: approximate matching on sequence databases.

## API

### `fuzzyfst.build(output_path, words, sort_input=True)`

Build an FST index file from a list of words.

- `output_path` — path to write the `.fst` file.
- `words` — list of strings.
- `sort_input` — if `True` (default), sorts the input automatically.

### `fuzzyfst.Fst.open(path)`

Open a previously-built FST file. The file is memory-mapped for zero-copy access.

### `fst.contains(key) -> bool`

Exact membership test. O(key length), independent of dictionary size.

### `fst.fuzzy_search(query, max_distance=1, metric="levenshtein", algorithm="bit-parallel") -> list[tuple[str, int]]`

Find all words within `max_distance` edits of `query`. Returns a list of `(word, distance)` tuples.

- `metric` — `"levenshtein"` (default) or `"damerau"` for Damerau-Levenshtein (transpositions count as one edit).
- `algorithm` — `"bit-parallel"` (default, zero startup) or `"dfa"` (compiled DFA, faster traversal but higher startup cost).
- Max query length: 64 characters.
- Recommended max distance: 1-3. Distance 4+ works but produces large result sets.

### `fst.num_nodes -> int`

Number of nodes in the FST (for diagnostics).

## Performance

Benchmarked on a 370,105-word English dictionary (Intel i5-9600K, GCC 13.2, -O3):

| Distance | Lev BitParallel | Lev DFA | Damerau DFA | Avg results |
|----------|-----------------|---------|-------------|-------------|
| d=1 | 53 us | 17 us | 21 us | 1.9 / 1.9 / 2.1 |
| d=2 | 498 us | 168 us | 209 us | 39.2 / 39.2 / 40.9 |
| d=3 | 2,525 us | 924 us | 989 us | 471.9 / 471.9 / 487.7 |

DFA modes add one-time compilation cost per (query, distance) pair: Lev DFA 135-1,200 us, Damerau DFA 1,447-13,648 us at d=1-3. Index size: 1.9 MB for 370K words. Build time: 93 ms.

## How it works

FuzzyFST stores the dictionary in a minimized acyclic finite state transducer (FST). Three search modes: **bit-parallel** (Myers' NFA, zero startup, ~12 bitwise ops per node), **Levenshtein DFA** (compiled DFA, O(1) table lookup, 3x faster traversal), and **Damerau DFA** (handles transpositions). Subtrees that cannot produce matches are pruned early.

See [INTERNALS.md](https://github.com/markymn/fuzzyfst/blob/main/INTERNALS.md) for algorithm details.

## C++ API

FuzzyFST is a C++23 library with Python bindings. The full C++ API and build instructions are at [github.com/markymn/fuzzyfst](https://github.com/markymn/fuzzyfst).
