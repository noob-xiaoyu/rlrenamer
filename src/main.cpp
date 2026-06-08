#include "lexer.h"
#include "raylib_api_extract.h"
#include "consumer_heuristics.h"
#include "fs_utils.h"
#include "multi_round_strict.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace rlren {

struct Args {
    fs::path root;
    fs::path outDir;
    bool inPlace = false;
    bool dryRun = false;
    fs::path reportPath;
    std::string prefix = "RL_";
    std::string enumPrefix; // optional: prefix for enum values (when enabled)

    // Symbol source
    fs::path symbolsFile;      // name-only
    fs::path symbolsDbFile;    // TSV DB
    fs::path raylibHeader;
    fs::path raylibSrc;

    // Function specifier macros to detect in raylib headers (repeatable / comma-separated)
    std::vector<std::string> apiDefines;

    // Optional extraction categories
    bool withMacros = false;
    bool withEnumValues = false;

    // When --raylib-src is provided, the caller must also specify one of these modes:
    //  - strictPublicApi: only use --raylib-h (public header) as the symbol baseline
    //  - usingExpandHeaders: additionally scan headers under --raylib-src to expand the symbol set
    bool usingExpandHeaders = false;
    bool strictPublicApi = false;

    // Exclusions while scanning --raylib-src for identifiers
    std::vector<std::string> extractExcludeDirs;

    fs::path dumpSymbols;
    fs::path dumpSymbolsDb;

    // Target exclusions
    std::vector<std::string> excludeDirs;

    // Consumer mode (JC-like) validation
    bool consumerMode = false;
    bool consumerForce = false;
    bool consumerRewritePreproc = false;
    int consumerMaxSamplesPerFile = 24;

    fs::path exportConsumerContext;

    // Multi-round strict check (consumer-mode only): build include-order context and skip risky rewrites.
    bool multiRoundStrictCheck = false;
};

static void PrintUsage() {
    std::cout
        << "raylib_renamer (lexer-based C/C++ identifier rewriter)\n\n"
        << "USAGE:\n"
        << "  raylib_renamer --root <dir> (--out <dir> | --in-place) [options]\n\n"
        << "SYMBOL SOURCE (choose one):\n"
        << "  --symbols-db <file>        Load a TSV symbol DB (recommended for consumer-mode)\n"
        << "  --symbols-file <file>      Load newline-separated identifier list (legacy)\n"
        << "  --raylib-h <file>          Extract identifiers+defs from raylib header (can be combined with --raylib-src)\n"
        << "  --raylib-src <dir>         Provide a raylib source tree (requires --using-expand-headers or --strict-public-api)\n\n"
        << "GENERAL OPTIONS:\n"
        << "  --prefix <str>             Prefix to add (default: RL_)\n"
        << "  --enum-prefix <str>        Prefix for enum values (when --with-enum-values is enabled). Defaults to --prefix\n"
        << "  --api-define <a,b,c>       Function specifier macros (default: RLAPI,RMAPI)\n"
        << "  --with-macros              Also extract #define macro names (OFF by default)\n"
        << "  --with-enum-values         Also extract enum value names (OFF by default)\n"
        << "  --using-expand-headers     (Requires --raylib-src) Expand symbol extraction to headers under --raylib-src\n"
        << "  --strict-public-api        (Requires --raylib-src) Use only --raylib-h as the public API baseline (no extra headers)\n"
        << "  --extract-exclude-dir <a,b,c>  Exclude directories by name when scanning --raylib-src (repeatable; default excludes: external,.git,build,out)\n"
        << "  --exclude-dir <a,b,c>      Exclude directories by name under --root (repeatable)\n"
        << "  --dump-symbols <file>      Write extracted/loaded identifiers (name-only) to file\n"
        << "  --dump-symbols-db <file>   Write extracted/loaded symbol DB (TSV) to file\n"
        << "  --dry-run                  Analyze and report, don't write files\n"
        << "  --report <path>            Write a TSV usage report (file + replacement counts)\n\n"
        << "CONSUMER MODE (safe rewrite for projects like JackalClient):\n"
        << "  --consumer-mode            Enable validation heuristics (skip shadowed/qualified/suspicious by default)\n"
        << "  --consumer-force           Replace even when suspicious (still reported)\n"
        << "  --consumer-rewrite-preproc Allow rewriting inside preprocessor directives\n"
        << "  --consumer-max-samples <N> Max suspicious samples recorded per file (default: 24)\n"
        << "  --export-consumer-context <file>  Export consumer analysis context as JSON\n"
        << "  --multi-round-strict-check   Multi-pass include-order analysis; skip rewrites that are shadowed by consumer definitions in includer context\n";
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; i++) {
        std::string key = argv[i];
        auto need = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + opt);
            return std::string(argv[++i]);
        };

        if (key == "-h" || key == "--help") {
            PrintUsage();
            return false;
        } else if (key == "--root" || key == "--consumer-root") {
            a.root = need("--root");
        } else if (key == "--out") {
            a.outDir = need("--out");
        } else if (key == "--in-place") {
            a.inPlace = true;
        } else if (key == "--dry-run") {
            a.dryRun = true;
        } else if (key == "--report") {
            a.reportPath = need("--report");
        } else if (key == "--prefix") {
            a.prefix = need("--prefix");
        } else if (key == "--enum-prefix" || key == "--enum-value-prefix") {
            a.enumPrefix = need("--enum-prefix");
        } else if (key == "--symbols-file") {
            a.symbolsFile = need("--symbols-file");
        } else if (key == "--symbols-db") {
            a.symbolsDbFile = need("--symbols-db");
        } else if (key == "--raylib-h") {
            a.raylibHeader = need("--raylib-h");
        } else if (key == "--raylib-src") {
            a.raylibSrc = need("--raylib-src");
        } else if (key == "--api-define") {
            std::string v = need("--api-define");
            auto parts = SplitComma(v);
            a.apiDefines.insert(a.apiDefines.end(), parts.begin(), parts.end());
        } else if (key == "--with-macros") {
            a.withMacros = true;
        } else if (key == "--with-enum-values") {
            a.withEnumValues = true;
        } else if (key == "--include-source-defs") {
            throw std::runtime_error("--include-source-defs was removed; use --using-expand-headers or --strict-public-api");
        } else if (key == "--using-expand-headers") {
            a.usingExpandHeaders = true;
        } else if (key == "--strict-public-api") {
            a.strictPublicApi = true;
        } else if (key == "--extract-exclude-dir") {
            std::string v = need("--extract-exclude-dir");
            auto parts = SplitComma(v);
            a.extractExcludeDirs.insert(a.extractExcludeDirs.end(), parts.begin(), parts.end());
        } else if (key == "--exclude-dir") {
            std::string v = need("--exclude-dir");
            auto parts = SplitComma(v);
            a.excludeDirs.insert(a.excludeDirs.end(), parts.begin(), parts.end());
        } else if (key == "--dump-symbols") {
            a.dumpSymbols = need("--dump-symbols");
        } else if (key == "--dump-symbols-db") {
            a.dumpSymbolsDb = need("--dump-symbols-db");
        } else if (key == "--consumer-mode") {
            a.consumerMode = true;
        } else if (key == "--consumer-force") {
            a.consumerForce = true;
        } else if (key == "--consumer-rewrite-preproc") {
            a.consumerRewritePreproc = true;
        } else if (key == "--consumer-max-samples") {
            a.consumerMaxSamplesPerFile = std::stoi(need("--consumer-max-samples"));
        } else if (key == "--export-consumer-context") {
            a.exportConsumerContext = need("--export-consumer-context");
        } else if (key == "--multi-round-strict-check") {
            a.multiRoundStrictCheck = true;
        } else {
            throw std::runtime_error("Unknown argument: " + key);
        }
    }

    if (a.root.empty()) throw std::runtime_error("--root is required");
    if (!a.inPlace && a.outDir.empty()) throw std::runtime_error("Either --out or --in-place is required");
    if (a.inPlace && !a.outDir.empty()) throw std::runtime_error("Do not use --out together with --in-place");

    const bool hasSymbolsFile = !a.symbolsFile.empty();
    const bool hasSymbolsDb = !a.symbolsDbFile.empty();
    const bool hasRaylibHeader = !a.raylibHeader.empty();
    const bool hasRaylibSrc = !a.raylibSrc.empty();

    // If --raylib-src is provided but --raylib-h is omitted, try common default locations.
    if (hasRaylibSrc && !hasRaylibHeader) {
        fs::path root = a.raylibSrc;
        fs::path cand1 = root / "src" / "raylib.h";
        fs::path cand2 = root / "raylib.h";
        if (fs::exists(cand1)) a.raylibHeader = cand1;
        else if (fs::exists(cand2)) a.raylibHeader = cand2;
    }

    const int sources = (hasSymbolsDb ? 1 : 0) + (hasSymbolsFile ? 1 : 0) + ((hasRaylibHeader || hasRaylibSrc) ? 1 : 0);
    if (sources != 1) {
        throw std::runtime_error("Choose exactly one symbol source: --symbols-db OR --symbols-file OR (--raylib-h/--raylib-src)");
    }

    // Defaults for API define macros
    if ((hasRaylibHeader || hasRaylibSrc) && a.apiDefines.empty()) {
        a.apiDefines = {"RLAPI", "RMAPI"};
    }

    // Defaults for raylib-src scanning exclusions
    if (hasRaylibSrc && a.extractExcludeDirs.empty()) {
        a.extractExcludeDirs = {"external", ".git", "build", "out"};
    }

    // Mode selection for --raylib-src
    if (hasRaylibSrc) {
        if (a.usingExpandHeaders == a.strictPublicApi) {
            throw std::runtime_error("When using --raylib-src, specify exactly one of --using-expand-headers or --strict-public-api");
        }
        if (a.strictPublicApi && a.raylibHeader.empty()) {
            throw std::runtime_error("--strict-public-api requires a public header: provide --raylib-h (or a raylib-src containing src/raylib.h)");
        }
    } else {
        if (a.usingExpandHeaders || a.strictPublicApi) {
            throw std::runtime_error("--using-expand-headers/--strict-public-api require --raylib-src");
        }
    }

    if (a.consumerMaxSamplesPerFile < 0) a.consumerMaxSamplesPerFile = 0;

    return true;
}

static std::unordered_set<std::string> LoadSymbolsFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) throw std::runtime_error("Failed to open symbols file: " + p.string());
    std::unordered_set<std::string> syms;
    std::string line;
    while (std::getline(in, line)) {
        // trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) start++;
        if (start > 0) line.erase(0, start);
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        syms.insert(line);
    }
    return syms;
}

static void DumpSymbols(const fs::path& outFile, const std::unordered_set<std::string>& syms) {
    std::vector<std::string> v(syms.begin(), syms.end());
    std::sort(v.begin(), v.end());
    std::ofstream out(outFile);
    if (!out) throw std::runtime_error("Failed to open dump file: " + outFile.string());
    for (const auto& s : v) out << s << "\n";
}

static std::unordered_map<std::string, std::string> BuildRenameMap(const ApiSymbolDB& db,
                                                                   const std::string& prefix,
                                                                   const std::string& enumPrefix) {
    std::unordered_map<std::string, std::string> m;
    m.reserve(db.size());

    // Never rename C/C++/preprocessor keywords or common built-ins even if they were
    // accidentally collected from headers.
    const std::unordered_set<std::string> kNeverRename = {
        "defined",
        "bool", "true", "false",
        "nullptr", "NULL",
    };

    for (const auto& kv : db) {
        const auto& s = kv.first;
        const auto& info = kv.second;
        if (s.empty()) continue;
        if (kNeverRename.find(s) != kNeverRename.end()) continue;
        if (s.rfind(prefix, 0) == 0) continue; // already prefixed
        if (!enumPrefix.empty() && s.rfind(enumPrefix, 0) == 0) continue;
        if (s == "RLAPI" || s == "RMAPI" || s == "RGAPI" || s == "RLGL" || s == "RAYLIB_H") continue;

        const bool isEnumValue = (info.kind == ApiSymbolKind::EnumValue);
        const std::string& usePrefix = (isEnumValue && !enumPrefix.empty()) ? enumPrefix : prefix;
        m.emplace(s, usePrefix + s);
    }
    return m;
}

static std::vector<std::string> ListTopLevelDirs(const fs::path& root) {
    std::vector<std::string> out;
    if (!fs::exists(root) || !fs::is_directory(root)) return out;
    for (auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory()) continue;
        out.push_back(e.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace rlren

int main(int argc, char** argv) {
    using namespace rlren;

    Args args;
    try {
        if (!ParseArgs(argc, argv, args)) return 0;

        ApiSymbolDB symbolDb;

        if (!args.symbolsDbFile.empty()) {
            symbolDb = LoadSymbolDBTsv(args.symbolsDbFile.string());
        } else if (!args.symbolsFile.empty()) {
            auto names = LoadSymbolsFile(args.symbolsFile);
            for (const auto& n : names) {
                ApiSymbolInfo s;
                s.name = n;
                s.kind = ApiSymbolKind::Unknown;
                symbolDb.emplace(n, std::move(s));
            }
        } else {
            ApiExtractOptions opt;
            opt.apiDefines = args.apiDefines;
            opt.includeDefineMacros = args.withMacros;
            opt.includeEnumValues = args.withEnumValues;

            if (!args.raylibHeader.empty()) {
                std::string header = ReadFileBinary(args.raylibHeader);
                auto db = ExtractRaylibApiSymbolDBFromHeader(header, opt, args.raylibHeader.filename().string());
                for (const auto& kv : db) symbolDb.emplace(kv.first, kv.second);
            }

            if (!args.raylibSrc.empty()) {
                fs::path scanRoot = args.raylibSrc;
                if (fs::exists(scanRoot / "src") && fs::is_directory(scanRoot / "src")) {
                    scanRoot = scanRoot / "src";
                }

                // Expand symbol extraction beyond the public header only when explicitly enabled.
                if (args.usingExpandHeaders) {
                    std::vector<std::string> hExts = {".h", ".hpp"};
                    auto headers = CollectSourceFiles(scanRoot, hExts, args.extractExcludeDirs);
                    for (const auto& hp : headers) {
                        std::string txt = ReadFileBinary(hp);
                        auto db = ExtractRaylibApiSymbolDBFromHeader(txt, opt, fs::relative(hp, scanRoot).generic_string());
                        for (const auto& kv : db) {
                            auto it = symbolDb.find(kv.first);
                            if (it == symbolDb.end()) symbolDb.emplace(kv.first, kv.second);
                            else {
                                // merge (prefer richer)
                                if (it->second.kind == ApiSymbolKind::Unknown && kv.second.kind != ApiSymbolKind::Unknown) it->second.kind = kv.second.kind;
                                if (it->second.paramMin < 0 && kv.second.paramMin >= 0) it->second.paramMin = kv.second.paramMin;
                                if (!it->second.varargs && kv.second.varargs) it->second.varargs = true;
                                if (it->second.decl.empty() && !kv.second.decl.empty()) it->second.decl = kv.second.decl;
                                if (it->second.origin.empty() && !kv.second.origin.empty()) { it->second.origin = kv.second.origin; it->second.originLine = kv.second.originLine; }
                            }
                        }
                    }
                }
            }
        }

        // Name-only view (for dump-symbols)
        std::unordered_set<std::string> symbols;
        symbols.reserve(symbolDb.size());
        for (const auto& kv : symbolDb) symbols.insert(kv.first);

        if (!args.dumpSymbols.empty()) {
            DumpSymbols(args.dumpSymbols, symbols);
        }
        if (!args.dumpSymbolsDb.empty()) {
            WriteSymbolDBTsv(args.dumpSymbolsDb.string(), symbolDb);
        }

        const std::string enumPrefix = args.enumPrefix.empty() ? args.prefix : args.enumPrefix;
        auto renameMap = BuildRenameMap(symbolDb, args.prefix, enumPrefix);

        // Collect macro names for macro-aware rewrite:
        // macro names are only renamed in #define / #ifdef / #ifndef / #undef / defined() positions.
        std::unordered_set<std::string> macroNames;
        for (const auto& kv : symbolDb) {
            if (kv.second.kind == ApiSymbolKind::DefineMacro) {
                macroNames.insert(kv.first);
            }
        }

        struct ReportRow {
            std::string relPath;
            std::size_t replacements = 0;
            std::size_t uniqueIdents = 0;
            std::vector<std::pair<std::string, std::size_t>> top;
        };
        std::vector<ReportRow> reportRows;

        std::vector<std::string> exts = {".h", ".hpp", ".c", ".cpp", ".cc", ".cxx"};
        auto files = CollectSourceFiles(args.root, exts, args.excludeDirs);

        ConsumerProjectContext consumerCtx;
        consumerCtx.totalFiles = files.size();
        consumerCtx.suggestedExcludeDirs = SuggestConsumerExcludeDirs(ListTopLevelDirs(args.root));

        size_t changedFiles = 0;
        size_t totalFiles = files.size();

        ConsumerModeOptions cm;
        cm.replaceSuspicious = args.consumerForce;
        cm.rewritePreprocessor = args.consumerRewritePreproc;
        cm.maxSamplesPerFile = args.consumerMaxSamplesPerFile;

        // Multi-round strict check (consumer mode only)
        MultiRoundStrictCheckResult mr;
        fs::path mrReportPath;
        if (args.consumerMode && args.multiRoundStrictCheck) {
            mr = RunMultiRoundStrictCheck(args.root, files, args.excludeDirs, symbolDb);
            // Write a dedicated report next to the main report/context when possible.
            if (!args.outDir.empty()) {
                mrReportPath = args.outDir / "multi_round_conflicts.tsv";
            } else if (!args.reportPath.empty()) {
                mrReportPath = args.reportPath.parent_path() / "multi_round_conflicts.tsv";
            } else if (!args.exportConsumerContext.empty()) {
                mrReportPath = fs::path(args.exportConsumerContext).parent_path() / "multi_round_conflicts.tsv";
            } else {
                mrReportPath = args.root / "multi_round_conflicts.tsv";
            }
            // Ensure parent dir exists
            {
                std::error_code ec;
                fs::create_directories(mrReportPath.parent_path(), ec);
            }
            WriteMultiRoundStrictReportTsv(mrReportPath, mr);
        }

        for (const auto& p : files) {
            std::string inText = ReadFileBinary(p);
            std::string outText;

            RewriteStats stats;

            if (!args.consumerMode) {
                if (args.reportPath.empty()) {
                    outText = RewriteWithMacroAwareness(inText, renameMap, nullptr, macroNames);
                } else {
                    outText = RewriteWithMacroAwareness(inText, renameMap, &stats, macroNames);
                }
            } else {
                // Consumer mode: shadow scan + validated rewrite
                std::unordered_set<std::string> shadowed = DetectShadowedNamesInConsumerFile(inText, symbolDb);

                const std::unordered_map<std::string, std::string>* includerShadowed = nullptr;
                if (args.multiRoundStrictCheck) {
                    const std::string relKey = fs::relative(p, args.root).generic_string();
                    auto it = mr.includerShadowed.find(relKey);
                    if (it != mr.includerShadowed.end()) includerShadowed = &it->second;
                }

                ConsumerFileAnalysis analysis;
                outText = RewriteConsumerWithValidation(inText, symbolDb, args.prefix, enumPrefix, shadowed, includerShadowed, cm, &analysis);

                consumerCtx.totalReplacements += analysis.replacements;
                consumerCtx.totalSuspicious += analysis.suspicious.size();
                if (!analysis.shadowed.empty()) consumerCtx.totalShadowedFiles++;

                const std::string rel = fs::relative(p, args.root).generic_string();
                consumerCtx.files.emplace(rel, std::move(analysis));

                // fill stats for report if requested
                if (!args.reportPath.empty()) {
                    stats.replacements = consumerCtx.files[rel].replacements;
                    stats.perIdentifier = consumerCtx.files[rel].perIdent;
                }
            }

            if (!args.reportPath.empty() && stats.replacements > 0) {
                ReportRow row;
                row.relPath = fs::relative(p, args.root).generic_string();
                row.replacements = stats.replacements;
                row.uniqueIdents = stats.perIdentifier.size();
                for (const auto& kv : stats.perIdentifier) row.top.emplace_back(kv.first, kv.second);
                std::sort(row.top.begin(), row.top.end(), [](const auto& a, const auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                });
                if (row.top.size() > 5) row.top.resize(5);
                reportRows.push_back(std::move(row));
            }

            if (outText != inText) {
                changedFiles++;
                if (!args.dryRun) {
                    if (args.inPlace) {
                        WriteFileBinary(p, outText);
                    } else {
                        fs::path rel = fs::relative(p, args.root);
                        fs::path dst = args.outDir / rel;
                        WriteFileBinary(dst, outText);
                    }
                }
            } else {
                if (!args.dryRun && !args.inPlace && !args.outDir.empty()) {
                    fs::path rel = fs::relative(p, args.root);
                    fs::path dst = args.outDir / rel;
                    WriteFileBinary(dst, inText);
                }
            }
        }

        if (!args.reportPath.empty()) {
            std::ofstream rep(args.reportPath, std::ios::binary);
            if (!rep) throw std::runtime_error("Failed to open report file for writing: " + args.reportPath.string());
            rep << "file\treplacements\tunique\ttop\n";
            for (const auto& row : reportRows) {
                rep << row.relPath << "\t" << row.replacements << "\t" << row.uniqueIdents << "\t";
                for (size_t i = 0; i < row.top.size(); i++) {
                    if (i) rep << "|";
                    rep << row.top[i].first << ":" << row.top[i].second;
                }
                rep << "\n";
            }
            std::cout << "Report written: " << args.reportPath.string() << "\n";
        }

        if (!args.exportConsumerContext.empty()) {
            consumerCtx.changedFiles = changedFiles;
            WriteConsumerContextJson(args.exportConsumerContext.string(), consumerCtx);
            std::cout << "Consumer context written: " << args.exportConsumerContext.string() << "\n";
        }

        std::cout << "Scanned files: " << totalFiles << "\n";
        std::cout << "Symbols loaded: " << symbolDb.size() << "\n";
        std::cout << "Files changed: " << changedFiles << (args.dryRun ? " (dry-run)" : "") << "\n";
        if (args.consumerMode) {
            std::cout << "Consumer mode: replacements=" << consumerCtx.totalReplacements
                      << " suspiciousSamples=" << consumerCtx.totalSuspicious
                      << " shadowedFiles=" << consumerCtx.totalShadowedFiles << "\n";
            if (args.multiRoundStrictCheck) {
                std::cout << "Multi-round strict check: translationUnits=" << mr.translationUnits
                          << " filesWithConflicts=" << mr.filesWithConflicts
                          << " totalConflicts=" << mr.totalConflicts << "\n";
                std::cout << "Multi-round report: " << mrReportPath.string() << "\n";
            }
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        PrintUsage();
        return 1;
    }
}
