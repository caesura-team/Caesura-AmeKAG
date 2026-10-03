// ===========================================================================
//  Caesura (AmeKAG) — ISaveProvider.cpp
//  Local filesystem save provider implementation.
// ===========================================================================

#include "LocalFileSaveProvider.h"
#include "AtomicSaveFile.h"
#include "CloudSaveSnapshot.h"
#include <vector>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <exception>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <fstream>
#include <cstdio>
#include <filesystem>

namespace Caesura {

std::string LocalFileSaveProvider::readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz <= 0 || static_cast<size_t>(sz) > 10 * 1024 * 1024) return "";
    std::string content(static_cast<size_t>(sz), '\0');
    in.seekg(0, std::ios::beg);
    in.read(&content[0], sz);
    if (!in.good()) return "";
    return content;
}

namespace detail {
namespace {

thread_local SaveWriteTestHook saveWriteHook;
std::atomic<unsigned long long> temporarySequence{0};

bool proceedWith(SaveWriteStage stage, const std::filesystem::path& temporary) {
    return !saveWriteHook.checkpoint ||
           saveWriteHook.checkpoint(stage, temporary, saveWriteHook.context);
}

unsigned long saveProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

class TemporarySaveFile {
public:
    TemporarySaveFile() = default;
    TemporarySaveFile(const TemporarySaveFile&) = delete;
    TemporarySaveFile& operator=(const TemporarySaveFile&) = delete;

    ~TemporarySaveFile() {
        close();
        if (m_owned) {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }
    }

    bool create(const std::filesystem::path& target) {
        // Exclusive creation, not the name alone, establishes ownership.
        constexpr int maxAttempts = 128;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            m_path = target.parent_path() /
                (".caesura-save-tmp-" + std::to_string(saveProcessId()) + "-" +
                 std::to_string(temporarySequence.fetch_add(1, std::memory_order_relaxed)));
            if (!proceedWith(SaveWriteStage::CreateTemporary, m_path)) return false;
#ifdef _WIN32
            m_handle = CreateFileW(m_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m_handle != INVALID_HANDLE_VALUE) {
                m_owned = true;
                return true;
            }
            const DWORD error = GetLastError();
            // CREATE_NEW reports ACCESS_DENIED for a directory collision.
            // It is still someone else's name, so retry without removing it.
            if (error == ERROR_ACCESS_DENIED) {
                const DWORD attributes = GetFileAttributesW(m_path.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            }
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
#else
            m_handle = ::open(m_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (m_handle >= 0) {
                m_owned = true;
                return true;
            }
            if (errno != EEXIST) return false;
#endif
        }
        return false;
    }

    bool write(const std::string& bytes) {
        if (!proceedWith(SaveWriteStage::Write, m_path)) return false;
        size_t offset = 0;
        while (offset < bytes.size()) {
            constexpr size_t writeChunkBytes = 64 * 1024;
            const size_t count = std::min(writeChunkBytes, bytes.size() - offset);
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(m_handle, bytes.data() + offset,
                           static_cast<DWORD>(count), &written, nullptr) ||
                written == 0) return false;
#else
            const auto written = ::write(m_handle, bytes.data() + offset, count);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
#endif
            offset += static_cast<size_t>(written);
            if (!proceedWith(SaveWriteStage::WriteProgress, m_path)) return false;
        }
        return true;
    }

    bool flush() {
        if (!proceedWith(SaveWriteStage::Flush, m_path)) return false;
#ifdef _WIN32
        return FlushFileBuffers(m_handle) != 0;
#else
        int result = 0;
        do { result = ::fsync(m_handle); } while (result < 0 && errno == EINTR);
        return result == 0;
#endif
    }

    bool closeForCommit() {
        return proceedWith(SaveWriteStage::Close, m_path) && close();
    }

    bool commit(const std::filesystem::path& target) {
        if (!proceedWith(SaveWriteStage::Replace, m_path)) return false;
#ifdef _WIN32
        // Do not use COPY_ALLOWED or a remove-then-rename fallback. Both paths
        // would discard the single filesystem publication point.
        const bool published = MoveFileExW(m_path.c_str(), target.c_str(),
                                            MOVEFILE_REPLACE_EXISTING |
                                            MOVEFILE_WRITE_THROUGH) != 0;
#else
        const bool published = ::rename(m_path.c_str(), target.c_str()) == 0;
#endif
        if (published) m_owned = false;
        return published;
    }

private:
    bool close() noexcept {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return true;
        const HANDLE handle = m_handle;
        m_handle = INVALID_HANDLE_VALUE;
        return CloseHandle(handle) != 0;
#else
        if (m_handle < 0) return true;
        const int handle = m_handle;
        m_handle = -1;
        // Retrying close after EINTR may close a reused descriptor.
        return ::close(handle) == 0;
#endif
    }

    std::filesystem::path m_path;
    bool m_owned = false;
#ifdef _WIN32
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_handle = -1;
#endif
};

} // namespace

ScopedSaveWriteTestHook::ScopedSaveWriteTestHook(SaveWriteTestHook hook) noexcept
    : m_previous(saveWriteHook) {
    saveWriteHook = hook;
}

ScopedSaveWriteTestHook::~ScopedSaveWriteTestHook() {
    saveWriteHook = m_previous;
}

bool writeSaveFileAtomically(const std::string& path, const std::string& bytes) {
    constexpr size_t maxSaveBytes = 10 * 1024 * 1024;
    if (bytes.size() > maxSaveBytes) return false;
    try {
        const std::filesystem::path target(path);
        TemporarySaveFile temporary;
        return temporary.create(target) && temporary.write(bytes) &&
               temporary.flush() && temporary.closeForCommit() &&
               temporary.commit(target);
    } catch (const std::exception&) {
        // All fallible work precedes publication; RAII cleans only our own
        // temporary. A failed write must never delete the destination.
        return false;
    }
}

CloudSnapshot readLocalCloudSnapshot(const std::string& rawPath) {
    constexpr size_t limit = 10u * 1024u * 1024u;
    CloudSnapshot snapshot;
    auto fail = [&](CloudReadState state, CloudReadError error) {
        snapshot.state = state;
        snapshot.error = error;
        snapshot.bytes.clear();
        return snapshot;
    };
    if (rawPath.empty() || rawPath.find('\0') != std::string::npos)
        return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
    try {
        namespace fs = std::filesystem;
        const auto input = fs::u8path(rawPath);
        // Do not normalize away an unchecked directory or link before '..'.
        for (const auto& part : input.relative_path())
            if (part.empty() || part == "." || part == "..")
                return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
        const auto path = fs::absolute(input);
        std::vector<fs::path> parts;
        for (const auto& part : path.relative_path()) parts.push_back(part);
        if (parts.empty()) return fail(CloudReadState::Invalid, CloudReadError::NotRegularFile);

#ifdef _WIN32
        struct Handles {
            std::vector<HANDLE> values;
            ~Handles() { for (const auto value : values) CloseHandle(value); }
        } handles;
        handles.values.reserve(parts.size() + 1);
        auto openError = [&](DWORD error, bool leaf) {
            if (leaf && error == ERROR_FILE_NOT_FOUND)
                return fail(CloudReadState::Missing, CloudReadError::None);
            if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
                return fail(CloudReadState::Failed, CloudReadError::ReadDenied);
            if (error == ERROR_PATH_NOT_FOUND || error == ERROR_DIRECTORY || error == ERROR_INVALID_NAME)
                return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
            return fail(CloudReadState::Failed, CloudReadError::Io);
        };
        fs::path current = path.root_path();
        auto openDirectory = [&](const fs::path& directory) -> bool {
            // Retain each ancestor without write/delete sharing, preventing
            // replacement or reparse-point mutation during the leaf read.
            const auto handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                snapshot = openError(GetLastError(), false); return false;
            }
            handles.values.push_back(handle);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle, &info)) {
                snapshot = fail(CloudReadState::Failed, CloudReadError::Io); return false;
            }
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snapshot = fail(CloudReadState::Invalid, CloudReadError::InvalidPath); return false;
            }
            return true;
        };
        if (!openDirectory(current)) return snapshot;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (parts[i].native().find(L':') != std::wstring::npos)
                return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
            current /= parts[i];
            if (!openDirectory(current)) return snapshot;
        }
        if (parts.back().native().find(L':') != std::wstring::npos)
            return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
        current /= parts.back();
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return fail(CloudReadState::Invalid, CloudReadError::NotRegularFile);
        const auto file = CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return openError(GetLastError(), true);
        handles.values.push_back(file);
        BY_HANDLE_FILE_INFORMATION before{}, after{};
        if (!GetFileInformationByHandle(file, &before))
            return fail(CloudReadState::Failed, CloudReadError::Io);
        if (GetFileType(file) != FILE_TYPE_DISK ||
            (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return fail(CloudReadState::Invalid, CloudReadError::NotRegularFile);
        const uint64_t size = (uint64_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
        if (size > limit) return fail(CloudReadState::Invalid, CloudReadError::TooLarge);
        snapshot.bytes.resize(static_cast<size_t>(size));
        while (snapshot.observedBytes < size) {
            DWORD count = 0;
            if (!ReadFile(file, snapshot.bytes.data() + static_cast<size_t>(snapshot.observedBytes),
                          static_cast<DWORD>(size - snapshot.observedBytes), &count, nullptr))
                return fail(CloudReadState::Failed, CloudReadError::Io);
            if (!count) return fail(CloudReadState::Failed, CloudReadError::Truncated);
            snapshot.observedBytes += count;
        }
        if (!GetFileInformationByHandle(file, &after))
            return fail(CloudReadState::Failed, CloudReadError::Io);
        if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
            before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow ||
            before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
            CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) != 0)
            return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
#else
        struct Handles {
            std::vector<int> values;
            ~Handles() { for (const int value : values) ::close(value); }
        } handles;
        handles.values.reserve(parts.size() + 1);
        auto openError = [&](int error, bool leaf) {
            if (leaf && error == ENOENT) return fail(CloudReadState::Missing, CloudReadError::None);
            if (error == EACCES || error == EPERM)
                return fail(CloudReadState::Failed, CloudReadError::ReadDenied);
            if (error == ELOOP || error == ENOTDIR || error == ENOENT)
                return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
            return fail(CloudReadState::Failed, CloudReadError::Io);
        };
        const int root = ::open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (root < 0) return openError(errno, false);
        handles.values.push_back(root);
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            const int next = ::openat(handles.values.back(), parts[i].c_str(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) return openError(errno, false);
            handles.values.push_back(next);
        }
        const int parent = handles.values.back();
        const int file = ::openat(parent, parts.back().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (file < 0) {
            const int error = errno;
            if (error == ENOENT) {
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    struct stat named{}, held{};
                    if (::fstatat(handles.values[i], parts[i].c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                        ::fstat(handles.values[i + 1], &held) != 0 || named.st_dev != held.st_dev ||
                        named.st_ino != held.st_ino || !S_ISDIR(named.st_mode))
                        return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
                }
            }
            return openError(error, true);
        }
        handles.values.push_back(file);
        struct stat before{}, after{};
        if (::fstat(file, &before) != 0) return fail(CloudReadState::Failed, CloudReadError::Io);
        if (!S_ISREG(before.st_mode)) return fail(CloudReadState::Invalid, CloudReadError::NotRegularFile);
        if (before.st_size < 0) return fail(CloudReadState::Failed, CloudReadError::Io);
        const auto size = static_cast<uint64_t>(before.st_size);
        if (size > limit) return fail(CloudReadState::Invalid, CloudReadError::TooLarge);
        snapshot.bytes.resize(static_cast<size_t>(size));
        while (snapshot.observedBytes < size) {
            const auto count = ::read(file, snapshot.bytes.data() + static_cast<size_t>(snapshot.observedBytes),
                                      static_cast<size_t>(size - snapshot.observedBytes));
            if (count < 0) {
                if (errno == EINTR) continue;
                return fail(CloudReadState::Failed, CloudReadError::Io);
            }
            if (!count) return fail(CloudReadState::Failed, CloudReadError::Truncated);
            snapshot.observedBytes += static_cast<uint64_t>(count);
        }
        if (::fstat(file, &after) != 0) return fail(CloudReadState::Failed, CloudReadError::Io);
        auto sameObject = [](const struct stat& a, const struct stat& b) {
            return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode;
        };
#ifdef __APPLE__
        const bool sameTimes = before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
            before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
            before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec && before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
        const bool sameTimes = before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
            before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
        if (!sameObject(before, after) || before.st_size != after.st_size || !sameTimes)
            return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
        // Directory FDs pin traversal but do not prevent rename on POSIX.
        // Re-observe every parent/name binding, including the actual leaf.
        for (size_t i = 0; i < parts.size(); ++i) {
            struct stat named{}, held{};
            if (::fstatat(handles.values[i], parts[i].c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
                ::fstat(handles.values[i + 1], &held) != 0 || !sameObject(named, held))
                return fail(CloudReadState::Failed, CloudReadError::ChangedDuringRead);
        }
#endif
        snapshot.state = CloudReadState::Present;
        return snapshot;
    } catch (const std::filesystem::filesystem_error&) {
        return fail(CloudReadState::Invalid, CloudReadError::InvalidPath);
    } catch (const std::exception&) {
        return fail(CloudReadState::Failed, CloudReadError::Io);
    }
}

} // namespace detail

bool LocalFileSaveProvider::writeFile(const std::string& path, const std::string& content) {
    return detail::writeSaveFileAtomically(path, content);
}

bool LocalFileSaveProvider::deleteFile(const std::string& path) {
    return std::remove(path.c_str()) == 0;
}

std::vector<std::string> LocalFileSaveProvider::listFiles(const std::string& pattern) {
    std::vector<std::string> result;
    std::string p = pattern;
    auto slash = p.find_last_of("/\\");
    std::string dirPath = (slash != std::string::npos) ? p.substr(0, slash) : ".";
    std::string glob = (slash != std::string::npos) ? p.substr(slash + 1) : pattern;
    bool matchAll = (glob == "*" || glob == "*.*");

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (!entry.is_regular_file()) continue;
            std::string fn = entry.path().filename().string();
            if (matchAll || fn == glob) {
                result.push_back(entry.path().string());
            }
        }
    } catch (const std::exception&) {
        // Directory may not exist — return empty
    }
    return result;
}

bool LocalFileSaveProvider::pushToCloud(const std::string&) {
    return false;
}

bool LocalFileSaveProvider::pullFromCloud(const std::string&) {
    return false;
}

} // namespace Caesura
