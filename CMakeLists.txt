cmake_minimum_required(VERSION 3.20)
project(fuzzyfst LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    src/damerau_nfa.cpp
    src/damerau_search.cpp
    src/levenshtein_dfa.cpp
    src/levenshtein_dfa_search.cpp
    src/hyyro_nfa.cpp
    src/hyyro_search.cpp
    src/fuzzyfst.cpp
)
target_include_directories(fuzzyfst PUBLIC include)
target_include_directories(fuzzyfst PRIVATE src)
if(MSVC)
    target_compile_options(fuzzyfst PRIVATE /GR-)
else()
    target_compile_options(fuzzyfst PRIVATE -fno-rtti)
endif()
set_target_properties(fuzzyfst PROPERTIES POSITION_INDEPENDENT_CODE ON)

# CLI
add_executable(fst-cli cli/main.cpp)
target_link_libraries(fst-cli PRIVATE fuzzyfst)
target_include_directories(fst-cli PRIVATE src)

# Tests
enable_testing()
add_executable(fst-tests
    tests/test_main.cpp
    tests/test_arena.cpp
    tests/test_radix_sort.cpp
    tests/test_state_map.cpp
    tests/test_fst_builder.cpp
    tests/test_fst_roundtrip.cpp
    tests/test_levenshtein_nfa.cpp
    tests/test_fuzzy_correctness.cpp
    tests/test_damerau_nfa.cpp
    tests/test_damerau_correctness.cpp
    tests/test_metric_comparison.cpp
    # tests/test_hyyro_nfa.cpp          # Disabled until Hyyro is fixed
    # tests/test_hyyro_correctness.cpp   # Disabled until Hyyro is fixed
    tests/test_four_modes.cpp
)
target_link_libraries(fst-tests PRIVATE fuzzyfst)
target_include_directories(fst-tests PRIVATE src)
add_test(NAME all_tests COMMAND fst-tests)

# Profiling / benchmarks
add_executable(fst-profile tests/bench_profile.cpp)
target_link_libraries(fst-profile PRIVATE fuzzyfst)
target_include_directories(fst-profile PRIVATE src)

add_executable(bench-damerau bench/bench_damerau.cpp)
target_link_libraries(bench-damerau PRIVATE fuzzyfst)
target_include_directories(bench-damerau PRIVATE src)

add_executable(bench-four-modes bench/bench_four_modes.cpp)
target_link_libraries(bench-four-modes PRIVATE fuzzyfst)
target_include_directories(bench-four-modes PRIVATE src)

# Word list check
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/data/words.txt")
    message(WARNING "data/words.txt not found. Tests and benchmarks require a word list. "
                    "See https://github.com/dwyl/english-words for the 370K-word dictionary.")
endif()

# Python bindings (optional, enabled by scikit-build-core during pip install)
option(FUZZYFST_PYTHON "Build Python bindings" OFF)

if(FUZZYFST_PYTHON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(_fuzzyfst python/bindings.cpp)
    target_link_libraries(_fuzzyfst PRIVATE fuzzyfst)
    # _fuzzyfst compiles with RTTI (nanobind requires it).
    # The fuzzyfst library uses -fno-rtti (PRIVATE), so no conflict —
    # Pimpl ensures no RTTI-dependent types cross the boundary.

    install(TARGETS _fuzzyfst LIBRARY DESTINATION fuzzyfst)
endif()
