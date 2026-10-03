// CarcAssetProvider -- wraps CARCReader as an IAssetProvider
#pragma once
#include "resource/api/IAssetProvider.h"
#include <cstdint>  // fixed-width types (GCC strict)
#include <string>
#include "CARCReader.h"
#include <memory>

namespace Caesura::carc {

class CarcAssetProvider : public ::Caesura::IAssetProvider {
public:
    ::Caesura::AssetDirectoryResult listDirectory(const std::string&, size_t, size_t) override;
    explicit CarcAssetProvider(std::unique_ptr<CARCReader> reader,
                               int priority = 10,
                               std::string sourceName = "CARC");

    std::vector<uint8_t> read(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::string getSource() const override { return m_sourceName; }
    int priority() const override { return m_priority; }
    bool verify() override {
        if (!m_reader || !m_reader->isOpen()) return false;
        return m_reader->verifySignature();
    }

private:
    std::unique_ptr<CARCReader> m_reader;
    int m_priority = 10;
    std::string m_sourceName = "CARC";
};

} // namespace Caesura::carc
