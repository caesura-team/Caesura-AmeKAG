// Storage-internal coordinator. The host owns the directory namespace and calls
// from one owner thread. No provider, encryption key or plaintext is retained.
#pragma once
#include "api/ICloudSaveCoordinator.h"
#include <filesystem>
#include <functional>
#include <thread>
#include <set>

namespace Caesura::detail {
struct CloudCoordinatorState {
    using Validate = std::function<bool(int, const std::string&)>;
    const std::thread::id owner = std::this_thread::get_id();
    bool active = false, bound = false;
    CloudCoordinatorStamp stamp;
    CloudCoordinatorBinding binding;
    std::filesystem::path contextRoot;
    std::string rootIdentity;
    std::set<std::string> revoked;

    // A busy mutation invalidates the operation but must leave the actual
    // provider/key/migration alive until its caller has unwound.
    bool allowMutation();
    void invalidate();
    CloudCoordinatorBindResult bind(const CloudCoordinatorBinding& next);
    CloudPrepareResult prepare(int slot, const std::string& token,
        ICloudSaveSnapshotTransport* transport, const std::string& slotPath,
        int schema, int policy, const Validate& validate);
    CloudPrepareResult reopen(const CloudPreparationRef& expected, const Validate& validate);
    CloudPreparationList list(int slot, const Validate& validate);
    CloudHistoryExportResult exportHistory(const CloudHistorySelection& selection,
        const std::string& token, const Validate& validate);
    CloudPublicationCheck check(const CloudHistorySelection& selection, CloudSide destination,
        ICloudSaveSnapshotTransport* transport,
        const std::function<std::string(int)>& slotPath, const Validate& validate);
};
} // namespace Caesura::detail
