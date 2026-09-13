// DirAssetProvider -- reads files from a filesystem directory
#pragma once
#include "api/IAssetProvider.h"
#include <cstdint>  // fixed-width types (GCC strict)
#include <filesystem>
#include <string>

namespace Caesura {

class DirAssetProvider : public IAssetProvider {
public:
    explicit DirAssetProvider(std::string rootDir)
        : m_rootDir(std::move(rootDir)) {}

    std::vector<uint8_t> read(const std::string& path) override;
    bool exists(const std::string& path) override;
    std::string getSource() const override { return "Dir:" + m_rootDir; }
    int priority() const override { return 5; }
    bool verify() override { return true; }

private:
    std::string m_rootDir;
    std::filesystem::path fullPath(const std::string& path) const;
};

} // namespace Caesura
