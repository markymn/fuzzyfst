#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yams::symspell {

enum class Verbosity { Top, Closest, All };

struct Suggestion {
    std::string term;
    int distance;
    int64_t frequency;

    bool operator==(const Suggestion&) const = default;
};

class ISymSpellStore {
public:
    virtual ~ISymSpellStore() = default;

    virtual void addDelete(int hash, std::string_view term) = 0;
    virtual std::vector<std::string> getTerms(int hash) = 0;
    virtual void setFrequency(std::string_view term, int64_t freq) = 0;
    virtual std::optional<int64_t> getFrequency(std::string_view term) = 0;
    virtual bool termExists(std::string_view term) = 0;
};

class MemoryStore : public ISymSpellStore {
public:
    explicit MemoryStore(int = 2, int = 7) {}

    void addDelete(int hash, std::string_view term) override {
        deletes_[hash].push_back(std::string(term));
    }

    std::vector<std::string> getTerms(int hash) override {
        auto it = deletes_.find(hash);
        if (it != deletes_.end()) {
            return it->second;
        }
        return {};
    }

    void setFrequency(std::string_view term, int64_t freq) override {
        words_[std::string(term)] = freq;
    }

    std::optional<int64_t> getFrequency(std::string_view term) override {
        auto it = words_.find(std::string(term));
        if (it != words_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool termExists(std::string_view term) override { return words_.contains(std::string(term)); }

private:
    std::unordered_map<int, std::vector<std::string>> deletes_;
    std::unordered_map<std::string, int64_t> words_;
};

class SymSpell {
public:
    SymSpell(std::unique_ptr<ISymSpellStore> store, int maxEditDistance = 2, int prefixLength = 7)
        : store_(std::move(store)), maxEditDistance_(maxEditDistance), prefixLength_(prefixLength),
          compactMask_(calculateCompactMask(5)), maxDictionaryWordLength_(0) {}

    bool createDictionaryEntry(std::string_view key, int64_t count = 1) {
        if (count <= 0) {
            return false;
        }

        auto it = belowThresholdWords_.find(std::string(key));
        if (it != belowThresholdWords_.end()) {
            count = saturatingAdd(it->second, count);
            if (count >= countThreshold_) {
                belowThresholdWords_.erase(it);
            } else {
                belowThresholdWords_[std::string(key)] = count;
                return false;
            }
        } else {
            auto freq = store_->getFrequency(key);
            if (freq.has_value()) {
                count = saturatingAdd(*freq, count);
                store_->setFrequency(key, count);
                return false;
            } else if (count < countThreshold_) {
                belowThresholdWords_[std::string(key)] = count;
                return false;
            }
        }

        store_->setFrequency(key, count);
        if (key.size() > static_cast<size_t>(maxDictionaryWordLength_)) {
            maxDictionaryWordLength_ = static_cast<int>(key.size());
        }

        auto edits = editsPrefix(key);
        for (const auto& deleteWord : edits) {
            int deleteHash = getStringHash(deleteWord);
            store_->addDelete(deleteHash, key);
        }

        return true;
    }

    std::vector<Suggestion> lookup(std::string_view input, Verbosity verbosity = Verbosity::Closest,
                                   int maxEditDistance = -1) const {
        if (maxEditDistance < 0) {
            maxEditDistance = maxEditDistance_;
        }

        if (maxEditDistance > maxEditDistance_) {
            maxEditDistance = maxEditDistance_;
        }

        std::vector<Suggestion> suggestions;
        int inputLen = static_cast<int>(input.size());

        // Early exit if input is too long for any dictionary word
        // Skip this check if maxDictionaryWordLength_ is 0 (not yet computed, e.g., loaded from DB)
        if (maxDictionaryWordLength_ > 0 && inputLen - maxEditDistance > maxDictionaryWordLength_) {
            return suggestions;
        }

        auto exactFreq = store_->getFrequency(input);
        if (exactFreq.has_value()) {
            suggestions.push_back({std::string(input), 0, *exactFreq});
            if (verbosity != Verbosity::All) {
                return suggestions;
            }
        }

        if (maxEditDistance == 0) {
            return suggestions;
        }

        std::unordered_set<std::string> consideredDeletes;
        std::unordered_set<std::string> consideredSuggestions;
        consideredSuggestions.insert(std::string(input));

        int maxEditDistance2 = maxEditDistance;
        std::vector<std::string> candidates;
        int inputPrefixLen = inputLen;

        if (inputPrefixLen > prefixLength_) {
            inputPrefixLen = prefixLength_;
        }
        candidates.push_back(std::string(input.substr(0, inputPrefixLen)));

        size_t candidatePointer = 0;

        while (candidatePointer < candidates.size()) {
            // Copy the candidate to avoid iterator invalidation when push_back reallocates
            std::string candidate = candidates[candidatePointer++];
            int candidateLen = static_cast<int>(candidate.size());
            int lengthDiff = inputPrefixLen - candidateLen;

            if (lengthDiff > maxEditDistance2) {
                if (verbosity == Verbosity::All) {
                    continue;
                }
                break;
            }

            int deleteHash = getStringHash(candidate);
            auto dictTerms = store_->getTerms(deleteHash);

            for (const auto& suggestion : dictTerms) {
                if (suggestion == input) {
                    continue;
                }

                int suggestionLen = static_cast<int>(suggestion.size());

                if (std::abs(suggestionLen - inputLen) > maxEditDistance2) {
                    continue;
                }

                if (suggestionLen < candidateLen) {
                    continue;
                }

                if (suggestionLen == candidateLen && suggestion != candidate) {
                    continue;
                }

                int suggPrefixLen = std::min(suggestionLen, prefixLength_);
                if (suggPrefixLen > inputPrefixLen &&
                    (suggPrefixLen - candidateLen) > maxEditDistance2) {
                    continue;
                }

                if (!deleteInSuggestionPrefix(candidate, suggestion)) {
                    continue;
                }

                if (!consideredSuggestions.insert(suggestion).second) {
                    continue;
                }

                int distance = damerauLevenshteinDistance(input, suggestion, maxEditDistance2);
                if (distance < 0 || distance > maxEditDistance2) {
                    continue;
                }

                auto freq = store_->getFrequency(suggestion);
                int64_t suggestionFreq = freq.value_or(0);

                if (verbosity == Verbosity::Top) {
                    if (suggestions.empty()) {
                        maxEditDistance2 = distance;
                        suggestions.push_back({suggestion, distance, suggestionFreq});
                    } else if (distance < maxEditDistance2 ||
                               (distance == maxEditDistance2 &&
                                suggestionFreq > suggestions[0].frequency)) {
                        maxEditDistance2 = distance;
                        suggestions[0] = {suggestion, distance, suggestionFreq};
                    }
                } else if (verbosity == Verbosity::Closest) {
                    if (distance < maxEditDistance2) {
                        suggestions.clear();
                        maxEditDistance2 = distance;
                        suggestions.push_back({suggestion, distance, suggestionFreq});
                    } else if (distance == maxEditDistance2) {
                        suggestions.push_back({suggestion, distance, suggestionFreq});
                    }
                } else {
                    suggestions.push_back({suggestion, distance, suggestionFreq});
                }
            }

            if (lengthDiff < maxEditDistance_ && candidateLen <= prefixLength_) {
                if (verbosity != Verbosity::All && lengthDiff >= maxEditDistance2) {
                    continue;
                }

                for (int i = 0; i < candidateLen; ++i) {
                    std::string deleteWord = candidate;
                    deleteWord.erase(i, 1);

                    if (consideredDeletes.insert(deleteWord).second) {
                        candidates.push_back(std::move(deleteWord));
                    }
                }
            }
        }

        if (verbosity != Verbosity::All && !suggestions.empty()) {
            std::sort(suggestions.begin(), suggestions.end(),
                      [](const Suggestion& a, const Suggestion& b) {
                          if (a.distance != b.distance) {
                              return a.distance < b.distance;
                          }
                          return a.frequency > b.frequency;
                      });

            if (verbosity == Verbosity::Closest) {
                int minDist = suggestions[0].distance;
                suggestions.erase(std::remove_if(suggestions.begin() + 1, suggestions.end(),
                                                 [minDist](const Suggestion& s) {
                                                     return s.distance != minDist;
                                                 }),
                                  suggestions.end());
            }
        }

        return suggestions;
    }

    void setCountThreshold(int64_t threshold) { countThreshold_ = threshold; }

    int maxEditDistance() const { return maxEditDistance_; }
    int prefixLength() const { return prefixLength_; }
    int maxWordLength() const { return maxDictionaryWordLength_; }

private:
    static int64_t saturatingAdd(int64_t lhs, int64_t rhs) {
        if (rhs <= 0) {
            return lhs;
        }
        if (lhs >= INT64_MAX - rhs) {
            return INT64_MAX;
        }
        return lhs + rhs;
    }

    static uint32_t calculateCompactMask(int compactLevel) {
        if (compactLevel > 16) {
            return 0xFFFFFFFF;
        }
        return (0xFFFFFFFF >> (3 + compactLevel)) << 2;
    }

    static int getStringHash(std::string_view s) {
        uint32_t hash = 2166136261u;
        size_t len = s.size();
        size_t lenMask = len > 3 ? 3 : len;

        for (char c : s) {
            hash ^= static_cast<uint32_t>(c);
            hash *= 16777619u;
        }

        return static_cast<int>((hash & 0xFFFFFFFFu) | lenMask);
    }

    std::vector<std::string> editsPrefix(std::string_view key) const {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;

        if (static_cast<int>(key.size()) <= maxEditDistance_) {
            result.push_back("");
            seen.insert("");
        }

        std::string prefix = std::string(key);
        if (prefix.size() > static_cast<size_t>(prefixLength_)) {
            prefix = prefix.substr(0, prefixLength_);
        }

        result.push_back(prefix);
        seen.insert(prefix);

        edits(prefix, 0, result, seen);

        return result;
    }

    void edits(std::string_view word, int editDistance, std::vector<std::string>& result,
               std::unordered_set<std::string>& seen) const {
        editDistance++;

        if (editDistance > maxEditDistance_) {
            return;
        }

        for (size_t i = 0; i < word.size(); ++i) {
            std::string deleteWord = std::string(word);
            deleteWord.erase(i, 1);

            if (seen.insert(deleteWord).second) {
                result.push_back(deleteWord);
                edits(deleteWord, editDistance, result, seen);
            }
        }
    }

    static bool deleteInSuggestionPrefix(std::string_view deleteWord, std::string_view suggestion) {
        if (deleteWord.empty()) {
            return true;
        }

        size_t suggLen = std::min(suggestion.size(), static_cast<size_t>(7));
        size_t delLen = deleteWord.size();

        size_t j = 0;
        for (size_t i = 0; i < delLen; ++i) {
            char delChar = deleteWord[i];
            while (j < suggLen && delChar != suggestion[j]) {
                ++j;
            }
            if (j == suggLen) {
                return false;
            }
        }

        return true;
    }

    static int damerauLevenshteinDistance(std::string_view s1, std::string_view s2,
                                          int maxDistance) {
        int len1 = static_cast<int>(s1.size());
        int len2 = static_cast<int>(s2.size());

        if (std::abs(len1 - len2) > maxDistance) {
            return maxDistance + 1;
        }

        std::vector<int> previous(len2 + 1);
        std::vector<int> current(len2 + 1);

        for (int j = 0; j <= len2; ++j) {
            previous[j] = j;
        }

        for (int i = 1; i <= len1; ++i) {
            current[0] = i;
            int minRow = i;

            for (int j = 1; j <= len2; ++j) {
                int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
                current[j] =
                    std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});

                if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
                    current[j] = std::min(current[j], previous[len2] + 1);
                }

                minRow = std::min(minRow, current[j]);
            }

            if (minRow > maxDistance) {
                return maxDistance + 1;
            }

            std::swap(previous, current);
        }

        int distance = previous[len2];
        return (distance > maxDistance) ? maxDistance + 1 : distance;
    }

    std::unique_ptr<ISymSpellStore> store_;
    int maxEditDistance_;
    int prefixLength_;
    uint32_t compactMask_;
    int maxDictionaryWordLength_;
    int64_t countThreshold_ = 1;
    std::unordered_map<std::string, int64_t> belowThresholdWords_;
};

} // namespace yams::symspell
