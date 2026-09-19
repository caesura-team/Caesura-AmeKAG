// CloudSaveProvider — ISaveProvider backed by ISteamRemoteStorage
// Splits saves > 256 KiB into generations, while reading the legacy layout.
#pragma once
#include "api/ISaveProvider.h"
#include "api/ICloudSaveTransport.h"
#include <cstdint>  // fixed-width types (GCC strict)

namespace Caesura {
class ISteamBackend;

class CloudSaveProvider : public ISaveProvider, public ICloudSaveTransport {
public:
    explicit CloudSaveProvider(ISteamBackend* steam);
    ~CloudSaveProvider() override = default;

    // Paths are normalized to a FLAT cloud key (directory component stripped),
    // so "<saveDir>/save_5.json" and "save_5.json" address the SAME cloud
    // object whichever entry point is used.
    // Chunk writes stage and verify new bytes before one metadata publication
    // call. This relies on synchronous SDK rejection leaving the head intact;
    // it does not establish crash atomicity or concurrent-writer arbitration.
    std::string readFile(const std::string& path) override;
    bool writeFile(const std::string& path, const std::string& content) override;
    bool deleteFile(const std::string& path) override;
    std::vector<std::string> listFiles(const std::string& pattern) override;

    // Cloud sync overrides
    bool pushToCloud(const std::string& slotPath) override;
    bool pullFromCloud(const std::string& slotPath) override;
    bool supportsCloudSync() const override { return true; }
    std::string readLocalFile(const std::string& slotPath) override;
    bool writeLocalFile(const std::string& slotPath, const std::string& bytes) override;
    std::string readCloudFile(const std::string& slotPath) override;
    bool writeCloudFile(const std::string& slotPath, const std::string& bytes) override;

private:
    // Flat cloud key for a slot path (directory component stripped).
    static std::string cloudKey(const std::string& slotPath);

    ISteamBackend* m_steam;
};

} // namespace Caesura
