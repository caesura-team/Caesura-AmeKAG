// HttpCloudSaveProvider — ISaveProvider with a real remote (HTTP) end.
// Local file I/O is delegated to LocalFileSaveProvider (the slot files on
// disk stay the source of truth for offline play); cloud sync pushes/pulls
// individual slot files to a REST endpoint:
//   PUT    {endpoint}/{name}   body = slot file bytes
//   GET    {endpoint}/{name}   -> 200 + bytes | 404
//   DELETE {endpoint}/{name}
// Connection failures/timeouts degrade gracefully (false, local save
// untouched) -- cloud sync is an enhancement, never a blocker.
#pragma once
#include "api/ISaveProvider.h"
#include "api/ICloudSaveTransport.h"
#include "api/ICloudSaveSnapshotTransport.h"
#include <memory>
#include <string>

namespace Caesura {

class HttpCloudSaveProvider final : public ISaveProvider, public ICloudSaveTransport,
                                   public ICloudSaveSnapshotTransport {
public:
    // endpoint may be http:// or https://; bearerToken (optional) is sent as
    // an Authorization header on every cloud request (ST-2).
    HttpCloudSaveProvider(std::string endpoint, int timeoutMs = 8000,
                          std::string bearerToken = std::string());
    ~HttpCloudSaveProvider() override = default;

    std::string readFile(const std::string& path) override;
    bool writeFile(const std::string& path, const std::string& content) override;
    bool deleteFile(const std::string& path) override;
    std::vector<std::string> listFiles(const std::string& pattern) override;

    bool pushToCloud(const std::string& slotPath) override;
    bool pullFromCloud(const std::string& slotPath) override;
    bool supportsCloudSync() const override { return true; }
    std::string readLocalFile(const std::string& slotPath) override;
    bool writeLocalFile(const std::string& slotPath, const std::string& bytes) override;
    std::string readCloudFile(const std::string& slotPath) override;
    bool writeCloudFile(const std::string& slotPath, const std::string& bytes) override;

    CloudSnapshot readSnapshot(CloudSide side, const std::string& slotPath) override;
    CloudConditionalWriteSupport conditionalWriteSupport(CloudSide side) const override;

private:
    // Basename only: remote keys must never contain path separators.
    static std::string safeName(const std::string& slotPath);
    // One-shot HTTP round trip; returns response body or empty on failure.
    bool httpPut(const std::string& name, const std::string& body);
    std::string httpGet(const std::string& name);
    bool httpDelete(const std::string& name);

    std::string m_endpoint;
    int         m_timeoutMs;
    std::string m_bearerToken;  // optional Authorization: Bearer <token>
    std::unique_ptr<ISaveProvider> m_local;  // LocalFileSaveProvider
};

} // namespace Caesura
