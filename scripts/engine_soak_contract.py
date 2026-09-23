"""Strict quiet-point checks for the native U27 workload; no backend execution."""

UINT64_MAX = 2**64 - 1
GROWTH_BUDGETS = {"memory.luaBytes": 1024**2, "memory.privateBytes": 64 * 1024**2}
FLAGS = {
    "jobs.supported": True, "jobs.running": True,
    "async.supported": True, "async.running": True,
    "host.supported": True, "host.initialized": True, "host.running": True,
    "host.luaPaused": False, "host.asyncOwnershipComplete": True,
    "host.audioCompletionTrackingSupported": True,
    "audio.supported": True, "audio.running": True,
    "render.supported": True, "render.contextInitialized": True,
    "render.renderingAvailable": True, "render.resourceCountsAvailable": True,
    "render.screenshotOwnershipComplete": True,
    "render.screenshotReadbackTrackingSupported": True,
    "render.screenshots.supported": True,
}
MODES = {"host.delivery": "DirectDrain", "audio.outputMode": "Device",
         "render.backendKind": "GraphicsApi", "render.backendName": "Direct3D11"}
DEBTS = (
    "jobs.workerPending", "jobs.queuedCompletions", "jobs.dispatchingCompletions",
    "async.pendingWaiters", "async.inflightKeys", "async.completedBuffered",
    "host.deferredAsyncPayloads", "host.drainingAsyncPayloads", "host.dispatchingAsyncPayloads",
    "host.audioCompletionsPending", "host.audioCompletionsActive", "host.audioCompletionOwnerRefs",
    "audio.liveVoices", "audio.sessionHandles", "audio.retiringBGM", "audio.retiringVoice",
    "audio.rawCacheEntries", "audio.voiceCompletionsPending", "audio.restoredSources",
    "render.screenshotReadbacksOutstanding", "render.screenshots.waiting", "render.screenshots.submitted",
    "render.screenshots.terminal", "render.screenshots.reservedBytes", "render.screenshots.pngBytes",
)
RESOURCE_FIELDS = ("dynamicIndexBuffers", "dynamicVertexBuffers", "frameBuffers", "indexBuffers",
                   "occlusionQueries", "programs", "shaders", "textures", "uniforms", "vertexBuffers", "vertexLayouts")
EXACT = tuple("render.resources." + name for name in RESOURCE_FIELDS) + ("audio.busVoices", "memory.textureBytes")
CACHES = ("async.cacheEntries", "async.cacheBytes", "audio.waveCacheEntries")
COUNTS = DEBTS + EXACT + CACHES + tuple(GROWTH_BUDGETS) + (
    "host.completedOwnerFrames", "render.contextGeneration", "render.captureSubmissionFrame",
    "memory.rssBytes", "memory.osHandles",
)
_MISSING = object()


def _get(document, path):
    value = document
    for field in path.split("."):
        if not isinstance(value, dict) or field not in value:
            return _MISSING
        value = value[field]
    return value


def check_quiet(sample, baseline):
    """Check one observed boundary against a frozen, valid warmed boundary.

    This checks component ownership and fixed budgets, not global atomic idle,
    GPU fence completion, physical audio, elapsed duration or workload coverage.
    The runner must independently verify those parts of its execution contract.
    Neither input is changed. RSS and OS handles are recorded unsigned counters;
    RSS is not substituted for allocated private bytes.
    """
    errors = []
    numeric = {}
    for role, document in (("sample", sample), ("baseline", baseline)):
        values = {}
        numeric[role] = values
        for path, expected in FLAGS.items():
            value = _get(document, path)
            if type(value) is not bool or value is not expected:
                errors.append(f"{role}.{path}: required boolean {expected}")
        for path, expected in MODES.items():
            value = _get(document, path)
            if type(value) is not str or value != expected:
                errors.append(f"{role}.{path}: required {expected}")
        for path in COUNTS:
            value = _get(document, path)
            # bool is an int subclass in Python; accepting it would turn a
            # support flag accidentally copied into a count into valid data.
            if type(value) is not int or not 0 <= value <= UINT64_MAX:
                errors.append(f"{role}.{path}: missing or invalid uint64 observation")
            else:
                values[path] = value
        for path in DEBTS:
            if path in values and values[path] != 0:
                errors.append(f"{role}.{path}: outstanding ownership {values[path]}")
        if values.get("render.contextGeneration") == 0:
            errors.append(f"{role}.render.contextGeneration: no initialized context identity")

    current, warmed = numeric["sample"], numeric["baseline"]
    for path in EXACT + ("render.contextGeneration",):
        if path in current and path in warmed and current[path] != warmed[path]:
            errors.append(f"sample.{path}: {current[path]} differs from frozen {warmed[path]}")
    for path in CACHES:
        if path in current and path in warmed and current[path] > warmed[path]:
            errors.append(f"sample.{path}: exceeds warmed finite fixture bound")
    for path, allowance in GROWTH_BUDGETS.items():
        if path in current and path in warmed and current[path] > warmed[path] + allowance:
            errors.append(f"sample.{path}: exceeds frozen growth budget {allowance}")
    return errors
