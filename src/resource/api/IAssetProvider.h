// IAssetProvider -- abstract interface for asset sources
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace Caesura {

constexpr size_t kMaxAssetDirectoryEntries = 4096;
constexpr size_t kMaxAssetDirectoryNameBytes = 1024 * 1024;
enum class AssetDirectoryStatus { Complete, Unsupported, InvalidPath, LimitExceeded, IoError };
struct AssetDirectoryResult {
    AssetDirectoryStatus status = AssetDirectoryStatus::Unsupported;
    std::vector<std::string> files; // UTF-8 leaf names only; populated only on Complete.
};

class IAssetProvider {
public:
    virtual ~IAssetProvider() = default;
    // Nonrecursive listing. Unsupported/error/limit results are never complete
    // empty listings; the caller must not silently fall through to other I/O.
    virtual AssetDirectoryResult listDirectory(const std::string& directory,
        size_t maxEntries, size_t maxNameBytes) = 0;

    // Read a file. Returns empty vector if not found or on error.
    virtual std::vector<uint8_t> read(const std::string& path) = 0;

    // Check if a file exists in this provider.
    virtual bool exists(const std::string& path) = 0;

    // Human-readable source name for debugging.
    virtual std::string getSource() const = 0;

    // Priority: higher = checked first. CARC=10, Dir=5, Patch=8.
    virtual int priority() const = 0;

    // Verify integrity of the asset source. Returns true if valid.
    // CARC providers verify Ed25519 signature; others return true.
    virtual bool verify() = 0;
};

} // namespace Caesura