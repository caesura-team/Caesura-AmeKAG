// Optional explicit conflict preparation and historical recovery capability.
#pragma once
#include "ICloudSaveSnapshotTransport.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Caesura {

enum class CloudCoordinatorCode {
    Unsupported, Ready, Complete, Replayed, NotConfigured, UnsupportedSnapshot,
    InvalidInput, InvalidAncestor, ContextChanged, StaleContext, StaleObservation,
    InspectionIncomplete, InvalidSave, MissingVariant, CapacityExceeded,
    CryptoUnavailable, IoFailed, PreservationFailed, PreservationIndeterminate,
    RecordPreservedReceiptFailed, TokenMismatch, Indeterminate,
    UnsupportedConditionalWrite
};
enum class CloudComparison {
    NotCompared, EqualObserved, LocalChanged, CloudChanged, Conflict,
    DivergedWithoutBase, OneSideMissing, BothMissing, InspectionIncomplete,
    InvalidSave
};
enum class CloudSaveValidity { NotChecked, NotPresent, ValidCurrentPolicy, InvalidCurrentPolicy };
enum class CloudPreservation { None, Complete, Failed, Indeterminate };
enum class CloudAncestorSelection {
    NotRequested, Unchanged, Selected, NotSelected, Indeterminate,
    RecoveredSelected, RecoveredNotSelected, Superseded
};
enum class CloudPreservedVariant { Local, Cloud, Base };

struct CloudCoordinatorBinding {
    std::string root; // Existing ordinary absolute directory, controlled by host.
    std::string scopeId; // 32 lower hex; host asserts provider/account namespace.
    uint64_t policyEpoch = 0; // Explicit persistent validation namespace, not key hash.
};
struct CloudCoordinatorStamp {
    std::string sessionId; // Per-instance random 128 bits; never an account identity.
    uint64_t generation = 0;
};
struct CloudPreparationRef {
    std::string token; // 32 lower hex; caller operation identity.
    std::string receiptSha256; // External exact expected receipt identity.
};
struct CloudPreservedRecordRef {
    std::string id;
    std::string manifestSha256;
};
struct CloudObservedSide {
    CloudReadState state = CloudReadState::Unsupported;
    CloudReadError error = CloudReadError::None;
    CloudSaveValidity validity = CloudSaveValidity::NotChecked;
    uint64_t byteCount = 0;
    std::string sha256; // Complete Present only; not a backend revision.
};
struct CloudCoordinatorBindResult {
    CloudCoordinatorCode code = CloudCoordinatorCode::Unsupported;
    CloudCoordinatorStamp stamp;
};
struct CloudPrepareResult {
    CloudCoordinatorCode code = CloudCoordinatorCode::Unsupported;
    CloudComparison comparison = CloudComparison::NotCompared;
    CloudPreservation preservation = CloudPreservation::None;
    CloudAncestorSelection ancestor = CloudAncestorSelection::NotRequested;
    CloudCoordinatorStamp stamp;
    int slot = 0;
    CloudObservedSide local, cloud;
    std::optional<CloudPreparationRef> preparation;
    std::optional<CloudPreservedRecordRef> record;
    std::optional<CloudPreservedRecordRef> base;
    // Preserve exact B1 candidate even when record cannot be claimed Complete.
    std::optional<CloudPreservedRecordRef> candidateRecord;
    // Empty means no candidate selection. SHA cannot be interpreted as CAS.
    std::string candidateCursorSha256;
};
struct CloudPreparationList {
    CloudCoordinatorCode code = CloudCoordinatorCode::Unsupported;
    std::vector<CloudPreparationRef> preparations;
    uint64_t incompleteOperations = 0;
    uint64_t invalidOperations = 0;
};
struct CloudHistorySelection {
    CloudPreparationRef preparation;
    CloudPreservedVariant variant = CloudPreservedVariant::Local;
    CloudCoordinatorStamp stamp; // Obtained from latest prepare/reopen in this session.
};
struct CloudHistoryExportResult {
    CloudCoordinatorCode code = CloudCoordinatorCode::Unsupported;
    std::string token;
    std::string path; // Only complete/replayed historical copy; never a live slot.
    uint64_t byteCount = 0;
    std::string sha256;
    std::string receiptSha256;
};
struct CloudPublicationCheck {
    CloudCoordinatorCode code = CloudCoordinatorCode::Unsupported;
    CloudObservedSide currentLocal, currentCloud;
    // No 'Applied', writable lease, or unconditional transfer ticket exists.
};

class ICloudSaveCoordinator {
public:
    virtual ~ICloudSaveCoordinator() = default;
    virtual CloudCoordinatorBindResult bindCloudCoordinator(const CloudCoordinatorBinding& binding) = 0;
    virtual CloudPrepareResult prepareCloudSync(int slot, const std::string& operationToken) = 0;
    virtual CloudPrepareResult reopenCloudPreparation(const CloudPreparationRef& expected) = 0;
    virtual CloudPreparationList listCloudPreparations(int slot) = 0;
    virtual CloudHistoryExportResult exportCloudHistory(const CloudHistorySelection& selection,
                                                       const std::string& exportToken) = 0;
    virtual CloudPublicationCheck checkCloudPublication(const CloudHistorySelection& selection,
                                                        CloudSide destination) = 0;
};

} // namespace Caesura
