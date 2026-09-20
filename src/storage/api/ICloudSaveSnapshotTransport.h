#pragma once

#include <cstdint>
#include <string>

namespace Caesura {

enum class CloudSide { Local, Cloud };
enum class CloudReadState { Present, Missing, Unavailable, Failed, Invalid, Unsupported };
enum class CloudReadError {
    None, InvalidPath, NotRegularFile, ReadDenied, Io, InvalidEndpoint,
    UnsupportedTransport, TransportUnavailable, HttpStatus, Truncated, TooLarge,
    BackendUnavailable, IndeterminateMissing, IndeterminateSize,
    MalformedMetadata, ChangedDuringRead
};
enum class CloudRevisionKind { None, BackendOpaque };
enum class CloudConditionalWriteSupport { Unsupported, CompareAndReplace };

struct CloudSnapshot {
    CloudReadState state = CloudReadState::Unsupported;
    // Only Present exposes a complete payload. An empty Present is not Missing
    // and does not prove a valid save envelope; callers validate its policy.
    std::string bytes;
    CloudReadError error = CloudReadError::None;
    // Payload bytes accepted before completion/failure; excludes SDK metadata.
    // This diagnostic never makes a partial response usable as a snapshot.
    uint64_t observedBytes = 0;
    int httpStatus = 0; // Zero means no HTTP status was observed.
    CloudRevisionKind revisionKind = CloudRevisionKind::None;
    std::string revision;
};

// Optional capability, independent of the original directional-copy API.
// Synchronous owner-thread calls; do not race provider reconfiguration or
// destruction. Local is the explicit disk path even for a Steam provider.
// Reads never write either endpoint, retry, merge, or validate save envelopes.
// Current transports expose unversioned observations, not atomic two-side
// snapshots. Missing is authoritative only for the observed local/HTTP read;
// the existing Steam API cannot distinguish absence from unavailable storage.
class ICloudSaveSnapshotTransport {
public:
    virtual ~ICloudSaveSnapshotTransport() = default;
    virtual CloudSnapshot readSnapshot(CloudSide side, const std::string& slotPath) = 0;
    // A digest, timestamp, ETag or generation name alone does not establish
    // this capability. Unsupported must never fall back to an ordinary write.
    virtual CloudConditionalWriteSupport conditionalWriteSupport(CloudSide side) const = 0;
};

} // namespace Caesura
