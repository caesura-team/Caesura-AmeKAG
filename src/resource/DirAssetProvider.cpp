#include "DirAssetProvider.h"
#include "AssetDirectoryValidation.h"
#include <algorithm>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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

AssetDirectoryResult DirAssetProvider::listDirectory(const std::string& directory,size_t maxEntries,size_t maxNameBytes) {
    using Status=AssetDirectoryStatus;
    std::string relative;
    if(!AssetDirectoryDetail::limits(maxEntries,maxNameBytes))return {Status::LimitExceeded,{}};
    if(!AssetDirectoryDetail::normalize(directory,relative))return {Status::InvalidPath,{}};
    try {
        const fs::path root=fs::absolute(m_rootDir.empty()?fs::current_path():fs::u8path(m_rootDir)).lexically_normal();
        const fs::path requested=root/fs::u8path(relative);
        std::vector<std::string> names;size_t bytes=0,visited=0;
        const auto append=[&](std::string name)->Status {
            if(!AssetDirectoryDetail::leaf(name))return Status::InvalidPath;
            if(names.size()>=maxEntries||name.size()>maxNameBytes-bytes)return Status::LimitExceeded;
            bytes+=name.size();names.push_back(std::move(name));return Status::Complete;
        };
#ifdef _WIN32
        struct Handle {
            HANDLE value=INVALID_HANDLE_VALUE;
            explicit Handle(HANDLE h):value(h){}
            Handle(Handle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
            Handle(const Handle&)=delete;
            ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        };
        std::vector<Handle> ancestors;
        const auto retain=[&](const fs::path& path)->Status {
            // FILE_LIST_DIRECTORY (not attributes alone), with no SHARE_DELETE,
            // keeps every directory component bound while the path is listed.
            Handle handle(CreateFileW(path.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(handle.value==INVALID_HANDLE_VALUE) {
                const auto error=GetLastError();
                if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return Status::Complete;
                return Status::IoError;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(handle.value,&info))return Status::IoError;
            if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
                ||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return Status::InvalidPath;
            ancestors.push_back(std::move(handle));return Status::Complete;
        };
        fs::path cursor=requested.root_path();
        auto status=retain(cursor);if(status!=Status::Complete)return {status,{}};
        if(ancestors.empty())return {Status::Complete,{}};
        for(const auto& component:requested.relative_path()) {
            cursor/=component;const auto count=ancestors.size();status=retain(cursor);
            if(status!=Status::Complete)return {status,{}};
            if(count==ancestors.size())return {Status::Complete,{}}; // Provider prefix is absent.
        }
        WIN32_FIND_DATAW entry{};
        HANDLE search=FindFirstFileW((requested/L"*").c_str(),&entry);
        if(search==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();
            return {error==ERROR_FILE_NOT_FOUND?Status::Complete:Status::IoError,{}};
        }
        struct Search {HANDLE value;~Search(){FindClose(value);}} searchOwner{search};
        do {
            const std::wstring leafName(entry.cFileName);
            if(leafName==L"."||leafName==L"..")continue;
            if(++visited>maxEntries)return {Status::LimitExceeded,{}};
            if(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE))continue;
            const auto encoded=fs::path(leafName).u8string();
            status=append(std::string(reinterpret_cast<const char*>(encoded.data()),encoded.size()));
            if(status!=Status::Complete)return {status,{}};
        }while(FindNextFileW(search,&entry));
        if(GetLastError()!=ERROR_NO_MORE_FILES)return {Status::IoError,{}};
#else
        struct Fd {
            int value=-1;
            explicit Fd(int fd):value(fd){}
            Fd(const Fd&)=delete;
            Fd& operator=(Fd&& other) noexcept {if(value>=0)::close(value);value=std::exchange(other.value,-1);return *this;}
            ~Fd(){if(value>=0)::close(value);}
        };
        Fd current(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
        if(current.value<0)return {Status::IoError,{}};
        for(const auto& component:requested.relative_path()) {
            Fd next(::openat(current.value,component.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
            if(next.value<0) {
                if(errno==ENOENT)return {Status::Complete,{}};
                return {errno==ELOOP||errno==ENOTDIR?Status::InvalidPath:Status::IoError,{}};
            }
            current=std::move(next);
        }
        Fd duplicate(::dup(current.value));if(duplicate.value<0)return {Status::IoError,{}};
        DIR* directoryStream=::fdopendir(duplicate.value);
        if(!directoryStream)return {Status::IoError,{}};
        duplicate.value=-1;
        struct Stream {DIR* value;~Stream(){::closedir(value);}} owner{directoryStream};
        while(true) {
            errno=0;auto* entry=::readdir(directoryStream);
            if(!entry){if(errno)return {Status::IoError,{}};break;}
            const std::string name(entry->d_name);
            if(name=="."||name=="..")continue;
            if(++visited>maxEntries)return {Status::LimitExceeded,{}};
            struct stat info{};
            if(::fstatat(current.value,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0)return {Status::IoError,{}};
            if(!S_ISREG(info.st_mode))continue;
            const auto status=append(name);if(status!=Status::Complete)return {status,{}};
        }
#endif
        std::sort(names.begin(),names.end());
        return {Status::Complete,std::move(names)};
    }catch(...){return {Status::IoError,{}};}
}

} // namespace Caesura
