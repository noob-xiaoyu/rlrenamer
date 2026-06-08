#include "multi_round_strict.h"

#include "consumer_heuristics.h"
#include "fs_utils.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <sstream>
#include <regex>
#include <unordered_set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rlren {

namespace {

static inline std::string ToGenericLower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static inline bool StartsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static bool IsUnderRoot(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    fs::path cr = fs::weakly_canonical(root, ec);
    fs::path cp = fs::weakly_canonical(p, ec);
    if (ec) return false;

    auto crs = cr.generic_string();
    auto cps = cp.generic_string();
    if (!crs.empty() && crs.back() != '/') crs.push_back('/');
    return StartsWith(cps, crs);
}

static std::vector<std::string> ParseIncludeDirectives(const std::string& text) {
    // We keep only the include path string, without <>/"".
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;

    // Simple pattern: # include "path" or <path>
    static const std::regex kInc(R"(^\s*#\s*include\s*([<\"])([^>\"]+)[>\"])");

    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, kInc)) {
            if (m.size() >= 3) {
                out.push_back(m[2].str());
            }
        }
    }
    return out;
}

static fs::path TryResolveInclude(const fs::path& root, const fs::path& includingFile, const std::string& inc) {
    // Try relative to including file first.
    fs::path p1 = includingFile.parent_path() / fs::path(inc);
    if (fs::exists(p1)) return p1;

    // Try relative to root.
    fs::path p2 = root / fs::path(inc);
    if (fs::exists(p2)) return p2;

    return {};
}

static std::unordered_set<std::string> DetectShadowedWithDefines(const std::string& text, const ApiSymbolDB& raylibDb) {
    std::unordered_set<std::string> s = DetectShadowedNamesInConsumerFile(text, raylibDb);

    // Add #define IDENT forms (macro shadowing).
    std::istringstream in(text);
    std::string line;
    static const std::regex kDef(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, kDef)) {
            if (m.size() >= 2) {
                const std::string ident = m[1].str();
                if (raylibDb.find(ident) != raylibDb.end()) s.insert(ident);
            }
        }
    }
    return s;
}

static void MergeOrigins(std::unordered_map<std::string, std::vector<std::string>>& dst,
                         const std::unordered_map<std::string, std::vector<std::string>>& src) {
    for (const auto& kv : src) {
        auto& v = dst[kv.first];
        for (const auto& o : kv.second) {
            if (std::find(v.begin(), v.end(), o) == v.end()) {
                v.push_back(o);
                if (v.size() >= 3) break; // keep it short
            }
        }
    }
}

static std::string JoinOrigins(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) out += ", ";
        out += v[i];
    }
    return out;
}

} // namespace

MultiRoundStrictCheckResult RunMultiRoundStrictCheck(
    const fs::path& root,
    const std::vector<fs::path>& projectFiles,
    const std::vector<std::string>& excludeDirNames,
    const ApiSymbolDB& raylibDb) {

    MultiRoundStrictCheckResult result;

    // Normalize root
    fs::path rootCanon = fs::weakly_canonical(root);

    // Build per-file text cache and include lists.
    struct FileInfo {
        std::string rel;                   // relative path under root (generic)
        std::string abs;                   // absolute canonical generic string
        std::string text;
        std::vector<std::string> includes; // resolved include abs keys (in order)
        std::unordered_map<std::string, std::vector<std::string>> localDefs; // ident -> origins
    };

    std::unordered_map<std::string, FileInfo> filesByAbs; // absKey -> info

    auto makeAbsKey = [&](const fs::path& p) -> std::string {
        fs::path cp = fs::weakly_canonical(p);
        return cp.generic_string();
    };

    // Load all project files.
    for (const auto& p : projectFiles) {
        if (PathHasExcludedDir(p, excludeDirNames)) continue;
        if (!fs::exists(p) || !fs::is_regular_file(p)) continue;

        std::string absKey = makeAbsKey(p);
        if (filesByAbs.find(absKey) != filesByAbs.end()) continue;

        FileInfo fi;
        fi.abs = absKey;
        fi.rel = fs::relative(p, rootCanon).generic_string();
        fi.text = ReadFileBinary(p);

        // Local defs: only for names that are in raylibDb.
        std::unordered_set<std::string> shadowed = DetectShadowedWithDefines(fi.text, raylibDb);
        for (const auto& name : shadowed) {
            fi.localDefs[name] = {fi.rel};
        }

        filesByAbs.emplace(absKey, std::move(fi));
    }

    // Resolve includes.
    for (auto& kv : filesByAbs) {
        FileInfo& fi = kv.second;
        auto incs = ParseIncludeDirectives(fi.text);
        fs::path includingAbs = fs::path(fi.abs);

        for (const auto& inc : incs) {
            fs::path resolved = TryResolveInclude(rootCanon, includingAbs, inc);
            if (resolved.empty()) continue;
            if (!IsUnderRoot(rootCanon, resolved)) continue;
            if (PathHasExcludedDir(resolved, excludeDirNames)) continue;

            std::string incKey = makeAbsKey(resolved);
            // Only follow includes that are part of the project file set.
            if (filesByAbs.find(incKey) == filesByAbs.end()) continue;
            fi.includes.push_back(incKey);
        }
    }

    // Compute closure (transitive local defs) for each file.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> closureMemo;
    std::unordered_set<std::string> inStack;

    std::function<const std::unordered_map<std::string, std::vector<std::string>>& (const std::string&)> getClosure;

    getClosure = [&](const std::string& absKey) -> const std::unordered_map<std::string, std::vector<std::string>>& {
        auto it = closureMemo.find(absKey);
        if (it != closureMemo.end()) return it->second;

        if (inStack.find(absKey) != inStack.end()) {
            // Cycle: return local defs only.
            closureMemo[absKey] = filesByAbs[absKey].localDefs;
            return closureMemo[absKey];
        }

        inStack.insert(absKey);

        std::unordered_map<std::string, std::vector<std::string>> merged = filesByAbs[absKey].localDefs;
        for (const auto& incKey : filesByAbs[absKey].includes) {
            const auto& child = getClosure(incKey);
            MergeOrigins(merged, child);
        }

        inStack.erase(absKey);
        closureMemo[absKey] = std::move(merged);
        return closureMemo[absKey];
    };

    for (const auto& kv : filesByAbs) {
        (void)getClosure(kv.first);
    }

    // Translation unit roots.
    std::vector<std::string> tuRoots;
    for (const auto& kv : filesByAbs) {
        const FileInfo& fi = kv.second;
        fs::path p = fs::path(fi.abs);
        std::string ext = ToGenericLower(p.extension().string());
        if (ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx") {
            tuRoots.push_back(fi.abs);
        }
    }

    result.translationUnits = tuRoots.size();

    // For each file, names defined before the file is entered in any TU expansion.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> preShadowedByAbs;

    for (const auto& tu : tuRoots) {
        std::unordered_set<std::string> expanded;
        std::unordered_map<std::string, std::vector<std::string>> active; // ident -> origins

        std::function<void(const std::string&)> expand;
        expand = [&](const std::string& absKey) {
            if (expanded.find(absKey) != expanded.end()) return;
            expanded.insert(absKey);

            // Record what is already active before entering this file.
            MergeOrigins(preShadowedByAbs[absKey], active);

            // Conservative: local definitions in this file may be visible for the remainder.
            MergeOrigins(active, filesByAbs[absKey].localDefs);

            // Expand includes in order.
            for (const auto& incKey : filesByAbs[absKey].includes) {
                expand(incKey);
                // After including a header, its closure becomes visible.
                MergeOrigins(active, closureMemo[incKey]);
            }
        };

        expand(tu);
    }

    // Convert to result map (relative paths).
    for (const auto& kv : preShadowedByAbs) {
        const std::string& absKey = kv.first;
        const auto& idents = kv.second;
        const auto fit = filesByAbs.find(absKey);
        if (fit == filesByAbs.end()) continue;

        const std::string& rel = fit->second.rel;
        if (rel.empty()) continue;

        auto& outMap = result.includerShadowed[rel];
        for (const auto& ikv : idents) {
            const std::string& ident = ikv.first;
            const std::vector<std::string>& origins = ikv.second;
            if (origins.empty()) continue;

            outMap.emplace(ident, std::string("defined earlier in includer context: ") + JoinOrigins(origins));
        }
    }

    // Summary stats.
    for (const auto& kv : result.includerShadowed) {
        if (!kv.second.empty()) {
            result.filesWithConflicts++;
            result.totalConflicts += kv.second.size();
        }
    }

    return result;
}

void WriteMultiRoundStrictReportTsv(const fs::path& outPath, const MultiRoundStrictCheckResult& r) {
    std::ofstream f(outPath, std::ios::binary);
    if (!f) throw std::runtime_error("Failed to open multi-round report for writing: " + outPath.string());

    f << "file\tident\tdetail\n";
    std::vector<std::string> files;
    files.reserve(r.includerShadowed.size());
    for (const auto& kv : r.includerShadowed) files.push_back(kv.first);
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        const auto& m = r.includerShadowed.at(file);
        std::vector<std::string> idents;
        idents.reserve(m.size());
        for (const auto& kv : m) idents.push_back(kv.first);
        std::sort(idents.begin(), idents.end());
        for (const auto& ident : idents) {
            f << file << "\t" << ident << "\t" << m.at(ident) << "\n";
        }
    }
}

} // namespace rlren
