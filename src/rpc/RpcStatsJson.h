#pragma once

#include "api/IRpcDispatcher.h"
#include <nlohmann_json.hpp>

namespace Caesura::rpc_detail {

inline const char* asyncDeliveryName(RpcAsyncDelivery value) {
    switch (value) {
    case RpcAsyncDelivery::DirectDrain: return "direct_drain";
    case RpcAsyncDelivery::SdlEvents: return "sdl_events";
    case RpcAsyncDelivery::Unknown: return "unknown";
    }
    return "unknown";
}

inline const char* audioOutputName(RpcAudioOutput value) {
    switch (value) {
    case RpcAudioOutput::Device: return "device";
    case RpcAudioOutput::ManualMix: return "manual_mix";
    case RpcAudioOutput::Software: return "software";
    case RpcAudioOutput::Unknown: return "unknown";
    }
    return "unknown";
}

// Serialize copied DTOs only. Keep bool and uint64 types intact; neither
// transport performs backend reads or infers support/global idle from zeroes.
inline nlohmann::json statsJson(const RpcStatsResult& s) {
    return {
        {"texture_budget_mb", s.textureBudgetMB},
        {"texture_tier", s.textureTier},
        {"texture_tier_name", s.textureTierName},
        {"mesh_count", s.meshCount},
        {"job_workers", s.jobWorkers},
        {"job_pending", s.jobPending},
        {"lua_kb", s.luaKb},
        {"jobs", {
            {"supported", s.jobs.supported},
            {"running", s.jobs.running},
            {"worker_pending", s.jobs.workerPending},
            {"queued_completions", s.jobs.queuedCompletions},
            {"dispatching_completions", s.jobs.dispatchingCompletions},
        }},
        {"async_loader", {
            {"supported", s.asyncLoader.supported},
            {"running", s.asyncLoader.running},
            {"pending_waiters", s.asyncLoader.pendingWaiters},
            {"inflight_keys", s.asyncLoader.inflightKeys},
            {"completed_buffered", s.asyncLoader.completedBuffered},
            {"cache_entries", s.asyncLoader.cacheEntries},
            {"cache_bytes", s.asyncLoader.cacheBytes},
        }},
        {"host", {
            {"supported", s.host.supported},
            {"initialized", s.host.initialized},
            {"running", s.host.running},
            {"lua_paused", s.host.luaPaused},
            {"delivery", asyncDeliveryName(s.host.delivery)},
            {"async_ownership_complete", s.host.asyncOwnershipComplete},
            {"completed_owner_frames", s.host.completedOwnerFrames},
            {"deferred_async_payloads", s.host.deferredAsyncPayloads},
            {"draining_async_payloads", s.host.drainingAsyncPayloads},
            {"dispatching_async_payloads", s.host.dispatchingAsyncPayloads},
            {"audio_completion_tracking_supported", s.host.audioCompletionTrackingSupported},
            {"audio_completions_pending", s.host.audioCompletionsPending},
            {"audio_completions_active", s.host.audioCompletionsActive},
            {"audio_completion_owner_refs", s.host.audioCompletionOwnerRefs},
        }},
        {"audio", {
            {"supported", s.audio.supported},
            {"running", s.audio.running},
            {"output_mode", audioOutputName(s.audio.outputMode)},
            {"live_voices", s.audio.liveVoices},
            {"bus_voices", s.audio.busVoices},
            {"session_handles", s.audio.sessionHandles},
            {"retiring_bgm", s.audio.retiringBGM},
            {"retiring_voice", s.audio.retiringVoice},
            {"wave_cache_entries", s.audio.waveCacheEntries},
            {"raw_cache_entries", s.audio.rawCacheEntries},
            {"voice_completions_pending", s.audio.voiceCompletionsPending},
            {"restored_sources", s.audio.restoredSources},
        }},
    };
}

} // namespace Caesura::rpc_detail
