#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace Caesura::carc {

inline std::filesystem::path pathFromUtf8(std::string_view path) {
    // Reject embedded NUL before a native file API can use only the prefix.
    if (path.find('\0') != std::string_view::npos) return {};
    try {
        return std::filesystem::path(std::u8string(path.begin(), path.end()));
    } catch (const std::system_error&) {
#ifdef _WIN32
        // Older callers pass path.string() in the native code page. Only
        // accept a lossless legacy spelling when UTF-8 decoding failed.
        try {
            std::filesystem::path native(path);
            if (native.string() == path) return native;
        } catch (const std::system_error&) {
            // Neither encoding names a usable filesystem path.
        }
#endif
        return {};
    }
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace Caesura::carc
