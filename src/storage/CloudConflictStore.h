// Local immutable observations and fork preservation (storage internal).
// Internal to storage; no new BackendRegistry backend or public module API.
#pragma once
#include "api/ICloudSaveSnapshotTransport.h"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Caesura::detail {

struct ConflictStoreContext {
    std::string scopeId; // 32 lower hex; caller-owned namespace, not account proof
    int slot = 0;        // -2..99, matching SaveManager
    uint64_t policyEpoch = 0; // nonzero; no key/secret is stored
};

struct ConflictRecordRef {
    std::string id;             // store-generated 128-bit random, 32 lower hex
    std::string manifestSha256; // external expected raw manifest identity
};

enum class ConflictRecordKind {
    EqualObserved, LocalChanged, CloudChanged, Conflict, DivergedWithoutBase
};

enum class ConflictStoreCode {
    Unsupported, // safe RED scaffold default; not evidence of implementation
    Preserved, Complete,
    InspectionIncomplete, InvalidInput, CapacityExceeded, CryptoUnavailable,
    PublicationFailed, Indeterminate,
    Missing, Incomplete, InvalidRecord, IoFailed
};

struct PreservedConflictRecord {
    ConflictRecordRef ref;
    ConflictStoreContext context;
    ConflictRecordKind kind = ConflictRecordKind::EqualObserved;
    std::optional<ConflictRecordRef> baseRef;
    std::optional<std::string> baseBytes;
    std::string localBytes;
    std::string cloudBytes;
    // No field can claim save-envelope validation or conditional-write support.
};

struct ConflictStoreResult {
    ConflictStoreCode code = ConflictStoreCode::Unsupported;
    // Retained even for PublicationFailed/Indeterminate once the attempt exists.
    // It is not a usable record reference without validated manifest SHA.
    std::string operationId;
    // Set once exact final manifest bytes are prepared, before publication.
    // Indeterminate MUST retain this expected identity for a later readRecord;
    // callers must not discover a new hash from a possibly changed manifest.
    std::optional<ConflictRecordRef> candidateRef;
    std::optional<PreservedConflictRecord> record;
};

struct ConflictStoreListResult {
    ConflictStoreCode code = ConflictStoreCode::Unsupported;
    std::vector<ConflictRecordRef> completeRecords;
    uint64_t incompleteRecords = 0;
    uint64_t invalidRecords = 0;
};

struct ConflictStoreLimits {
    // Valid ranges: 1..128 entries and 1..256 MiB of logical file bytes.
    // Incomplete/unknown entries count too; no implicit eviction.
    uint64_t maxRecords = 128;
    uint64_t maxStoredBytes = 256u * 1024u * 1024u;
};

class CloudConflictStore {
public:
    // Root must be a caller-controlled existing ordinary absolute directory.
    // Owner thread only; one active writer owns this directory namespace.
    explicit CloudConflictStore(std::filesystem::path root,
                                ConflictStoreLimits limits = {});
    ~CloudConflictStore();
    CloudConflictStore(const CloudConflictStore&) = delete;
    CloudConflictStore& operator=(const CloudConflictStore&) = delete;

    // Observations remain opaque: this does not validate a save envelope, select
    // a current ancestor, or write either transport endpoint. A failed attempt
    // is retained; after Indeterminate, reopen the returned candidateRef first.
    ConflictStoreResult preserve(const ConflictStoreContext& context,
                                 const CloudSnapshot& local,
                                 const CloudSnapshot& cloud,
                                 const std::optional<ConflictRecordRef>& expectedBase = std::nullopt);
    ConflictStoreResult readRecord(const ConflictStoreContext& context,
                                   const ConflictRecordRef& expectedRecord) const;
    ConflictStoreListResult listRecords(const ConflictStoreContext& context) const;

private:
    std::filesystem::path m_root;
    ConflictStoreLimits m_limits;
    const std::thread::id m_ownerThread;
    mutable bool m_active = false; // owner-thread reentry guard, not a mutex
};

} // namespace Caesura::detail
