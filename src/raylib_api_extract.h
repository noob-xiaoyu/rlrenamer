#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rlren {

struct ApiExtractOptions {
    // Function specifier macros in raylib headers (raylib.h: RLAPI, raymath.h: RMAPI, etc.)
    // If any of these identifiers appears before a function declaration/definition,
    // we treat the identifier immediately before the first '(' as the function name.
    //
    // Defaults cover the most common raylib headers.
    std::vector<std::string> apiDefines = {"RLAPI", "RMAPI", "RGAPI"};

    // Optional symbol categories.
    // NOTE: Renaming #define macros can break user-controlled build flags; keep it off unless you
    // really want to prefix API macros/constants.
    bool includeDefineMacros = false;         // #define MACRO
    bool includeTypedefAliases = true;        // typedef ... Alias;
    bool includeEnums = true;                 // typedef enum { A, B } Name;
    bool includeEnumValues = false;           // A, B, ... inside enums
    bool skipAlreadyPrefixedRL = true;        // skip names starting with RL or rl
    bool skipLeadingUnderscore = true;        // skip names starting with _
    bool requireLeadingUppercase = true;      // keep only names whose first char is [A-Z]
};

enum class ApiSymbolKind {
    Function,
    TypedefStruct,
    TypedefEnum,
    TypedefAlias,
    DefineMacro,
    EnumValue,
    Unknown
};

struct ApiSymbolInfo {
    std::string name;
    ApiSymbolKind kind = ApiSymbolKind::Unknown;

    // Function signature hints (best-effort, lexer-based)
    int paramMin = -1;       // -1 = unknown
    bool varargs = false;

    // Best-effort declaration snippet (single-line, trimmed). Optional.
    std::string decl;

    // Optional origin info (file path relative to the scanned root, if known)
    std::string origin;
    int originLine = -1;
};

using ApiSymbolDB = std::unordered_map<std::string, ApiSymbolInfo>;

// Extract a symbol DB from a raylib-style header.
// Conservative by design: avoids struct field names, tries to capture public typedefs and RLAPI-style functions.
ApiSymbolDB ExtractRaylibApiSymbolDBFromHeader(
    const std::string& headerText,
    const ApiExtractOptions& opt,
    const std::string& originLabel = ""
);

// Optional: Extract additional symbols from C/C++ source text by looking for
// top-level function definitions (braceDepth==0).
// This is conservative. Signature info may be unknown.
ApiSymbolDB ExtractTopLevelFunctionDBFromSource(
    const std::string& sourceText,
    const ApiExtractOptions& opt,
    const std::string& originLabel = ""
);

// Legacy helpers (name-only), kept for backward compatibility.
std::unordered_set<std::string> ExtractRaylibApiSymbolsFromHeader(
    const std::string& headerText,
    const ApiExtractOptions& opt
);

std::unordered_set<std::string> ExtractTopLevelFunctionDefinitionsFromSource(
    const std::string& sourceText,
    const ApiExtractOptions& opt
);

} // namespace rlren
