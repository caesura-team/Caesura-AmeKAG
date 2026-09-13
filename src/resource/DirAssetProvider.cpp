#include "DirAssetProvider.h"
#include <exception>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Caesura {

fs::path DirAssetProvider::fullPath(const std::string& path) const
{
    if (path.empty()) return {};

    try {
        // Public asset paths and configured roots are UTF-8. Keep the native
        // path representation through canonicalization and I/O, so Windows
        // never converts a validated path back through the ANSI code page.
        const fs::path requested = fs::u8path(path);
        if (requested.is_absolute() || requested.has_root_name()) return {};
        for (const auto& part : requested) {
            if (part == "..") return {};
        }

        std::error_code ec;
        fs::path root = m_rootDir.empty()
            ? fs::current_path(ec)
            : fs::absolute(fs::u8path(m_rootDir), ec);
        if (ec) return {};
        root = fs::weakly_canonical(root, ec);
        if (ec) return {};

        const fs::path candidate = fs::weakly_canonical(root / requested, ec);
        if (ec) return {};
        const fs::path relative = candidate.lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) return {};
        for (const auto& part : relative) {
            if (part == "..") return {};
        }
        return candidate;
    } catch (const std::exception&) {
        // Invalid path encodings must obey the provider's failure contract,
        // including direct callers which do not use ProviderChain's guard.
        return {};
    }
}

std::vector<uint8_t> DirAssetProvider::read(const std::string& path)
{
    const fs::path fp = fullPath(path);
    if (fp.empty()) return {};

    std::ifstream file(fp, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    const std::streamsize size = file.tellg();
    if (size <= 0) return {};

    // Cap a single asset read (review RD-3): a corrupt or misleading file
    // must not trigger a multi-GB heap allocation. 512 MiB matches the
    // documented per-asset ceiling used by the packaged formats.
    constexpr std::streamsize kMaxAssetBytes = 512ull * 1024ull * 1024ull;
    if (size > kMaxAssetBytes) return {};

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file) return {};
    return data;
}

bool DirAssetProvider::exists(const std::string& path)
{
    const fs::path fp = fullPath(path);
    if (fp.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(fp, ec) && !ec;
}

} // namespace Caesura
