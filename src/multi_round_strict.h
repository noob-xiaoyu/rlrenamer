#pragma once

#include "raylib_api_extract.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace rlren {

// Multi-round strict check: approximate MSVC include order at file granularity.
//
// Goal: detect names that are defined by the consumer project in headers included
// *before* a given file is expanded into translation units. If such a name is also
// a raylib symbol, rewriting unqualified occurrences is risky.
//
// The result maps each file (relative path under root) to a set of identifiers
// that may be shadowed by includer context, with a reviewer-friendly detail string.
struct MultiRoundStrictCheckResult {
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> includerShadowed;

    std::size_t translationUnits = 0;
    std::size_t filesWithConflicts = 0;
    std::size_t totalConflicts = 0;
};

MultiRoundStrictCheckResult RunMultiRoundStrictCheck(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& projectFiles,
    const std::vector<std::string>& excludeDirNames,
    const ApiSymbolDB& raylibDb
);

// Writes a TSV report: file\tident\tdetail
void WriteMultiRoundStrictReportTsv(
    const std::filesystem::path& outPath,
    const MultiRoundStrictCheckResult& r
);

} // namespace rlren
