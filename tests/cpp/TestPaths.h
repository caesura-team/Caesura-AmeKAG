#pragma once

#include <atomic>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Caesura::TestPaths {

inline unsigned long currentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

inline std::string sanitize(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(std::isalnum(ch) ? static_cast<char>(ch) : '_');
    }
    return out.empty() ? "case" : out;
}

inline std::filesystem::path uniqueTempDir(const std::string& name,
        const std::filesystem::path& parent = std::filesystem::temp_directory_path()) {
    static std::atomic<unsigned long> counter{0};
    std::ostringstream dirName;
    dirName << "caesura_test_" << sanitize(name)
            << "_p" << currentProcessId()
            << "_" << counter.fetch_add(1);
    return parent / dirName.str();
}

inline std::string withTrailingSeparator(const std::filesystem::path& path) {
    auto value = path.string();
    if (!value.empty() && value.back() != '/' && value.back() != '\\') {
        value.push_back(std::filesystem::path::preferred_separator);
    }
    return value;
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::string& name,
            const std::filesystem::path& parent = std::filesystem::temp_directory_path())
        : m_path(uniqueTempDir(name, parent)) {
        std::filesystem::remove_all(m_path);
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    const std::filesystem::path& path() const { return m_path; }
    std::string string() const { return withTrailingSeparator(m_path); }

private:
    std::filesystem::path m_path;
};

} // namespace Caesura::TestPaths
