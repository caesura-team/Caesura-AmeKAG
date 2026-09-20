#pragma once

#include "api/IEngineHostSnapshot.h"
#include "../job/api/IJobSystem.h"
#include "../resource/api/IAsyncLoader.h"
#include "../audio/api/IAudioBackend.h"

namespace Caesura {

// Composition-root value copy, not an RPC DTO or an atomic idle observation.
// Keep native API types so entry does not acquire an RPC dependency.
struct RuntimeStatsSnapshot {
    JobSystemSnapshot jobs;
    AsyncLoaderSnapshot asyncLoader;
    EngineHostSnapshot host;
    AudioBackendSnapshot audio;
};

// Owner/main thread only, while the registered backend owners remain alive.
// Read each available observer exactly once; do not pump, drain, mix or cull.
// Missing backends retain unsupported defaults. Return values own no backend
// pointer and remain independent of later registry/session changes.
RuntimeStatsSnapshot captureRuntimeStats(const IEngineHostSnapshot& host);

} // namespace Caesura
