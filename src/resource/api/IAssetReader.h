#pragma once
#include <cstddef>
#include "IAssetProvider.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Caesura {

// Immutable input for restore preparation. The highest-priority matching
// source owns the result: read errors cannot fall through to different bytes.
class IAssetReader {
public:
    virtual ~IAssetReader() = default;
    virtual AssetDirectoryResult listDirectory(const std::string& directory,
        size_t maxEntries, size_t maxNameBytes) = 0;
    // Empty means unavailable, failed or over limit. The limit bounds returned
    // bytes; each provider retains its own transient-read allocation ceiling.
    virtual std::vector<uint8_t> readAsset(const std::string& path,
                                          size_t maxBytes) = 0;
};

} // namespace Caesura
