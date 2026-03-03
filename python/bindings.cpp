// Python bindings for FuzzyFST using nanobind.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <fuzzyfst/fuzzyfst.h>

namespace nb = nanobind;

static void raise_error(fuzzyfst::Error e) {
    switch (e) {
        case fuzzyfst::Error::FileOpenFailed:
            throw nb::value_error("Failed to open file");
        case fuzzyfst::Error::MmapFailed:
            throw nb::value_error("Memory mapping failed");
        case fuzzyfst::Error::InvalidFormat:
            throw nb::value_error("Invalid FST format");
        case fuzzyfst::Error::InputNotSorted:
            throw nb::value_error("Input words are not sorted");
        case fuzzyfst::Error::BufferTooSmall:
            throw nb::value_error("Buffer too small");
        case fuzzyfst::Error::QueryTooLong:
            throw nb::value_error("Query exceeds 64 characters");
        default:
            throw nb::value_error("Unknown error");
    }
}

NB_MODULE(_fuzzyfst, m) {
    m.doc() = "Fast fuzzy string search using finite state transducers";

    m.def("build", [](const std::string& output_path,
                       std::vector<std::string>& words,
                       bool sort_input) {
        std::vector<std::string_view> views;
        views.reserve(words.size());
        for (auto& w : words) views.emplace_back(w);

        fuzzyfst::BuildOptions opts{sort_input};
        auto result = fuzzyfst::build(output_path.c_str(),
                                       std::span{views}, opts);
        if (!result.has_value()) raise_error(result.error());
    },
    nb::arg("output_path"),
    nb::arg("words"),
    nb::arg("sort_input") = true,
    "Build an FST index file from a list of words.");

    nb::class_<fuzzyfst::Fst>(m, "Fst")
        .def_static("open", [](const std::string& path) {
            auto result = fuzzyfst::Fst::open(path.c_str());
            if (!result.has_value()) raise_error(result.error());
            return std::move(*result);
        }, nb::arg("path"),
        "Open a previously-built FST file (memory-mapped).")

        .def("contains", [](const fuzzyfst::Fst& self,
                            const std::string& key) {
            return self.contains(key);
        }, nb::arg("key"),
        "Return True if key is in the FST.")

        .def("fuzzy_search", [](const fuzzyfst::Fst& self,
                                 const std::string& query,
                                 uint32_t max_distance,
                                 const std::string& metric,
                                 const std::string& algorithm) {
            fuzzyfst::DistanceMetric dm = fuzzyfst::DistanceMetric::Levenshtein;
            if (metric == "damerau") {
                dm = fuzzyfst::DistanceMetric::DamerauLevenshtein;
            } else if (metric != "levenshtein") {
                throw nb::value_error("metric must be 'levenshtein' or 'damerau'");
            }

            fuzzyfst::Algorithm algo = fuzzyfst::Algorithm::BitParallel;
            if (algorithm == "dfa") {
                algo = fuzzyfst::Algorithm::DFA;
            } else if (algorithm != "bit-parallel") {
                throw nb::value_error("algorithm must be 'bit-parallel' or 'dfa'");
            }

            fuzzyfst::SearchOptions opts{max_distance, dm, algo};
            auto results = self.fuzzy_search(query, opts);

            // Copy string_view data into Python strings immediately.
            // The C++ convenience overload uses a thread_local buffer
            // that is invalidated on the next call.
            nb::list py_results;
            for (const auto& r : results) {
                py_results.append(nb::make_tuple(
                    nb::str(r.word.data(), r.word.size()),
                    r.distance
                ));
            }
            return py_results;
        },
        nb::arg("query"), nb::arg("max_distance") = 1,
        nb::arg("metric") = "levenshtein",
        nb::arg("algorithm") = "bit-parallel",
        "Find all words within max_distance edits of query.\n"
        "metric: 'levenshtein' (default) or 'damerau' for Damerau-Levenshtein.\n"
        "algorithm: 'bit-parallel' (default) or 'dfa' for compiled DFA.\n"
        "Returns list of (word, distance) tuples.")

        .def_prop_ro("num_nodes", &fuzzyfst::Fst::num_nodes,
                      "Number of nodes in the FST.");
}
