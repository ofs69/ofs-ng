#pragma once

#include <initializer_list>
#include <string_view>

// Unified UTF-8-aware fuzzy matcher shared by every "type-to-filter" surface (command palette,
// add-node menu, theme filter, shortcut filter). Case- AND diacritic-insensitive: "offnen" matches
// both "Öffnen" and "öffnen". Allocation-free — takes std::string_view, returns a POD — so it is safe
// to call in per-frame render paths (hot-path rule). The matching is a scored subsequence:
// every needle codepoint must occur, in order, somewhere in the haystack; the score rewards
// consecutive / prefix / word-boundary hits so the best matches sort first.

namespace ofs::util {

struct FuzzyResult {
    bool matched = false;
    int score = 0; // higher = better; meaningful only when `matched`
};

// Scored UTF-8 subsequence match. An empty needle matches everything with score 0 (so an empty filter
// preserves the caller's input order). Invalid UTF-8 bytes degrade to raw single bytes, never crash.
FuzzyResult fuzzyMatch(std::string_view needle, std::string_view haystack);

// Best score across several candidate haystacks — for sites that match an item against more than one
// field (the add-node menu's name/category, the shortcut window's title/group/id). matched is true if
// ANY field matches; score is the maximum over the matching fields.
FuzzyResult fuzzyMatchAny(std::string_view needle, std::initializer_list<std::string_view> haystacks);

// Variant that also records the haystack BYTE offset of each matched codepoint into a caller-supplied
// buffer (for future match highlighting). Writes at most `maxMatches` offsets and sets `*outMatchCount`
// to the number written. Not wired into any renderer yet — provided so highlighting needs no second
// rewrite of the matcher later. `outMatchByteOffsets`/`outMatchCount` may be null to ignore positions.
FuzzyResult fuzzyMatch(std::string_view needle, std::string_view haystack, int *outMatchByteOffsets, int maxMatches,
                       int *outMatchCount);

} // namespace ofs::util
