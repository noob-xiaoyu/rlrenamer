#include "fs_utils.h"

#include <fstream>
#include <stdexcept>

namespace rlren {

std::string ReadFileBinary(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Failed to open file for reading: " + p.string());
    std::string data;
    in.seekg(0, std::ios::end);
    std::streamsize sz = in.tellg();
    in.seekg(0, std::ios::beg);
    if (sz < 0) sz = 0;
    data.resize(static_cast<size_t>(sz));
    if (!data.empty()) in.read(&data[0], sz);
    return data;
}

void WriteFileBinary(const std::filesystem::path& p, const std::string& data) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Failed to open file for writing: " + p.string());
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool PathHasExcludedDir(const std::filesystem::path& p, const std::vector<std::string>& excludeDirNames) {
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string part = it->string();
        for (const auto& ex : excludeDirNames) {
            if (!ex.empty() && part == ex) return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> CollectSourceFiles(
    const std::filesystem::path& root,
    const std::vector<std::string>& exts,
    const std::vector<std::string>& excludeDirNames) {

    std::vector<std::filesystem::path> out;
    if (!std::filesystem::exists(root)) return out;

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto p = it->path();
        if (it->is_directory(ec)) {
            if (PathHasExcludedDir(p, excludeDirNames)) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file(ec)) continue;
        if (PathHasExcludedDir(p, excludeDirNames)) continue;

        auto ext = p.extension().string();
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        for (const auto& e : exts) {
            if (ext == e) {
                out.push_back(p);
                break;
            }
        }
    }

    return out;
}

} // namespace rlren
