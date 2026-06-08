#include "consumer_heuristics.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace rlren {

namespace {

static inline bool IsIdentStart(unsigned char c) {
    return std::isalpha(c) || c == '_';
}

static inline bool IsIdentCont(unsigned char c) {
    return std::isalnum(c) || c == '_';
}

struct SigTok {
    enum class Kind { None, Ident, Punct } kind = Kind::None;
    std::string text;
};

static inline bool IsWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static size_t SkipStringLiteral(const std::string& s, size_t i, int& line, int& col) {
    const size_t n = s.size();
    // s[i] is '"'
    i++; col++;
    bool escaped = false;
    while (i < n) {
        char c = s[i++];
        if (c == '\n') { line++; col = 1; }
        else col++;
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') break;
    }
    return i;
}

static size_t SkipCharLiteral(const std::string& s, size_t i, int& line, int& col) {
    const size_t n = s.size();
    i++; col++;
    bool escaped = false;
    while (i < n) {
        char c = s[i++];
        if (c == '\n') { line++; col = 1; }
        else col++;
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '\'') break;
    }
    return i;
}

static size_t SkipRawString(const std::string& s, size_t i, int& line, int& col) {
    const size_t n = s.size();
    // Expect R"delim( ... )delim"
    size_t start = i;
    if (i + 1 >= n || s[i] != 'R' || s[i + 1] != '"') return i + 1;
    size_t j = i + 2;
    std::string delim;
    while (j < n && s[j] != '(') {
        delim.push_back(s[j]);
        j++;
    }
    if (j >= n) return n;
    std::string endMarker = ")" + delim + "\"";
    size_t contentStart = j + 1;
    size_t endPos = s.find(endMarker, contentStart);
    if (endPos == std::string::npos) return n;

    size_t endI = endPos + endMarker.size();
    for (size_t k = start; k < endI; k++) {
        if (s[k] == '\n') { line++; col = 1; }
        else col++;
    }
    return endI;
}

static size_t SkipLineComment(const std::string& s, size_t i, int& line, int& col) {
    const size_t n = s.size();
    // starts with //
    i += 2; col += 2;
    while (i < n) {
        char c = s[i++];
        if (c == '\n') { line++; col = 1; break; }
        col++;
    }
    return i;
}

static size_t SkipBlockComment(const std::string& s, size_t i, int& line, int& col) {
    const size_t n = s.size();
    // starts with /*
    i += 2; col += 2;
    while (i < n) {
        char c = s[i++];
        if (c == '\n') { line++; col = 1; }
        else col++;
        if (c == '*' && i < n && s[i] == '/') {
            i++; col++;
            break;
        }
    }
    return i;
}

static SigTok ConsumePunctToken(const std::string& s, size_t& i, int& line, int& col) {
    const size_t n = s.size();
    SigTok tok;
    tok.kind = SigTok::Kind::Punct;

    char c = s[i];
    // multi-char punct we care about
    if ((c == '-' && i + 1 < n && s[i + 1] == '>') ||
        (c == ':' && i + 1 < n && s[i + 1] == ':') ||
        (c == '&' && i + 1 < n && s[i + 1] == '&') ||
        (c == '|' && i + 1 < n && s[i + 1] == '|') ||
        (c == '<' && i + 1 < n && s[i + 1] == '<') ||
        (c == '>' && i + 1 < n && s[i + 1] == '>')) {
        tok.text.push_back(c);
        tok.text.push_back(s[i + 1]);
        i += 2;
        col += 2;
        return tok;
    }

    tok.text.push_back(c);
    i++;
    if (c == '\n') { line++; col = 1; }
    else col++;
    return tok;
}

static size_t PeekNextNonWs(const std::string& s, size_t k) {
    const size_t n = s.size();
    while (k < n && IsWs(s[k])) k++;
    return k;
}

struct ArgCountResult {
    bool ok = false;
    int argc = -1;
};

static ArgCountResult CountCallArgs(const std::string& s, size_t openParenIndex) {
    const size_t n = s.size();
    if (openParenIndex >= n || s[openParenIndex] != '(') return {false, -1};

    size_t i = openParenIndex + 1;
    int parenDepth = 1;
    int braceDepth = 0;
    int bracketDepth = 0;

    bool sawToken = false;
    bool onlyVoid = true;

    int commas = 0;

    while (i < n) {
        char c = s[i];

        // comments / strings inside args
        if (c == '/' && i + 1 < n) {
            if (s[i + 1] == '/') {
                // skip to newline
                i += 2;
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (s[i + 1] == '*') {
                i += 2;
                while (i + 1 < n) {
                    if (s[i] == '*' && s[i + 1] == '/') { i += 2; break; }
                    i++;
                }
                continue;
            }
        }
        if (c == 'R' && i + 1 < n && s[i + 1] == '"') {
            int dummyLine = 1, dummyCol = 1;
            i = SkipRawString(s, i, dummyLine, dummyCol);
            continue;
        }
        if (c == '"') {
            // skip string
            i++;
            bool esc = false;
            while (i < n) {
                char cc = s[i++];
                if (esc) { esc = false; continue; }
                if (cc == '\\') { esc = true; continue; }
                if (cc == '"') break;
            }
            sawToken = true;
            onlyVoid = false;
            continue;
        }
        if (c == '\'') {
            i++;
            bool esc = false;
            while (i < n) {
                char cc = s[i++];
                if (esc) { esc = false; continue; }
                if (cc == '\\') { esc = true; continue; }
                if (cc == '\'') break;
            }
            sawToken = true;
            onlyVoid = false;
            continue;
        }

        if (IsWs(c)) { i++; continue; }

        if (c == '(') { parenDepth++; i++; sawToken = true; onlyVoid = false; continue; }
        if (c == ')') {
            parenDepth--;
            i++;
            if (parenDepth == 0) break;
            sawToken = true;
            onlyVoid = false;
            continue;
        }
        if (c == '{') { braceDepth++; i++; sawToken = true; onlyVoid = false; continue; }
        if (c == '}') { if (braceDepth > 0) braceDepth--; i++; sawToken = true; onlyVoid = false; continue; }
        if (c == '[') { bracketDepth++; i++; sawToken = true; onlyVoid = false; continue; }
        if (c == ']') { if (bracketDepth > 0) bracketDepth--; i++; sawToken = true; onlyVoid = false; continue; }

        if (parenDepth == 1 && braceDepth == 0 && bracketDepth == 0 && c == ',') {
            commas++;
            i++;
            sawToken = true;
            onlyVoid = false;
            continue;
        }

        // ident: check if only 'void'
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            size_t j = i + 1;
            while (j < n && IsIdentCont(static_cast<unsigned char>(s[j]))) j++;
            std::string ident = s.substr(i, j - i);
            if (parenDepth == 1 && braceDepth == 0 && bracketDepth == 0) {
                sawToken = true;
                if (ident != "void" && ident != "const" && ident != "volatile" && ident != "restrict") {
                    onlyVoid = false;
                }
            }
            i = j;
            continue;
        }

        // other tokens
        sawToken = true;
        onlyVoid = false;
        i++;
    }

    if (parenDepth != 0) return {false, -1};

    if (!sawToken) return {true, 0};
    if (onlyVoid) return {true, 0};
    return {true, commas + 1};
}

static bool IsBuiltinTypeKeyword(const std::string& s) {
    static const std::unordered_set<std::string> k = {
        "void","bool","char","short","int","long","float","double",
        "signed","unsigned","wchar_t","char16_t","char32_t","auto"
    };
    return k.find(s) != k.end();
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // drop control chars
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

static const char* KindToStr(ApiSymbolKind k) {
    switch (k) {
        case ApiSymbolKind::Function: return "function";
        case ApiSymbolKind::TypedefStruct: return "typedef_struct";
        case ApiSymbolKind::TypedefEnum: return "typedef_enum";
        case ApiSymbolKind::TypedefAlias: return "typedef_alias";
        case ApiSymbolKind::DefineMacro: return "define";
        case ApiSymbolKind::EnumValue: return "enum_value";
        default: return "unknown";
    }
}

static ApiSymbolKind StrToKind(const std::string& s) {
    if (s == "function") return ApiSymbolKind::Function;
    if (s == "typedef_struct") return ApiSymbolKind::TypedefStruct;
    if (s == "typedef_enum") return ApiSymbolKind::TypedefEnum;
    if (s == "typedef_alias") return ApiSymbolKind::TypedefAlias;
    if (s == "define") return ApiSymbolKind::DefineMacro;
    if (s == "enum_value") return ApiSymbolKind::EnumValue;
    return ApiSymbolKind::Unknown;
}

} // namespace

std::unordered_set<std::string> DetectShadowedNamesInConsumerFile(
    const std::string& text,
    const ApiSymbolDB& raylibDb) {

    // Heuristic: look for function declarations/definitions and type declarations.
    // If the declared name matches a raylib symbol (function/type), we mark it as shadowed.

    std::unordered_set<std::string> shadowed;

    const std::unordered_set<std::string> kSkipBeforeParen = {
        "if","for","while","switch","return","sizeof","defined","catch","new","delete"
    };

    size_t i = 0;
    const size_t n = text.size();

    int line = 1;
    int col = 1;

    // simple token tracking
    SigTok prevSig;
    SigTok prevPrevSig;

    int braceDepth = 0;

    auto pushSig = [&](const SigTok& t) {
        prevPrevSig = prevSig;
        prevSig = t;
    };

    auto readIdent = [&](size_t start) -> std::pair<std::string, size_t> {
        size_t j = start + 1;
        while (j < n && IsIdentCont(static_cast<unsigned char>(text[j]))) j++;
        return {text.substr(start, j - start), j};
    };

    while (i < n) {
        char c = text[i];

        // comments
        if (c == '/' && i + 1 < n) {
            if (text[i + 1] == '/') {
                i = SkipLineComment(text, i, line, col);
                continue;
            }
            if (text[i + 1] == '*') {
                i = SkipBlockComment(text, i, line, col);
                continue;
            }
        }

        // strings/chars/raw
        if (c == 'R' && i + 1 < n && text[i + 1] == '"') {
            i = SkipRawString(text, i, line, col);
            continue;
        }
        if (c == '"') { i = SkipStringLiteral(text, i, line, col); continue; }
        if (c == '\'') { i = SkipCharLiteral(text, i, line, col); continue; }

        // whitespace
        if (IsWs(c)) {
            if (c == '\n') { line++; col = 1; }
            else col++;
            i++;
            continue;
        }

        // braces (track global-ish scope)
        if (c == '{') { braceDepth++; pushSig({SigTok::Kind::Punct, "{"}); i++; col++; continue; }
        if (c == '}') { if (braceDepth > 0) braceDepth--; pushSig({SigTok::Kind::Punct, "}"}); i++; col++; continue; }

        // identifiers
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            auto [ident, j] = readIdent(i);
            SigTok tok{SigTok::Kind::Ident, ident};

            // Detect type declarations: struct/class/enum NAME
            if (ident == "struct" || ident == "class" || ident == "enum") {
                // skip ws
                size_t k = PeekNextNonWs(text, j);
                if (k < n && IsIdentStart(static_cast<unsigned char>(text[k]))) {
                    auto [nm, k2] = readIdent(k);
                    auto it = raylibDb.find(nm);
                    if (it != raylibDb.end()) {
                        shadowed.insert(nm);
                    }
                    // advance only past keyword; leave the name to be processed naturally.
                }
            }

            // Detect typedef/using alias names: typedef ... NAME; / using NAME =
            if (ident == "using") {
                size_t k = PeekNextNonWs(text, j);
                if (k < n && IsIdentStart(static_cast<unsigned char>(text[k]))) {
                    auto [nm, k2] = readIdent(k);
                    auto it = raylibDb.find(nm);
                    if (it != raylibDb.end()) shadowed.insert(nm);
                }
            }

            // Function declaration/definition heuristic: candidate ident immediately before '(' not qualified.
            // We'll check this when we later see '(' by looking at prevSig token.
            pushSig(tok);

            col += static_cast<int>(j - i);
            i = j;
            continue;
        }

        // punctuation tokens
        {
            size_t before = i;
            SigTok tok = ConsumePunctToken(text, i, line, col);

            if (tok.text == "(") {
                // candidate is previous significant ident
                if (prevSig.kind == SigTok::Kind::Ident) {
                    const std::string& cand = prevSig.text;

                    // Not qualified by ., ->, ::
                    const bool qualified = (prevPrevSig.kind == SigTok::Kind::Punct &&
                                            (prevPrevSig.text == "." || prevPrevSig.text == "->" || prevPrevSig.text == "::"));

                    const bool looksLikeDeclPrefix = (prevPrevSig.kind == SigTok::Kind::Ident) ||
                                            (prevPrevSig.kind == SigTok::Kind::Punct && (prevPrevSig.text == "*" || prevPrevSig.text == "&"));

                    if (!qualified && looksLikeDeclPrefix && kSkipBeforeParen.find(cand) == kSkipBeforeParen.end()) {
                        // Consume paren list quickly to reach ')'
                        int depth = 1;
                        size_t k = i;
                        while (k < n && depth > 0) {
                            char cc = text[k];
                            if (cc == '/' && k + 1 < n) {
                                if (text[k + 1] == '/') {
                                    // line comment
                                    k += 2;
                                    while (k < n && text[k] != '\n') k++;
                                    continue;
                                }
                                if (text[k + 1] == '*') {
                                    k += 2;
                                    while (k + 1 < n) {
                                        if (text[k] == '*' && text[k + 1] == '/') { k += 2; break; }
                                        k++;
                                    }
                                    continue;
                                }
                            }
                            if (cc == '"') {
                                // string
                                k++;
                                bool esc = false;
                                while (k < n) {
                                    char sc = text[k++];
                                    if (esc) { esc = false; continue; }
                                    if (sc == '\\') { esc = true; continue; }
                                    if (sc == '"') break;
                                }
                                continue;
                            }
                            if (cc == '\'') {
                                k++;
                                bool esc = false;
                                while (k < n) {
                                    char sc = text[k++];
                                    if (esc) { esc = false; continue; }
                                    if (sc == '\\') { esc = true; continue; }
                                    if (sc == '\'') break;
                                }
                                continue;
                            }
                            if (cc == 'R' && k + 1 < n && text[k + 1] == '"') {
                                int dl=1, dc=1;
                                k = SkipRawString(text, k, dl, dc);
                                continue;
                            }

                            if (cc == '(') depth++;
                            else if (cc == ')') depth--;
                            k++;
                        }

                        // Look ahead for '{' or ';' (a declaration/definition) at the same "statement" level.
                        size_t m = k;
                        while (m < n && IsWs(text[m])) m++;
                        // skip common qualifiers words and attributes (best-effort)
                        while (m < n && IsIdentStart(static_cast<unsigned char>(text[m]))) {
                            auto [w, m2] = [&]() {
                                size_t jj = m + 1;
                                while (jj < n && IsIdentCont(static_cast<unsigned char>(text[jj]))) jj++;
                                return std::pair<std::string,size_t>(text.substr(m, jj - m), jj);
                            }();
                            (void)w;
                            m = PeekNextNonWs(text, m2);
                        }

                        if (braceDepth == 0 && m < n && (text[m] == '{' || text[m] == ';')) {
                            // Treat as declaration/definition
                            if (raylibDb.find(cand) != raylibDb.end()) {
                                shadowed.insert(cand);
                            }
                        }
                    }
                }
            }

            pushSig(tok);
            (void)before;
            continue;
        }
    }

    return shadowed;
}

std::string RewriteConsumerWithValidation(
    const std::string& input,
    const ApiSymbolDB& raylibDb,
    const std::string& prefix,
    const std::string& enumPrefix,
    const std::unordered_set<std::string>& shadowedNames,
    const std::unordered_map<std::string, std::string>* includerShadowed,
    const ConsumerModeOptions& opt,
    ConsumerFileAnalysis* outAnalysis) {

    ConsumerFileAnalysis local;
    if (!outAnalysis) outAnalysis = &local;

    outAnalysis->shadowed = shadowedNames;

    std::string out;
    out.reserve(input.size());

    const size_t n = input.size();
    size_t i = 0;

    int line = 1;
    int col = 1;

    bool atLineStart = true;
    bool inPreproc = false;

    SigTok prevSig;
    SigTok prevPrevSig;

    auto pushSig = [&](const SigTok& t) {
        prevPrevSig = prevSig;
        prevSig = t;
    };

    auto readIdent = [&](size_t start) -> std::pair<std::string, size_t> {
        size_t j = start + 1;
        while (j < n && IsIdentCont(static_cast<unsigned char>(input[j]))) j++;
        return {input.substr(start, j - start), j};
    };

    auto recordSuspicious = [&](const std::string& ident, const std::string& reason, int l, int c, const std::string& detail) {
        if (static_cast<int>(outAnalysis->suspicious.size()) < opt.maxSamplesPerFile) {
            outAnalysis->suspicious.push_back({ident, reason, detail, l, c});
        }
    };

    auto shouldReplace = [&](const std::string& ident, const ApiSymbolInfo& sym) -> bool {
        // Preprocessor directives
        if (opt.skipPreprocessor && inPreproc && !opt.rewritePreprocessor) {
            recordSuspicious(ident, "preproc", line, col, "");
            return opt.replaceSuspicious;
        }

        // Shadowed names
        if (opt.skipShadowedNames && shadowedNames.find(ident) != shadowedNames.end()) {
            recordSuspicious(ident, "shadowed", line, col, "defined in this file");
            return opt.replaceSuspicious;
        }

        // Multi-round strict check: shadowed by includer context (headers included before this file).
        if (includerShadowed) {
            auto itS = includerShadowed->find(ident);
            if (itS != includerShadowed->end()) {
                recordSuspicious(ident, "multi_round_shadowed", line, col, itS->second);
                return opt.replaceSuspicious;
            }
        }

        // Qualified names
        if (opt.skipQualified) {
            if (prevSig.kind == SigTok::Kind::Punct && (prevSig.text == "." || prevSig.text == "->" || prevSig.text == "::")) {
                recordSuspicious(ident, "qualified", line, col, "");
                return opt.replaceSuspicious;
            }
            if (prevPrevSig.kind == SigTok::Kind::Punct && (prevPrevSig.text == "." || prevPrevSig.text == "->" || prevPrevSig.text == "::")) {
                // e.g. cv-qualified member access: obj->volatile Member
                recordSuspicious(ident, "qualified", line, col, "");
                return opt.replaceSuspicious;
            }
        }

        // Decide based on kind
        size_t k = PeekNextNonWs(input, i + ident.size());
        char next = (k < n) ? input[k] : 0;

        if (sym.kind == ApiSymbolKind::Function) {
            if (next != '(') {
                recordSuspicious(ident, "func_not_call", line, col, "");
                return opt.replaceSuspicious;
            }
            if (opt.validateFunctionCallArity && sym.paramMin >= 0) {
                ArgCountResult r = CountCallArgs(input, k);
                if (!r.ok) {
                    recordSuspicious(ident, "arg_parse_fail", line, col, "");
                    return opt.replaceSuspicious;
                }
                if (sym.varargs) {
                    if (r.argc < sym.paramMin) {
                        recordSuspicious(ident, "arg_mismatch", line, col, "");
                        return opt.replaceSuspicious;
                    }
                } else {
                    if (r.argc != sym.paramMin) {
                        recordSuspicious(ident, "arg_mismatch", line, col, "");
                        return opt.replaceSuspicious;
                    }
                }
            }
            return true;
        }

        // Types / aliases / enums: avoid obvious variable declarations like "int Color;" where prev token is builtin type.
        if (sym.kind == ApiSymbolKind::TypedefStruct || sym.kind == ApiSymbolKind::TypedefEnum || sym.kind == ApiSymbolKind::TypedefAlias) {
            if (prevSig.kind == SigTok::Kind::Ident && IsBuiltinTypeKeyword(prevSig.text)) {
                recordSuspicious(ident, "type_as_var", line, col, "");
                return opt.replaceSuspicious;
            }
            // If it looks like a call, it's suspicious.
            if (next == '(') {
                recordSuspicious(ident, "type_call_like", line, col, "");
                return opt.replaceSuspicious;
            }
            return true;
        }

        // Enum values: allow replacement with minimal checks.
        // These appear in expressions, switch cases, assignments, etc.
        if (sym.kind == ApiSymbolKind::EnumValue) {
            // If it looks like a call, it's suspicious.
            if (next == '(') {
                recordSuspicious(ident, "enum_value_call_like", line, col, "");
                return opt.replaceSuspicious;
            }
            return true;
        }

        // Other kinds (macros/enum values): keep conservative
        recordSuspicious(ident, "unknown_kind", line, col, "");
        return opt.replaceSuspicious;
    };

    while (i < n) {
        char c = input[i];

        // Update preprocessor mode: if at line start (ignoring spaces) and we see '#', mark preproc.
        if (atLineStart) {
            if (IsWs(c)) {
                // keep atLineStart
            } else {
                if (c == '#') inPreproc = true;
                atLineStart = false;
            }
        }

        // Whitespace (do not affect significant-token tracking)
        if (IsWs(c)) {
            if (c == "\n"[0]) {
                bool cont = false;
                if (inPreproc) {
                    size_t k = i;
                    while (k > 0 && (input[k-1] == " "[0] || input[k-1] == "\t"[0] || input[k-1] == "\r"[0])) k--;
                    if (k > 0 && input[k-1] == "\\"[0]) cont = true;
                }
                out.push_back("\n"[0]);
                i++;
                line++;
                col = 1;
                atLineStart = true;
                if (!(inPreproc && cont)) inPreproc = false;
                continue;
            }
            // Other whitespace
            out.push_back(c);
            i++;
            if (c == "\r"[0]) {
                // keep col as-is
            } else {
                col++;
            }
            continue;
        }

        // Comments
        if (c == '/' && i + 1 < n) {
            char n1 = input[i + 1];
            if (n1 == '/') {
                // copy comment
                out.push_back('/'); out.push_back('/');
                i += 2; col += 2;
                while (i < n) {
                    char cc = input[i++];
                    out.push_back(cc);
                    if (cc == '\n') { line++; col = 1; atLineStart = true; inPreproc = false; break; }
                    col++;
                }
                continue;
            }
            if (n1 == '*') {
                out.push_back('/'); out.push_back('*');
                i += 2; col += 2;
                while (i < n) {
                    char cc = input[i++];
                    out.push_back(cc);
                    if (cc == '\n') { line++; col = 1; }
                    else col++;
                    if (cc == '*' && i < n && input[i] == '/') {
                        out.push_back('/');
                        i++; col++;
                        break;
                    }
                }
                continue;
            }
        }

        // Raw string
        if (c == 'R' && i + 1 < n && input[i + 1] == '"') {
            size_t start = i;
            int l0 = line, c0 = col;
            i = SkipRawString(input, i, line, col);
            out.append(input.substr(start, i - start));
            // raw string may contain newlines and resets line/col already
            pushSig({SigTok::Kind::Ident, ""});
            (void)l0; (void)c0;
            continue;
        }

        // String literal
        if (c == '"') {
            size_t start = i;
            i = SkipStringLiteral(input, i, line, col);
            out.append(input.substr(start, i - start));
            continue;
        }

        // Char literal
        if (c == '\'') {
            size_t start = i;
            i = SkipCharLiteral(input, i, line, col);
            out.append(input.substr(start, i - start));
            continue;
        }

        // Identifier
        if (IsIdentStart(static_cast<unsigned char>(c))) {
            auto [ident, j] = readIdent(i);

            // Never rewrite preprocessor operator
            if (ident == "defined") {
                out.append(ident);
                pushSig({SigTok::Kind::Ident, ident});
                col += static_cast<int>(j - i);
                i = j;
                continue;
            }

            auto it = raylibDb.find(ident);
            if (it != raylibDb.end()) {
                const ApiSymbolInfo& sym = it->second;
                bool ok = shouldReplace(ident, sym);
                if (ok) {
                    const bool isEnumValue = (sym.kind == ApiSymbolKind::EnumValue);
                    const std::string& usePrefix = (isEnumValue && !enumPrefix.empty()) ? enumPrefix : prefix;
                    out.append(usePrefix);
                    out.append(ident);
                    outAnalysis->replacements++;
                    outAnalysis->perIdent[ident]++;
                } else {
                    out.append(ident);
                }
            } else {
                out.append(ident);
            }

            pushSig({SigTok::Kind::Ident, ident});
            col += static_cast<int>(j - i);
            i = j;
            continue;
        }

        // Punctuation / other chars
        {
            size_t before = i;
            int l0 = line, c0 = col;
            SigTok tok = ConsumePunctToken(input, i, line, col);
            out.append(input.substr(before, i - before));

            // If we are in a preprocessor directive, it may continue with a trailing backslash.
            if (tok.text == "\\" && inPreproc) {
                // no-op; handled at newline boundary implicitly.
            }

            pushSig(tok);
            (void)l0; (void)c0;
            continue;
        }
    }

    return out;
}

std::vector<std::string> SuggestConsumerExcludeDirs(const std::vector<std::string>& topLevelDirNames) {
    // Simple heuristic based on common dependency/build dir names.
    const std::unordered_set<std::string> kBad = {
        ".git", ".svn", ".hg", ".vs", ".vscode",
        "build", "out", "bin", "obj", "cmake-build-debug", "cmake-build-release",
        "third_party", "thirdparty", "3rdparty", "external", "extern", "vendor", "deps", "dep",
        "raylib", "imgui", "glfw", "glew", "stb", "zlib", "curl", "sqlite3", "nlohmann"
    };

    std::vector<std::string> out;
    for (const auto& d : topLevelDirNames) {
        if (kBad.find(d) != kBad.end()) out.push_back(d);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void WriteConsumerContextJson(const std::string& outPath, const ConsumerProjectContext& ctx) {
    std::ofstream f(outPath, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open context output for writing: " + outPath);

    f << "{\n";
    f << "  \"totalFiles\": " << ctx.totalFiles << ",\n";
    f << "  \"changedFiles\": " << ctx.changedFiles << ",\n";
    f << "  \"totalReplacements\": " << ctx.totalReplacements << ",\n";
    f << "  \"totalSuspicious\": " << ctx.totalSuspicious << ",\n";
    f << "  \"totalShadowedFiles\": " << ctx.totalShadowedFiles << ",\n";

    f << "  \"suggestedExcludeDirs\": [";
    for (size_t i = 0; i < ctx.suggestedExcludeDirs.size(); i++) {
        if (i) f << ", ";
        f << "\"" << JsonEscape(ctx.suggestedExcludeDirs[i]) << "\"";
    }
    f << "],\n";

    f << "  \"files\": {\n";

    bool firstFile = true;
    for (const auto& kv : ctx.files) {
        if (!firstFile) f << ",\n";
        firstFile = false;
        const std::string& rel = kv.first;
        const ConsumerFileAnalysis& a = kv.second;

        f << "    \"" << JsonEscape(rel) << "\": {\n";
        f << "      \"replacements\": " << a.replacements << ",\n";
        f << "      \"shadowedCount\": " << a.shadowed.size() << ",\n";

        f << "      \"shadowed\": [";
        {
            std::vector<std::string> v(a.shadowed.begin(), a.shadowed.end());
            std::sort(v.begin(), v.end());
            for (size_t i = 0; i < v.size(); i++) {
                if (i) f << ", ";
                f << "\"" << JsonEscape(v[i]) << "\"";
            }
        }
        f << "],\n";

        f << "      \"suspicious\": [";
        for (size_t i = 0; i < a.suspicious.size(); i++) {
            if (i) f << ", ";
            const auto& s = a.suspicious[i];
            f << "{\"ident\":\"" << JsonEscape(s.ident) << "\",\"reason\":\"" << JsonEscape(s.reason);
            if (!s.detail.empty()) {
                f << "\",\"detail\":\"" << JsonEscape(s.detail);
            }
            f << "\",\"line\":" << s.line << ",\"col\":" << s.col << "}";
        }
        f << "]\n";
        f << "    }";
    }

    f << "\n  }\n";
    f << "}\n";
}

void WriteSymbolDBTsv(const std::string& outPath, const ApiSymbolDB& db) {
    std::ofstream f(outPath, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open symbol DB for writing: " + outPath);

    f << "name\tkind\tparamMin\tvarargs\torigin\toriginLine\tdecl\n";

    std::vector<std::string> names;
    names.reserve(db.size());
    for (const auto& kv : db) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        const ApiSymbolInfo& s = db.at(name);
        f << s.name << "\t" << KindToStr(s.kind) << "\t" << s.paramMin << "\t" << (s.varargs ? 1 : 0) << "\t"
          << s.origin << "\t" << s.originLine << "\t" << s.decl << "\n";
    }
}

ApiSymbolDB LoadSymbolDBTsv(const std::string& inPath) {
    std::ifstream f(inPath, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open symbol DB for reading: " + inPath);

    ApiSymbolDB db;

    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;

        std::vector<std::string> cols;
        cols.reserve(7);
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                cols.push_back(line.substr(start));
                break;
            }
            cols.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (cols.size() < 4) continue;

        ApiSymbolInfo s;
        s.name = cols[0];
        s.kind = StrToKind(cols[1]);
        s.paramMin = std::atoi(cols[2].c_str());
        s.varargs = (std::atoi(cols[3].c_str()) != 0);
        if (cols.size() >= 5) s.origin = cols[4];
        if (cols.size() >= 6) s.originLine = std::atoi(cols[5].c_str());
        if (cols.size() >= 7) s.decl = cols[6];

        db.emplace(s.name, std::move(s));
    }

    return db;
}

} // namespace rlren
