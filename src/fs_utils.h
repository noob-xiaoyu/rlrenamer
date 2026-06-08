#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rlren {

std::string ReadFileBinary(const std::filesystem::path& p);
void WriteFileBinary(const std::filesystem::path& p, const std::string& data);

// Returns list of candidate source files in root with specified extensions.
std::vector<std::filesystem::path> CollectSourceFiles(
    const std::filesystem::path& root,
    const std::vector<std::string>& exts,
    const std::vector<std::string>& excludeDirNames
);

bool PathHasExcludedDir(const std::filesystem::path& p, const std::vector<std::string>& excludeDirNames);

} // namespace rlren
