#include "lexer.h"

#include <cctype>

namespace rlren {

static inline bool IsIdentStart(unsigned char c) {
    return std::isalpha(c) || c == '_';
}

static inline bool IsIdentCont(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

// Forward declarations for literal copiers used by CopyPrefixedLiteral.
static size_t CopyStringLiteral(const std::string& s, size_t i, std::string& out);
static size_t CopyCharLiteral(const std::string& s, size_t i, std::string& out);
static size_t CopyRawStringLiteral(const std::string& s, size_t i, std::string& out);

// Copy a C/C++ numeric literal (integer/float), including suffixes (e.g. 0L, 1ULL, 1.0f)
// and user-defined literal suffixes (e.g. 10_km). This prevents trailing letters like 'L'
// from being lexed as identifiers.
static size_t CopyNumberLiteral(const std::string& s, size_t i, std::string& out) {
    const size_t n = s.size();
    bool allowExpSign = false;
    while (i < n) {
        char c = s[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '\'' || c == '_') {
            out.push_back(c);
            i++;
            if (c == 'e' || c == 'E' || c == 'p' || c == 'P') allowExpSign = true;
            else allowExpSign = false;
            continue;
        }
        if (allowExpSign && (c == '+' || c == '-')) {
            out.push_back(c);
            i++;
            allowExpSign = false;
            continue;
        }
        break;
    }
    return i;
}

// Copy a possibly-prefixed C++ string/char literal. Supported prefixes: L, u, U, u8, and
// raw-string forms like LR"..." and u8R"...".
static size_t CopyPrefixedLiteral(const std::string& s, size_t i, std::string& out) {
    const size_t n = s.size();

    auto copy_prefix = [&](size_t len) {
        out.append(s.substr(i, len));
        i += len;
    };

    if (i + 1 < n && s[i] == 'u' && s[i + 1] == '8') {
        if (i + 2 < n && s[i + 2] == '"') {
            copy_prefix(2);
            return CopyStringLiteral(s, i, out);
        }
        if (i + 3 < n && s[i + 2] == 'R' && s[i + 3] == '"') {
            copy_prefix(2);
            return CopyRawStringLiteral(s, i, out);
        }
    }

    if (s[i] == 'L' || s[i] == 'u' || s[i] == 'U') {
        if (i + 1 < n && s[i + 1] == '"') {
            copy_prefix(1);
            return CopyStringLiteral(s, i, out);
        }
        if (i + 1 < n && s[i + 1] == '\'') {
            copy_prefix(1);
            return CopyCharLiteral(s, i, out);
        }
        if (i + 2 < n && s[i + 1] == 'R' && s[i + 2] == '"') {
            copy_prefix(1);
            return CopyRawStringLiteral(s, i, out);
        }
    }

    out.push_back(s[i]);
    return i + 1;
}

// Copy a normal C/C++ string literal starting at position i (at quote '"').
// Returns new index after copied segment.
static size_t CopyStringLiteral(const std::string& s, size_t i, std::string& out) {
    const size_t n = s.size();
    out.push_back(s[i]);
    i++;
    bool escaped = false;
    while (i < n) {
        char c = s[i];
        out.push_back(c);
        i++;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') break;
    }
    return i;
}

// Copy a normal C/C++ char literal starting at position i (at quote '\'').
static size_t CopyCharLiteral(const std::string& s, size_t i, std::string& out) {
    const size_t n = s.size();
    out.push_back(s[i]);
    i++;
    bool escaped = false;
    while (i < n) {
        char c = s[i];
        out.push_back(c);
        i++;
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '\'') break;
    }
    return i;
}

// Copy a raw string literal starting at position i (at 'R' and next is '"').
// If the pattern is malformed, it falls back to copying just 'R'.
static size_t CopyRawStringLiteral(const std::string& s, size_t i, std::string& out) {
    const size_t n = s.size();
    const size_t start = i;

    // Expect R"...(
    if (i + 1 >= n || s[i] != 'R' || s[i + 1] != '"') {
        out.push_back(s[i]);
        return i + 1;
    }

    size_t j = i + 2; // after R"
    std::string delim;
    while (j < n && s[j] != '(') {
        // Per standard, delimiter can't contain space, backslash, parens.
        delim.push_back(s[j]);
        j++;
    }
    if (j >= n) {
        // Malformed; just copy the remaining and stop.
        out.append(s.substr(start));
        return n;
    }

    // Now j is '(' position, raw content begins at j+1
    const std::string endMarker = ")" + delim + "\"";
    size_t contentStart = j + 1;
    size_t endPos = s.find(endMarker, contentStart);
    if (endPos == std::string::npos) {
        out.append(s.substr(start));
        return n;
    }

    // Copy from start to end of marker
    out.append(s.substr(start, (endPos - start) + endMarker.size()));
    return endPos + endMarker.size();
}

std::string RewriteWithIdentifierMap(const std::string& input,
                                     const std::unordered_map<std::string, std::string>& renameMap,
                                     RewriteStats* stats) {
    std::string out;
    out.reserve(input.size());

    const size_t n = input.size();
    size_t i = 0;
    while (i < n) {
        char c = input[i];

        // Comments
        if (c == '/' && i + 1 < n) {
            char n1 = input[i + 1];
            if (n1 == '/') {
                // line comment
                out.push_back('/');
                out.push_back('/');
                i += 2;
                while (i < n) {
                    char cc = input[i];
                    out.push_back(cc);
                    i++;
                    if (cc == '\n') break;
                }
                continue;
            }
            if (n1 == '*') {
                // block comment
                out.push_back('/');
                out.push_back('*');
                i += 2;
                while (i < n) {
                    char cc = input[i];
                    out.push_back(cc);
                    i++;
                    if (cc == '*' && i < n && input[i] == '/') {
                        out.push_back('/');
                        i++;
                        break;
                    }
                }
                continue;
            }
        }

        // Prefixed literals: L"...", L'...', u8"...", u"...", U"...", and raw forms like LR"...".
        if ((c == 'L' || c == 'u' || c == 'U') && i + 1 < n) {
            char n1 = input[i + 1];
            if (n1 == '"' || n1 == '\'' || n1 == 'R' || (c == 'u' && n1 == '8')) {
                i = CopyPrefixedLiteral(input, i, out);
                continue;
            }
        }

        // Raw string literal (C++): R"delim( ... )delim"
        if (c == 'R' && i + 1 < n && input[i + 1] == '"') {
            i = CopyRawStringLiteral(input, i, out);
            continue;
        }

        // Standard string literal
        if (c == '"') {
            i = CopyStringLiteral(input, i, out);
            continue;
        }

        // Char literal
        if (c == '\'') {
            i = CopyCharLiteral(input, i, out);
            continue;
        }

        // Numeric literal: consume suffixes so trailing letters (e.g. 'L') aren't rewritten.
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(input[i + 1])))) {
            i = CopyNumberLiteral(input, i, out);
            continue;
        }

        // Identifier
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(static_cast<unsigned char>(input[j]))) j++;
            std::string ident = input.substr(i, j - i);

            // The preprocessor operator `defined` is lexed as an identifier token.
            // Never rewrite it, even if it appears in the rename map.
            if (ident == "defined") {
                out.append(ident);
                i = j;
                continue;
            }

            auto it = renameMap.find(ident);
            if (it != renameMap.end()) {
                out.append(it->second);
                if (stats) {
                    stats->replacements++;
                    stats->perIdentifier[ident]++;
                }
            } else {
                out.append(ident);
            }

            i = j;
            continue;
        }

        // Default
        out.push_back(c);
        i++;
    }

    return out;
}

std::string RewriteWithIdentifierMap(const std::string& input,
                                     const std::unordered_map<std::string, std::string>& renameMap) {
    return RewriteWithIdentifierMap(input, renameMap, nullptr);
}

// ---------------------------------------------------------------------------
// Macro-aware preprocessor handling
// ---------------------------------------------------------------------------

namespace {

static inline bool IsSpaceChar(char c) {
    unsigned char uc = static_cast<unsigned char>(c);
    return uc == ' ' || uc == '\t' || uc == '\r' || uc == '\n' || uc == '\f' || uc == '\v';
}

// Read a complete preprocessor logical line starting at '#'.
// Handles backslash-newline continuations.
// ppText receives the full text; returns the index past the last consumed char.
static size_t ReadPreprocessorLine(const std::string& s, size_t i, std::string& ppText) {
    const size_t n = s.size();
    while (i < n) {
        char c = s[i];
        ppText.push_back(c);
        i++;
        if (c == '\n') {
            // Check for backslash continuation: \ immediately before \n (possibly with \r)
            size_t len = ppText.size();
            if (len >= 2 && ppText[len - 2] == '\r') {
                // \r\n ending — check before \r
                if (len >= 3 && ppText[len - 3] == '\\') continue;
            } else {
                // \n ending — check before \n
                if (len >= 2 && ppText[len - 2] == '\\') continue;
            }
            break;
        }
    }
    return i;
}

// Check if position `idx` in ppText is at a "macro name position" based on
// the directive type.
// Returns true and sets outIdent/outEnd if the position is a macro name site.
static bool IsMacroNamePosition(const std::string& ppText, size_t idx) {
    // Walk backwards from idx to find the start of the preprocessor keyword context.
    // This is a lightweight scan to determine if we're at:
    //   #define <here>
    //   #ifdef <here>
    //   #ifndef <here>
    //   #undef <here>
    //   defined(<here>)  or  defined <here>

    // Skip '#' and whitespace at the very start
    size_t pos = 0;
    while (pos < ppText.size() && ppText[pos] == '#') pos++;
    while (pos < ppText.size() && IsSpaceChar(ppText[pos])) pos++;

    // Read directive keyword
    size_t kwStart = pos;
    while (pos < ppText.size() && (std::isalpha(static_cast<unsigned char>(ppText[pos])) || ppText[pos] == '_')) pos++;
    std::string kw = ppText.substr(kwStart, pos - kwStart);

    // For #define / #ifdef / #ifndef / #undef: the first identifier after the keyword
    if (kw == "define" || kw == "ifdef" || kw == "ifndef" || kw == "undef") {
        // Skip whitespace after keyword
        while (pos < ppText.size() && IsSpaceChar(ppText[pos])) pos++;
        return (pos == idx);
    }

    // For #if / #elif: scan for `defined` keyword
    if (kw == "if" || kw == "elif") {
        // Scan the line for `defined` occurrences; check if idx follows one
        size_t scan = pos;
        while (scan < ppText.size()) {
            // Find "defined"
            if (scan + 7 <= ppText.size() && ppText.substr(scan, 7) == "defined") {
                size_t after = scan + 7;
                // Must be at word boundary
                if (after < ppText.size() && (IsIdentStart(static_cast<unsigned char>(ppText[after])) || IsIdentCont(static_cast<unsigned char>(ppText[after])))) {
                    scan++;
                    continue;
                }
                // Check the chars before "defined" for word boundary
                if (scan > 0 && (IsIdentStart(static_cast<unsigned char>(ppText[scan - 1])) || IsIdentCont(static_cast<unsigned char>(ppText[scan - 1])))) {
                    scan++;
                    continue;
                }

                // Skip whitespace after "defined"
                size_t namePos = after;
                while (namePos < ppText.size() && IsSpaceChar(ppText[namePos])) namePos++;
                // Optional '('
                if (namePos < ppText.size() && ppText[namePos] == '(') {
                    namePos++;
                    while (namePos < ppText.size() && IsSpaceChar(ppText[namePos])) namePos++;
                }
                if (namePos == idx) return true;
                scan = namePos;
                continue;
            }
            scan++;
        }
        return false;
    }

    return false;
}

} // namespace

std::string RewriteWithMacroAwareness(
    const std::string& input,
    const std::unordered_map<std::string, std::string>& renameMap,
    RewriteStats* stats,
    const std::unordered_set<std::string>& macroNames) {

    // Fallback: no macro names → use standard behavior
    if (macroNames.empty()) {
        return RewriteWithIdentifierMap(input, renameMap, stats);
    }

    std::string out;
    out.reserve(input.size());

    const size_t n = input.size();
    size_t i = 0;
    bool atLineStart = true;

    while (i < n) {
        char c = input[i];

        // ---- newline handling ----
        if (c == '\n') {
            out.push_back(c);
            i++;
            atLineStart = true;
            continue;
        }

        // ---- whitespace (non-newline) at line start ----
        if (atLineStart && IsSpaceChar(c)) {
            out.push_back(c);
            i++;
            continue;
        }

        // ---- preprocessor directive detection ----
        if (atLineStart && c == '#') {
            std::string ppText;
            i = ReadPreprocessorLine(input, i, ppText);

            // Process the PP line with macro-aware rules.
            // Walk ppText char by char, same as the main loop below, but with
            // additional logic for macro-name positions.
            {
                size_t pi = 0;
                const size_t pn = ppText.size();
                bool ppExpectMacroName = false;   // next ident is at a macro-name site
                bool ppInDefineParams = false;    // inside #define NAME(...)
                int ppDefineParenDepth = 0;

                while (pi < pn) {
                    char pc = ppText[pi];

                    // ---- comments in PP lines ----
                    if (pc == '/' && pi + 1 < pn) {
                        char n1 = ppText[pi + 1];
                        if (n1 == '/') {
                            out.push_back('/'); out.push_back('/');
                            pi += 2;
                            while (pi < pn) {
                                char cc = ppText[pi];
                                out.push_back(cc);
                                pi++;
                                if (cc == '\n') break;
                            }
                            continue;
                        }
                        if (n1 == '*') {
                            out.push_back('/'); out.push_back('*');
                            pi += 2;
                            while (pi < pn) {
                                char cc = ppText[pi];
                                out.push_back(cc);
                                pi++;
                                if (cc == '*' && pi < pn && ppText[pi] == '/') {
                                    out.push_back('/');
                                    pi++;
                                    break;
                                }
                            }
                            continue;
                        }
                    }

                    // ---- strings in PP lines ----
                    if (pc == '"') {
                        out.push_back(pc);
                        pi++;
                        bool escaped = false;
                        while (pi < pn) {
                            char sc = ppText[pi];
                            out.push_back(sc);
                            pi++;
                            if (escaped) { escaped = false; continue; }
                            if (sc == '\\') { escaped = true; continue; }
                            if (sc == '"') break;
                        }
                        continue;
                    }

                    // ---- char literals in PP lines ----
                    if (pc == '\'') {
                        out.push_back(pc);
                        pi++;
                        bool escaped = false;
                        while (pi < pn) {
                            char sc = ppText[pi];
                            out.push_back(sc);
                            pi++;
                            if (escaped) { escaped = false; continue; }
                            if (sc == '\\') { escaped = true; continue; }
                            if (sc == '\'') break;
                        }
                        continue;
                    }

                    // ---- raw strings in PP lines (unlikely but handled) ----
                    if (pc == 'R' && pi + 1 < pn && ppText[pi + 1] == '"') {
                        size_t start = pi;
                        pi = CopyRawStringLiteral(ppText, pi, out);
                        if (pi == start + 1 && start < pn) {
                            // CopyRawStringLiteral fell back — already handled
                        }
                        continue;
                    }

                    // ---- numeric literal in PP ----
                    if (std::isdigit(static_cast<unsigned char>(pc)) ||
                        (pc == '.' && pi + 1 < pn && std::isdigit(static_cast<unsigned char>(ppText[pi + 1])))) {
                        pi = CopyNumberLiteral(ppText, pi, out);
                        continue;
                    }

                    // ---- identifiers in PP lines ----
                    if (IsIdentStart(static_cast<unsigned char>(pc))) {
                        size_t pj = pi + 1;
                        while (pj < pn && IsIdentCont(static_cast<unsigned char>(ppText[pj]))) pj++;
                        std::string ident = ppText.substr(pi, pj - pi);

                        // In #define NAME(args), don't rename parameters
                        if (ppInDefineParams) {
                            out.append(ident);
                            pi = pj;
                            continue;
                        }

                        // `defined` operator — never rename it, but next ident is macro name
                        if (ident == "defined") {
                            out.append(ident);
                            pi = pj;
                            ppExpectMacroName = true;
                            continue;
                        }

                        // Determine if this position expects a macro name
                        bool atMacroNamePos = ppExpectMacroName || IsMacroNamePosition(ppText, pi);

                        if (atMacroNamePos) {
                            // Macro name position: always try to rename
                            auto it = renameMap.find(ident);
                            if (it != renameMap.end()) {
                                out.append(it->second);
                                if (stats) { stats->replacements++; stats->perIdentifier[ident]++; }
                            } else {
                                out.append(ident);
                            }
                            ppExpectMacroName = false;

                            // After a macro name in #define, check for (...)
                            // Look ahead past whitespace for '('
                            size_t look = pj;
                            while (look < pn && IsSpaceChar(ppText[look])) look++;
                            if (look < pn && ppText[look] == '(') {
                                ppInDefineParams = true;
                                ppDefineParenDepth = 0; // will be set to 1 when we hit '('
                            }
                        } else {
                            // Not a macro name position: skip macro names, rename others
                            if (macroNames.find(ident) != macroNames.end()) {
                                out.append(ident); // macro used outside PP context → keep
                            } else {
                                auto it = renameMap.find(ident);
                                if (it != renameMap.end()) {
                                    out.append(it->second);
                                    if (stats) { stats->replacements++; stats->perIdentifier[ident]++; }
                                } else {
                                    out.append(ident);
                                }
                            }
                        }

                        pi = pj;
                        continue;
                    }

                    // ---- define params parens ----
                    if (ppInDefineParams) {
                        if (pc == '(') ppDefineParenDepth++;
                        else if (pc == ')') {
                            ppDefineParenDepth--;
                            if (ppDefineParenDepth == 0) ppInDefineParams = false;
                        }
                        out.push_back(pc);
                        pi++;
                        continue;
                    }

                    // ---- consumed '(' after 'defined' ----
                    if (pc == '(' && ppExpectMacroName) {
                        out.push_back(pc);
                        pi++;
                        continue;
                    }

                    // ---- default ----
                    out.push_back(pc);
                    pi++;
                }
            }

            atLineStart = true;  // PP line ends with \n, next char is at line start
            continue;
        }

        atLineStart = false;

        // ================================================================
        // Normal (non-PP) code — identical to RewriteWithIdentifierMap
        // ================================================================

        // Comments
        if (c == '/' && i + 1 < n) {
            char n1 = input[i + 1];
            if (n1 == '/') {
                out.push_back('/');
                out.push_back('/');
                i += 2;
                while (i < n) {
                    char cc = input[i];
                    out.push_back(cc);
                    i++;
                    if (cc == '\n') break;
                }
                atLineStart = true;  // line comment consumes \n, next char starts a new line
                continue;
            }
            if (n1 == '*') {
                out.push_back('/');
                out.push_back('*');
                i += 2;
                while (i < n) {
                    char cc = input[i];
                    out.push_back(cc);
                    i++;
                    if (cc == '*' && i < n && input[i] == '/') {
                        out.push_back('/');
                        i++;
                        break;
                    }
                }
                continue;
            }
        }

        // Prefixed literals: L"...", L'...', u8"...", u"...", U"...", and raw forms
        if ((c == 'L' || c == 'u' || c == 'U') && i + 1 < n) {
            char n1 = input[i + 1];
            if (n1 == '"' || n1 == '\'' || n1 == 'R' || (c == 'u' && n1 == '8')) {
                i = CopyPrefixedLiteral(input, i, out);
                continue;
            }
        }

        // Raw string literal R"delim( ... )delim"
        if (c == 'R' && i + 1 < n && input[i + 1] == '"') {
            i = CopyRawStringLiteral(input, i, out);
            continue;
        }

        // Standard string literal
        if (c == '"') {
            i = CopyStringLiteral(input, i, out);
            continue;
        }

        // Char literal
        if (c == '\'') {
            i = CopyCharLiteral(input, i, out);
            continue;
        }

        // Numeric literal
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(input[i + 1])))) {
            i = CopyNumberLiteral(input, i, out);
            continue;
        }

        // Identifier
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(static_cast<unsigned char>(input[j]))) j++;
            std::string ident = input.substr(i, j - i);

            // Never rewrite `defined`
            if (ident == "defined") {
                out.append(ident);
                i = j;
                continue;
            }

            // Macro name in regular code → don't rename
            if (macroNames.find(ident) != macroNames.end()) {
                out.append(ident);
                i = j;
                continue;
            }

            auto it = renameMap.find(ident);
            if (it != renameMap.end()) {
                out.append(it->second);
                if (stats) {
                    stats->replacements++;
                    stats->perIdentifier[ident]++;
                }
            } else {
                out.append(ident);
            }

            i = j;
            continue;
        }

        // Default
        out.push_back(c);
        i++;
    }

    return out;
}

} // namespace rlren
