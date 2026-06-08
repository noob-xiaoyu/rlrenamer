#include "raylib_api_extract.h"

#include <cctype>
#include <sstream>

namespace rlren {

namespace {

static inline bool StartsWith(const std::string& s, const char* prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= s.size() || s[i] != prefix[i]) return false;
        i++;
    }
    return true;
}

static inline bool IsIdentStart(unsigned char c) {
    return std::isalpha(c) || c == '_';
}

static inline bool IsIdentCont(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

static bool ShouldKeepName(const std::string& name, const ApiExtractOptions& opt) {
    if (name.empty()) return false;
    if (opt.skipLeadingUnderscore && name[0] == '_') return false;
    if (opt.skipAlreadyPrefixedRL && (StartsWith(name, "RL") || StartsWith(name, "rl"))) return false;
    if (opt.requireLeadingUppercase) {
        unsigned char c = static_cast<unsigned char>(name[0]);
        if (!(c >= 'A' && c <= 'Z')) return false;
    }
    return true;
}

struct Tok {
    enum class Kind { Ident, Punct, End } kind;
    std::string text;
    char punct = 0;
    int line = 1;
};

// Skip comments/strings and return next token.
static Tok NextTok(const std::string& s, size_t& i, int& line) {
    const size_t n = s.size();

    auto skip_ws = [&]() {
        while (i < n) {
            char c = s[i];
            if (c == '\n') line++;
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc == ' ' || uc == '\t' || uc == '\r' || uc == '\n' || uc == '\f' || uc == '\v') i++;
            else break;
        }
    };

    while (true) {
        skip_ws();
        if (i >= n) return {Tok::Kind::End, "", 0, line};

        char c = s[i];

        // Prefixed literals (C++): L"...", L'...', u8"...", u"...", U"...", and raw forms like LR"...".
        // Treat the prefix as part of the literal so it doesn't become an identifier token.
        if (c == 'u' && i + 1 < n && s[i + 1] == '8') {
            if (i + 2 < n && s[i + 2] == '"') { i += 2; continue; }
            if (i + 3 < n && s[i + 2] == 'R' && s[i + 3] == '"') { i += 2; continue; }
        }
        if (c == 'L' || c == 'u' || c == 'U') {
            if (i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\'')) { i += 1; continue; }
            if (i + 2 < n && s[i + 1] == 'R' && s[i + 2] == '"') { i += 1; continue; }
        }

        // comments
        if (c == '/' && i + 1 < n) {
            char n1 = s[i + 1];
            if (n1 == '/') {
                i += 2;
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (n1 == '*') {
                i += 2;
                while (i + 1 < n) {
                    if (s[i] == '\n') line++;
                    if (s[i] == '*' && s[i + 1] == '/') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }

        // raw strings R"delim( ... )delim"
        if (c == 'R' && i + 1 < n && s[i + 1] == '"') {
            size_t j = i + 2;
            std::string delim;
            while (j < n && s[j] != '(') {
                delim.push_back(s[j]);
                j++;
            }
            if (j >= n) {
                // malformed, consume 'R' as punct
                i++;
                return {Tok::Kind::Punct, "", 'R', line};
            }
            const std::string endMarker = ")" + delim + "\"";
            size_t contentStart = j + 1;
            size_t endPos = s.find(endMarker, contentStart);
            if (endPos == std::string::npos) {
                i = n;
                return {Tok::Kind::End, "", 0, line};
            }
            // count newlines in raw string
            for (size_t k = i; k < endPos + endMarker.size(); k++) {
                if (s[k] == '\n') line++;
            }
            i = endPos + endMarker.size();
            continue;
        }

        // strings
        if (c == '"') {
            i++;
            bool escaped = false;
            while (i < n) {
                char cc = s[i++];
                if (cc == '\n') line++;
                if (escaped) { escaped = false; continue; }
                if (cc == '\\') { escaped = true; continue; }
                if (cc == '"') break;
            }
            continue;
        }

        // chars
        if (c == '\'') {
            i++;
            bool escaped = false;
            while (i < n) {
                char cc = s[i++];
                if (cc == '\n') line++;
                if (escaped) { escaped = false; continue; }
                if (cc == '\\') { escaped = true; continue; }
                if (cc == '\'') break;
            }
            continue;
        }

        // numeric literals: consume the whole token (including suffix) so trailing letters like 'L' don't become identifiers
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            bool allowExpSign = false;
            while (i < n) {
                char cc = s[i];
                if (cc == '\n') line++;
                if (std::isalnum(static_cast<unsigned char>(cc)) || cc == '.' || cc == '\'' || cc == '_') {
                    i++;
                    if (cc == 'e' || cc == 'E' || cc == 'p' || cc == 'P') allowExpSign = true;
                    else allowExpSign = false;
                    continue;
                }
                if (allowExpSign && (cc == '+' || cc == '-')) { i++; allowExpSign = false; continue; }
                break;
            }
            continue;
        }

        // ident
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(static_cast<unsigned char>(s[j]))) j++;
            Tok t{Tok::Kind::Ident, s.substr(i, j - i), 0, line};
            i = j;
            return t;
        }

        // punct
        i++;
        return {Tok::Kind::Punct, "", c, line};
    }
}

// Skip to the end of the current preprocessor directive, including line continuations (\\\n).
static void SkipPreprocDirective(const std::string& s, size_t& i, int& line) {
    const size_t n = s.size();
    while (i < n) {
        size_t nl = s.find('\n', i);
        if (nl == std::string::npos) {
            i = n;
            return;
        }
        // Check if the line is continued with a trailing backslash (ignoring spaces/tabs and optional '\r').
        size_t k = nl;
        while (k > 0 && (s[k - 1] == '\r' || s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
        const bool continued = (k > 0 && s[k - 1] == '\\');
        i = nl + 1;
        line++;
        if (!continued) return;
    }
}

static std::string TrimOneLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool ws = false;
    for (char c : s) {
        if (c == '\n' || c == '\r') break;
        if (c == '\t') c = ' ';
        if (c == ' ') {
            if (!out.empty()) ws = true;
            continue;
        }
        if (ws) {
            out.push_back(' ');
            ws = false;
        }
        out.push_back(c);
    }
    // trim trailing spaces
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

struct ParamHint {
    int paramMin = -1;
    bool varargs = false;
};

// Parse a parameter list after an opening '(' token. i points to the token *after* '('.
// Counts top-level commas (depth==1) while respecting nested parens/brackets/braces.
static ParamHint ParseParamList(const std::string& text, size_t& i, int& line) {
    ParamHint h;
    int parenDepth = 1;
    int braceDepth = 0;
    int bracketDepth = 0;

    // Count params as: 1 + commas at depth==1, unless empty or void-only.
    int commaCount = 0;
    bool sawAnyTokenAtDepth1 = false;
    bool depth1HasOnlyVoid = true;

    // For varargs detection: look for "..." as three consecutive '.' tokens at depth1.
    int dotRun = 0;
    bool sawEllipsisAtDepth1 = false;

    while (true) {
        Tok t = NextTok(text, i, line);
        if (t.kind == Tok::Kind::End) break;

        if (t.kind == Tok::Kind::Punct) {
            char p = t.punct;
            if (p == '(') parenDepth++;
            else if (p == ')') {
                parenDepth--;
                if (parenDepth == 0) break;
            } else if (p == '{') braceDepth++;
            else if (p == '}') { if (braceDepth > 0) braceDepth--; }
            else if (p == '[') bracketDepth++;
            else if (p == ']') { if (bracketDepth > 0) bracketDepth--; }

            if (parenDepth == 1 && braceDepth == 0 && bracketDepth == 0) {
                if (p == ',') {
                    commaCount++;
                    dotRun = 0;
                } else if (p == '.') {
                    dotRun++;
                    if (dotRun >= 3) {
                        sawEllipsisAtDepth1 = true;
                    }
                } else {
                    dotRun = 0;
                }
                sawAnyTokenAtDepth1 = true;
            }
            continue;
        }

        if (t.kind == Tok::Kind::Ident) {
            if (parenDepth == 1 && braceDepth == 0 && bracketDepth == 0) {
                sawAnyTokenAtDepth1 = true;
                // If the only token is exactly 'void' (and maybe qualifiers), treat it as zero-arg.
                if (t.text != "void" && t.text != "const" && t.text != "volatile" && t.text != "restrict") {
                    depth1HasOnlyVoid = false;
                }
                dotRun = 0;
            }
        }
    }

    if (!sawAnyTokenAtDepth1) {
        h.paramMin = 0;
        h.varargs = false;
        return h;
    }

    if (depth1HasOnlyVoid) {
        h.paramMin = 0;
        h.varargs = false;
        return h;
    }

    // Base count ignoring varargs.
    int params = commaCount + 1;

    if (sawEllipsisAtDepth1) {
        // Heuristic: varargs has at least one fixed parameter before "...".
        // We keep min as params-1 if the last segment is ellipsis-only; otherwise keep params.
        // This is best-effort.
        h.varargs = true;
        h.paramMin = (params > 0) ? (params - 1) : 0;
    } else {
        h.varargs = false;
        h.paramMin = params;
    }

    return h;
}


} // namespace

ApiSymbolDB ExtractRaylibApiSymbolDBFromHeader(const std::string& headerText,
                                              const ApiExtractOptions& opt,
                                              const std::string& originLabel) {
    ApiSymbolDB out;

    std::unordered_set<std::string> apiSet;
    apiSet.reserve(opt.apiDefines.size() + 1);
    for (const auto& s : opt.apiDefines) {
        if (!s.empty()) apiSet.insert(s);
    }

    size_t i = 0;
    int line = 1;

    while (true) {
        Tok t = NextTok(headerText, i, line);
        if (t.kind == Tok::Kind::End) break;

        // Preprocessor directives: optionally record #define name.
        if (t.kind == Tok::Kind::Punct && t.punct == '#') {
            Tok t2 = NextTok(headerText, i, line);
            if (opt.includeDefineMacros && t2.kind == Tok::Kind::Ident && t2.text == "define") {
                Tok nameTok = NextTok(headerText, i, line);
                if (nameTok.kind == Tok::Kind::Ident && ShouldKeepName(nameTok.text, opt)) {
                    ApiSymbolInfo info;
                    info.name = nameTok.text;
                    info.kind = ApiSymbolKind::DefineMacro;
                    info.origin = originLabel;
                    info.originLine = nameTok.line;
                    out.emplace(info.name, std::move(info));
                }
            }
            SkipPreprocDirective(headerText, i, line);
            continue;
        }

        // typedef ...
        if (t.kind == Tok::Kind::Ident && t.text == "typedef") {
            Tok t2 = NextTok(headerText, i, line);

            // typedef struct { ... } Alias;
            if (t2.kind == Tok::Kind::Ident && t2.text == "struct") {
                std::string alias;
                int braceDepth = 0;
                const int startLine = t.line;
                size_t snippetStart = i; // best-effort
                while (true) {
                    Tok tt = NextTok(headerText, i, line);
                    if (tt.kind == Tok::Kind::End) break;
                    if (tt.kind == Tok::Kind::Punct) {
                        if (tt.punct == '{') braceDepth++;
                        else if (tt.punct == '}') { if (braceDepth > 0) braceDepth--; }
                        else if (tt.punct == ';' && braceDepth == 0) break;
                    } else if (tt.kind == Tok::Kind::Ident) {
                        if (braceDepth == 0) alias = tt.text;
                    }
                }
                if (!alias.empty() && ShouldKeepName(alias, opt)) {
                    ApiSymbolInfo info;
                    info.name = alias;
                    info.kind = ApiSymbolKind::TypedefStruct;
                    info.origin = originLabel;
                    info.originLine = startLine;
                    // decl snippet: from "typedef" line start (best-effort one-line)
                    info.decl = TrimOneLine(headerText.substr(snippetStart > 0 ? snippetStart - 1 : 0));
                    out.emplace(info.name, std::move(info));
                }
                continue;
            }

            // typedef enum { ... } Name;
            if (opt.includeEnums && t2.kind == Tok::Kind::Ident && t2.text == "enum") {
                std::string enumTypeName;
                int braceDepth = 0;
                bool inEnumBody = false;
                bool expectValueName = false;
                const int startLine = t.line;
                size_t snippetStart = i;
                while (true) {
                    Tok tt = NextTok(headerText, i, line);
                    if (tt.kind == Tok::Kind::End) break;

                    if (tt.kind == Tok::Kind::Punct) {
                        if (tt.punct == '{') {
                            braceDepth++;
                            inEnumBody = true;
                            expectValueName = true;
                        } else if (tt.punct == '}') {
                            if (braceDepth > 0) braceDepth--;
                            if (braceDepth == 0) {
                                inEnumBody = false;
                                expectValueName = false;
                            }
                        } else if (tt.punct == ',') {
                            if (inEnumBody && braceDepth == 1) expectValueName = true;
                        } else if (tt.punct == ';' && braceDepth == 0) {
                            break;
                        }
                        continue;
                    }

                    if (tt.kind == Tok::Kind::Ident) {
                        if (inEnumBody && opt.includeEnumValues && braceDepth == 1) {
                            if (expectValueName) {
                                if (ShouldKeepName(tt.text, opt)) {
                                    ApiSymbolInfo info;
                                    info.name = tt.text;
                                    info.kind = ApiSymbolKind::EnumValue;
                                    info.origin = originLabel;
                                    info.originLine = tt.line;
                                    out.emplace(info.name, std::move(info));
                                }
                                expectValueName = false;
                            }
                        } else {
                            // outside enum body: last identifier before ';' is the typedef name
                            if (braceDepth == 0) enumTypeName = tt.text;
                        }
                    }
                }

                if (!enumTypeName.empty() && ShouldKeepName(enumTypeName, opt)) {
                    ApiSymbolInfo info;
                    info.name = enumTypeName;
                    info.kind = ApiSymbolKind::TypedefEnum;
                    info.origin = originLabel;
                    info.originLine = startLine;
                    info.decl = TrimOneLine(headerText.substr(snippetStart > 0 ? snippetStart - 1 : 0));
                    out.emplace(info.name, std::move(info));
                }
                continue;
            }

            // Other typedefs (aliases, callbacks)
            if (opt.includeTypedefAliases) {
                std::string alias;
                bool wantFuncPtrAlias = false;
                bool funcPtrAliasFound = false;
                bool justEnteredParen = false;

                Tok tt = t2;
                int parenDepth = 0;
                int braceDepth = 0;
                const int startLine = t.line;
                size_t snippetStart = i;

                auto onTok = [&](const Tok& x) {
                    if (x.kind == Tok::Kind::Ident) {
                        if (wantFuncPtrAlias) {
                            alias = x.text;
                            wantFuncPtrAlias = false;
                            funcPtrAliasFound = true;
                        } else if (!funcPtrAliasFound && parenDepth == 0 && braceDepth == 0) {
                            alias = x.text;
                        }
                        return;
                    }

                    if (x.kind == Tok::Kind::Punct) {
                        if (x.punct == '(') {
                            parenDepth++;
                            if (parenDepth == 1 && !funcPtrAliasFound) justEnteredParen = true;
                        } else if (x.punct == ')') {
                            if (parenDepth > 0) parenDepth--;
                            justEnteredParen = false;
                        } else if (x.punct == '{') {
                            braceDepth++;
                        } else if (x.punct == '}') {
                            if (braceDepth > 0) braceDepth--;
                        } else if (x.punct == '*') {
                            if (justEnteredParen && parenDepth == 1 && !funcPtrAliasFound) {
                                wantFuncPtrAlias = true;
                                justEnteredParen = false;
                            }
                        }
                    }
                };

                onTok(tt);
                while (true) {
                    tt = NextTok(headerText, i, line);
                    if (tt.kind == Tok::Kind::End) break;

                    if (tt.kind == Tok::Kind::Punct && tt.punct == ';' && parenDepth == 0 && braceDepth == 0) {
                        break;
                    }

                    onTok(tt);
                }

                if (!alias.empty() && ShouldKeepName(alias, opt)) {
                    ApiSymbolInfo info;
                    info.name = alias;
                    info.kind = ApiSymbolKind::TypedefAlias;
                    info.origin = originLabel;
                    info.originLine = startLine;
                    info.decl = TrimOneLine(headerText.substr(snippetStart > 0 ? snippetStart - 1 : 0));
                    out.emplace(info.name, std::move(info));
                }
            }

            continue;
        }

        // API macro ... Name(...);
        if (t.kind == Tok::Kind::Ident && apiSet.find(t.text) != apiSet.end()) {
            std::string lastIdent;
            bool capturedFuncName = false;
            int parenDepth = 0;
            int startLine = t.line;
            size_t snippetStart = i;
            ParamHint hint;

            while (true) {
                Tok tt = NextTok(headerText, i, line);
                if (tt.kind == Tok::Kind::End) break;

                if (tt.kind == Tok::Kind::Ident) {
                    lastIdent = tt.text;
                    continue;
                }

                if (tt.kind == Tok::Kind::Punct) {
                    if (tt.punct == '(') {
                        if (!capturedFuncName) {
                            if (!lastIdent.empty() && ShouldKeepName(lastIdent, opt)) {
                                // Parse params to get signature hints.
                                hint = ParseParamList(headerText, i, line);

                                ApiSymbolInfo info;
                                info.name = lastIdent;
                                info.kind = ApiSymbolKind::Function;
                                info.paramMin = hint.paramMin;
                                info.varargs = hint.varargs;
                                info.origin = originLabel;
                                info.originLine = startLine;
                                info.decl = TrimOneLine(headerText.substr(snippetStart > 0 ? snippetStart - 1 : 0));
                                out.emplace(info.name, std::move(info));
                            }
                            capturedFuncName = true;
                        } else {
                            parenDepth++;
                        }
                        // ParseParamList already consumed until matching ')'.
                        // After that, keep scanning until ';' or '{' at parenDepth==0.
                        continue;
                    }

                    if (tt.punct == ')') {
                        if (parenDepth > 0) parenDepth--;
                    } else if ((tt.punct == ';' || tt.punct == '{') && parenDepth == 0) {
                        break;
                    }
                }
            }
            continue;
        }
    }

    return out;
}

ApiSymbolDB ExtractTopLevelFunctionDBFromSource(const std::string& sourceText,
                                               const ApiExtractOptions& opt,
                                               const std::string& originLabel) {
    ApiSymbolDB out;

    const std::unordered_set<std::string> kSkipBeforeParen = {
        "if", "for", "while", "switch", "return", "sizeof", "defined", "catch"
    };

    size_t i = 0;
    int braceDepth = 0;
    int line = 1;
    std::string lastIdent;

    while (true) {
        Tok t = NextTok(sourceText, i, line);
        if (t.kind == Tok::Kind::End) break;

        if (t.kind == Tok::Kind::Punct) {
            if (t.punct == '{') {
                braceDepth++;
                lastIdent.clear();
                continue;
            }
            if (t.punct == '}') {
                if (braceDepth > 0) braceDepth--;
                lastIdent.clear();
                continue;
            }

            if (t.punct == '(' && braceDepth == 0) {
                const std::string candidate = lastIdent;
                lastIdent.clear();

                if (!candidate.empty() && kSkipBeforeParen.find(candidate) == kSkipBeforeParen.end()) {
                    // Consume parameter list (we could parse args, but sources might be complex; keep unknown).
                    int parenDepth = 1;
                    while (parenDepth > 0) {
                        Tok tt = NextTok(sourceText, i, line);
                        if (tt.kind == Tok::Kind::End) break;
                        if (tt.kind == Tok::Kind::Punct) {
                            if (tt.punct == '(') parenDepth++;
                            else if (tt.punct == ')') parenDepth--;
                        }
                    }

                    // Look ahead for '{' (definition)
                    while (true) {
                        size_t save = i;
                        int saveLine = line;
                        Tok nx = NextTok(sourceText, i, line);
                        if (nx.kind == Tok::Kind::End) break;
                        if (nx.kind == Tok::Kind::Ident) {
                            continue; // qualifiers
                        }
                        if (nx.kind == Tok::Kind::Punct) {
                            if (nx.punct == '{') {
                                if (ShouldKeepName(candidate, opt)) {
                                    ApiSymbolInfo info;
                                    info.name = candidate;
                                    info.kind = ApiSymbolKind::Function;
                                    info.paramMin = -1;
                                    info.varargs = false;
                                    info.origin = originLabel;
                                    info.originLine = t.line;
                                    out.emplace(info.name, std::move(info));
                                }

                                // Skip body quickly
                                int d = 1;
                                while (d > 0) {
                                    Tok bt = NextTok(sourceText, i, line);
                                    if (bt.kind == Tok::Kind::End) break;
                                    if (bt.kind == Tok::Kind::Punct) {
                                        if (bt.punct == '{') d++;
                                        else if (bt.punct == '}') d--;
                                    }
                                }
                                break;
                            }
                            if (nx.punct == ';' || nx.punct == '=' || nx.punct == ',') {
                                break;
                            }
                            continue;
                        }
                        i = save;
                        line = saveLine;
                        break;
                    }
                }
                continue;
            }
        }

        if (t.kind == Tok::Kind::Ident) {
            lastIdent = t.text;
        }
    }

    return out;
}

// Legacy wrappers
std::unordered_set<std::string> ExtractRaylibApiSymbolsFromHeader(
    const std::string& headerText,
    const ApiExtractOptions& opt) {

    auto db = ExtractRaylibApiSymbolDBFromHeader(headerText, opt);
    std::unordered_set<std::string> out;
    out.reserve(db.size());
    for (const auto& kv : db) out.insert(kv.first);
    return out;
}

std::unordered_set<std::string> ExtractTopLevelFunctionDefinitionsFromSource(
    const std::string& sourceText,
    const ApiExtractOptions& opt) {

    auto db = ExtractTopLevelFunctionDBFromSource(sourceText, opt);
    std::unordered_set<std::string> out;
    out.reserve(db.size());
    for (const auto& kv : db) out.insert(kv.first);
    return out;
}

} // namespace rlren
