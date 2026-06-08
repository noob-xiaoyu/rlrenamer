#pragma once

#include "raylib_api_extract.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rlren {

struct ConsumerModeOptions {
    bool skipQualified = true;          // skip identifiers preceded by ., ->, ::
    bool skipPreprocessor = true;       // skip identifiers inside preprocessor directives
    bool validateFunctionCallArity = true; // compare arg count to raylib signature hint (if known)
    bool skipShadowedNames = true;      // skip any symbol name that is defined in the same file (heuristic)

    bool replaceSuspicious = false;     // if true, replace even when suspicious (still reported)

    // If set, preprocessor directive identifiers may be rewritten.
    bool rewritePreprocessor = false;

    // How many suspicious samples to keep per file (for context export)
    int maxSamplesPerFile = 24;
};

struct SuspiciousOccurrence {
    std::string ident;
    std::string reason;
    // Optional free-form detail for reviewers (e.g., conflict origin).
    std::string detail;
    int line = 0;
    int col = 0;
};

struct ConsumerFileAnalysis {
    // Names that appear to be locally defined (shadowing raylib) in this file.
    std::unordered_set<std::string> shadowed;

    // Suspicious occurrences encountered during rewrite.
    std::vector<SuspiciousOccurrence> suspicious;

    // Replacement counts
    std::size_t replacements = 0;
    std::unordered_map<std::string, std::size_t> perIdent;
};

struct ConsumerProjectContext {
    // Suggested exclude dirs under root
    std::vector<std::string> suggestedExcludeDirs;

    // Per-file analysis (relative paths)
    std::unordered_map<std::string, ConsumerFileAnalysis> files;

    // Totals
    std::size_t totalFiles = 0;
    std::size_t changedFiles = 0;
    std::size_t totalReplacements = 0;
    std::size_t totalSuspicious = 0;
    std::size_t totalShadowedFiles = 0;
};

// Analyze a consumer file for shadowing (local definitions) of raylib symbol names.
std::unordered_set<std::string> DetectShadowedNamesInConsumerFile(
    const std::string& text,
    const ApiSymbolDB& raylibDb
);

// Rewrite a consumer file with validation heuristics.
std::string RewriteConsumerWithValidation(
    const std::string& input,
    const ApiSymbolDB& raylibDb,
    const std::string& prefix,
    const std::string& enumPrefix,
    const std::unordered_set<std::string>& shadowedNames,
    // Optional: names that may be shadowed by includer context (multi-round strict check).
    // Map: ident -> detail string (e.g., where it is defined).
    const std::unordered_map<std::string, std::string>* includerShadowed,
    const ConsumerModeOptions& opt,
    ConsumerFileAnalysis* outAnalysis
);

// Heuristic suggestion of exclude directories: returns directory *names* under root.
std::vector<std::string> SuggestConsumerExcludeDirs(const std::vector<std::string>& topLevelDirNames);

// Export consumer project context as JSON (minimal, no external deps).
void WriteConsumerContextJson(const std::string& outPath, const ConsumerProjectContext& ctx);

// Export symbol DB as TSV (name\tkind\tparamMin\tvarargs\torigin\toriginLine\tdecl)
void WriteSymbolDBTsv(const std::string& outPath, const ApiSymbolDB& db);
ApiSymbolDB LoadSymbolDBTsv(const std::string& inPath);

} // namespace rlren
