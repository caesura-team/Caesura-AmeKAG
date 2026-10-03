#pragma once

#include <cstdint>

namespace Caesura {

enum class AsyncHostDelivery { DirectDrain, SdlEvents };

struct EngineHostSnapshot {
    bool supported = false;
    bool initialized = false;
    bool running = false;
    bool luaPaused = false;
    AsyncHostDelivery delivery = AsyncHostDelivery::SdlEvents;
    bool asyncOwnershipComplete = false;
    uint64_t completedOwnerFrames = 0;
    uint64_t deferredAsyncPayloads = 0;
    uint64_t drainingAsyncPayloads = 0;
    uint64_t dispatchingAsyncPayloads = 0;
    // Host-owned natural voice notifications, independently supported from
    // the audio backend and SDL async ownership observations. Pending includes
    // stale batches until disposal. Active covers returned-but-unadopted
    // notifications and the current callback through its stack cleanup.
    // Transfer phases can overlap; never sum these as resource totals.
    bool audioCompletionTrackingSupported = false;
    uint64_t audioCompletionsPending = 0;
    uint64_t audioCompletionsActive = 0;
    // Zero or one retained Lua registry owner pin, not a notification or ID.
    uint64_t audioCompletionOwnerRefs = 0;
};

class IEngineHostSnapshot {
public:
    virtual ~IEngineHostSnapshot() = default;

    // Owner/main thread only, including from a completion callback. Supported
    // values describe the live Engine even before init or during/after shutdown.
    // Deferred includes stale payloads not yet disposed. Draining includes the
    // current item and unfinished local batches; dispatching is the active
    // dispatch stack. These phases overlap: never sum them as resource totals.
    // Cancellation/shutdown cannot hide stack-owned work before it unwinds.
    // Ownership is complete only for the standard Engine direct-drain route
    // (headless/editor), combined with supported Job/Async observations. SDL
    // published events and externally taken results are not counted here;
    // ordinary SDL mode must report incomplete ownership, never measured zero.
    // completedOwnerFrames counts completed main-loop iterations, including
    // Null/headless loops, independent of --frames. It excludes standalone
    // render/capture calls, failed/recovery iterations and shutdown flushes.
    // It is not a GPU completion count and survives shutdown of this instance.
    // This query does not pump, drain, render, run Lua, or prove global idle.
    virtual EngineHostSnapshot getHostSnapshot() const = 0;
};

} // namespace Caesura
