// CarcAssetProvider implementation
#include "CarcAssetProvider.h"

namespace Caesura::carc {

CarcAssetProvider::CarcAssetProvider(std::unique_ptr<CARCReader> reader,
                                     int priority,
                                     std::string sourceName)
    : m_reader(std::move(reader))
    , m_priority(priority)
    , m_sourceName(std::move(sourceName))
{}

::Caesura::AssetDirectoryResult CarcAssetProvider::listDirectory(const std::string&, size_t, size_t) {
    // Current CARC indexes retain only path hashes, not recoverable names.
    // Neither guessing names nor returning hashes is an enumeration capability.
    return {::Caesura::AssetDirectoryStatus::Unsupported, {}};
}

std::vector<uint8_t> CarcAssetProvider::read(const std::string& path)
{
    if (!m_reader || !m_reader->isOpen()) return {};
    return m_reader->readFile(path);
}

bool CarcAssetProvider::exists(const std::string& path)
{
    if (!m_reader || !m_reader->isOpen()) return false;
    return m_reader->hasFile(path);
}

} // namespace Caesura::carc
// verify() is defined inline in CarcAssetProvider.h
// (returns m_reader->verifySignature())