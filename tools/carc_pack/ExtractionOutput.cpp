#include "ExtractionOutput.h"

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Caesura::carc_tool {
namespace {

fs::file_type safeType(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return fs::file_type::not_found;
    if (error) return fs::file_type::unknown;
#ifdef _WIN32
    if (fs::exists(status)) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return fs::file_type::unknown;
    }
#endif
    return status.type();
}

bool ensureDirectory(const fs::path& path) {
    const auto type = safeType(path);
    if (type != fs::file_type::not_found) return type == fs::file_type::directory;
    std::error_code error;
    fs::create_directory(path, error);
    return !error && safeType(path) == fs::file_type::directory;
}

bool canPublishTo(const fs::path& path, fs::file_type type) {
    if (type == fs::file_type::not_found) return true;
    if (type != fs::file_type::regular) return false;
    std::error_code error;
    const auto links = fs::hard_link_count(path, error);
    return !error && links == 1;
}

bool preserveAccess(const fs::path& source, const fs::path& replacement) {
#ifdef _WIN32
    // Replacement uses directory permissions, so check the file's write
    // permission separately without opening it for truncation.
    const auto writable = CreateFileW(source.c_str(), FILE_WRITE_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (writable == INVALID_HANDLE_VALUE || !CloseHandle(writable)) return false;

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetNamedSecurityInfoW(source.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    const std::unique_ptr<void, decltype(&LocalFree)> ownedDescriptor(descriptor, &LocalFree);
    if (result == ERROR_NOT_SUPPORTED) return true;
    if (result != ERROR_SUCCESS) return false;

    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)) return false;
    const auto flags = DACL_SECURITY_INFORMATION | ((control & SE_DACL_PROTECTED)
        ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    auto name = replacement.native();
    return SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, flags,
        nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
#else
    int writable;
    do {
        writable = ::open(source.c_str(), O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    } while (writable < 0 && errno == EINTR);
    if (writable < 0 || ::close(writable) != 0) return false;

    std::error_code error;
    const auto mode = fs::status(source, error).permissions();
    if (error) return false;
    fs::permissions(replacement, mode & fs::perms::all, error);
    return !error;
#endif
}

class StagingDirectory {
public:
    explicit StagingDirectory(const fs::path& parent) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = parent / (".carc-extract-" + std::to_string(stamp) +
                                       "-" + std::to_string(attempt));
            auto file = candidate / "member";
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                m_path = std::move(candidate);
                m_file = std::move(file);
                return;
            }
            if (error && error != std::errc::file_exists) return;
        }
    }

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;

    ~StagingDirectory() {
        if (m_path.empty()) return;
        std::error_code ignored;
        fs::remove(m_file, ignored);
        fs::remove(m_path, ignored);
    }

    bool ready() const { return !m_path.empty(); }
    const fs::path& file() const { return m_file; }

private:
    fs::path m_path;
    fs::path m_file;
};

} // namespace

bool validMemberPath(const fs::path& path) {
    if (path.empty() || path.has_root_path() || !path.has_filename()) return false;
    for (const auto& part : path) {
        if (part == "." || part == "..") return false;
#ifdef _WIN32
        const auto& name = part.native();
        if (name.back() == L'.' || name.back() == L' ' || name.find(L':') != std::wstring::npos) {
            return false;
        }
#endif
    }
    return true;
}

bool prepareOutputRoot(fs::path& root) {
    if (root.empty()) return false;
    root = fs::absolute(root).lexically_normal();
    if (root != root.root_path() && !root.has_filename()) root = root.parent_path();
    if (safeType(root) == fs::file_type::not_found) fs::create_directories(root);
    if (safeType(root) != fs::file_type::directory) return false;
    root = fs::canonical(root);
    return true;
}

bool writeExtractedFile(const fs::path& root, const fs::path& member,
                        std::span<const uint8_t> bytes) {
    if (!validMemberPath(member)) return false;
    auto parent = root;
    for (const auto& part : member.parent_path()) {
        parent /= part;
        if (!ensureDirectory(parent)) return false;
    }
    const auto target = parent / member.filename();
    const auto targetType = safeType(target);
    if (!canPublishTo(target, targetType)) return false;

    // Keep an existing regular file until the complete replacement is ready.
    StagingDirectory staging(parent);
    if (!staging.ready()) return false;
    const auto& temporary = staging.file();
    std::ofstream file(temporary, std::ios::binary);
    if (!file || (targetType == fs::file_type::regular && !preserveAccess(target, temporary))) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file || safeType(target) != targetType || !canPublishTo(target, targetType)) return false;

    std::error_code error;
    fs::rename(temporary, target, error);
    return !error;
}

} // namespace Caesura::carc_tool
