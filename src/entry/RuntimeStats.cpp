#include "RuntimeStats.h"
#include "../di/BackendRegistry.h"

namespace Caesura {

RuntimeStatsSnapshot captureRuntimeStats(const IEngineHostSnapshot& host) {
    RuntimeStatsSnapshot result;
    auto& registry = BackendRegistry::instance();
    if (auto* jobs = registry.getJobSystem()) {
        result.jobs = jobs->getSnapshot();
    }
    if (auto* loader = registry.getAsyncLoader()) {
        result.asyncLoader = loader->getSnapshot();
    }
    result.host = host.getHostSnapshot();
    if (auto* audio = registry.getAudioBackend()) {
        result.audio = audio->getSnapshot();
    }
    return result;
}

} // namespace Caesura
