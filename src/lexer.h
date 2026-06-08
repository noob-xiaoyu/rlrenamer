#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace rlren {

struct RewriteStats {
    std::size_t replacements = 0;
    // Optional per-identifier replacement counts
    std::unordered_map<std::string, std::size_t> perIdentifier;
};

// Performs token-level identifier replacement for C/C++ code.
// - Replaces identifiers only (not inside comments/strings/char literals/raw strings)
// - Preserves all other characters exactly.
std::string RewriteWithIdentifierMap(
    const std::string& input,
    const std::unordered_map<std::string, std::string>& renameMap
);

// Same as above, but optionally collects replacement counts.
std::string RewriteWithIdentifierMap(
    const std::string& input,
    const std::unordered_map<std::string, std::string>& renameMap,
    RewriteStats* stats
);

// Macro-aware rewrite: identifiers in `macroNames` are only renamed in
// preprocessor directive positions (#define, #ifdef, #ifndef, #undef, defined()).
// Other identifiers are renamed everywhere as usual.
// When macroNames is empty, falls back to the standard RewriteWithIdentifierMap behavior.
std::string RewriteWithMacroAwareness(
    const std::string& input,
    const std::unordered_map<std::string, std::string>& renameMap,
    RewriteStats* stats,
    const std::unordered_set<std::string>& macroNames
);

} // namespace rlren
