// fst-cli — Build and query FST indices.
//
// Usage:
//   fst-cli build <words.txt> <index.fst>
//   fst-cli query <index.fst> <query> [--distance <d>]

#include <fuzzyfst/fuzzyfst.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s build <words.txt> <index.fst>\n"
        "  %s query <index.fst> <query> [--distance <d>]\n"
        "\n"
        "Commands:\n"
        "  build   Build an FST index from a newline-delimited word list.\n"
        "  query   Fuzzy-search the index for words within edit distance d.\n"
        "\n"
        "Options:\n"
        "  --distance <d>  Maximum edit distance (default: 1, max: 3).\n",
        prog, prog);
}

// ── Load word list ──────────────────────────────────────────

static std::vector<std::string> load_words(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "Error: cannot open '%s'\n", path);
        return {};
    }

    std::vector<std::string> words;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            --len;
        if (len > 0) {
            words.emplace_back(line, len);
        }
    }
    std::fclose(f);
    return words;
}

// ── Build command ───────────────────────────────────────────

static int cmd_build(const char* words_path, const char* output_path) {
    auto t0 = std::chrono::steady_clock::now();

    auto words = load_words(words_path);
    if (words.empty()) {
        std::fprintf(stderr, "Error: no words loaded from '%s'\n", words_path);
        return 1;
    }

    auto t1 = std::chrono::steady_clock::now();

    // Convert to string_views for the API.
    std::vector<std::string_view> views;
    views.reserve(words.size());
    for (const auto& w : words) views.push_back(w);

    auto r = fuzzyfst::build(output_path, views);
    if (!r.has_value()) {
        std::fprintf(stderr, "Error: build failed (error %d)\n",
                     static_cast<int>(r.error()));
        return 1;
    }

    auto t2 = std::chrono::steady_clock::now();

    double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double build_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    std::printf("Built FST from %zu words.\n", words.size());
    std::printf("  Load:  %.1f ms\n", load_ms);
    std::printf("  Build: %.1f ms\n", build_ms);
    std::printf("  Output: %s\n", output_path);

    return 0;
}

// ── Query command ───────────────────────────────────────────

static int cmd_query(const char* index_path, const char* query,
                     uint32_t max_distance) {
    auto fst = fuzzyfst::Fst::open(index_path);
    if (!fst.has_value()) {
        std::fprintf(stderr, "Error: cannot open FST '%s' (error %d)\n",
                     index_path, static_cast<int>(fst.error()));
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();

    // Use zero-alloc overload for proper measurement.
    char word_buf[65536];
    fuzzyfst::FuzzyResult result_buf[8192];

    size_t n = fst->fuzzy_search(
        query, max_distance,
        std::span<fuzzyfst::FuzzyResult>(result_buf, 8192),
        std::span<char>(word_buf, sizeof(word_buf)));

    auto t1 = std::chrono::steady_clock::now();
    double query_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    // Sort results by distance, then alphabetically.
    std::sort(result_buf, result_buf + n,
              [](const fuzzyfst::FuzzyResult& a,
                 const fuzzyfst::FuzzyResult& b) {
                  if (a.distance != b.distance) return a.distance < b.distance;
                  return a.word < b.word;
              });

    std::printf("Query: \"%s\" (distance <= %u)\n", query, max_distance);
    std::printf("Found %zu results in %.1f us:\n", n, query_us);

    for (size_t i = 0; i < n; ++i) {
        std::printf("  d=%u  %.*s\n",
                    result_buf[i].distance,
                    static_cast<int>(result_buf[i].word.size()),
                    result_buf[i].word.data());
    }

    return 0;
}

// ── Main ────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    std::string_view cmd = argv[1];

    if (cmd == "build") {
        if (argc != 4) {
            std::fprintf(stderr, "Usage: %s build <words.txt> <index.fst>\n",
                         argv[0]);
            return 1;
        }
        return cmd_build(argv[2], argv[3]);
    }

    if (cmd == "query") {
        if (argc < 4) {
            std::fprintf(stderr,
                "Usage: %s query <index.fst> <query> [--distance <d>]\n",
                argv[0]);
            return 1;
        }

        uint32_t distance = 1;
        for (int i = 4; i < argc; ++i) {
            if (std::strcmp(argv[i], "--distance") == 0 && i + 1 < argc) {
                distance = static_cast<uint32_t>(std::atoi(argv[++i]));
                if (distance > 3) {
                    std::fprintf(stderr,
                        "Warning: distance %u > 3 not recommended.\n",
                        distance);
                }
            }
        }

        return cmd_query(argv[2], argv[3], distance);
    }

    std::fprintf(stderr, "Unknown command: %.*s\n",
                 static_cast<int>(cmd.size()), cmd.data());
    usage(argv[0]);
    return 1;
}
