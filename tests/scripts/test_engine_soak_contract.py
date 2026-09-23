"""Reject false quiet-point evidence; these fixtures are not native soak proof."""
import copy
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from engine_soak_contract import check_quiet


def measured():
    return {
        "jobs": {"supported": True, "running": True, "workerPending": 0,
                 "queuedCompletions": 0, "dispatchingCompletions": 0},
        "async": {"supported": True, "running": True, "pendingWaiters": 0,
                  "inflightKeys": 0, "completedBuffered": 0, "cacheEntries": 2, "cacheBytes": 2048},
        "host": {"supported": True, "initialized": True, "running": True,
                 "luaPaused": False, "delivery": "DirectDrain", "asyncOwnershipComplete": True,
                 "audioCompletionTrackingSupported": True, "deferredAsyncPayloads": 0,
                 "drainingAsyncPayloads": 0, "dispatchingAsyncPayloads": 0,
                 "audioCompletionsPending": 0, "audioCompletionsActive": 0, "audioCompletionOwnerRefs": 0,
                 "completedOwnerFrames": 1000},
        "audio": {"supported": True, "running": True, "outputMode": "Device",
                  "liveVoices": 0, "busVoices": 3, "sessionHandles": 0,
                  "retiringBGM": 0, "retiringVoice": 0, "waveCacheEntries": 2,
                  "rawCacheEntries": 0, "voiceCompletionsPending": 0, "restoredSources": 0},
        "render": {"supported": True, "contextInitialized": True, "renderingAvailable": True,
                   "resourceCountsAvailable": True, "backendKind": "GraphicsApi", "backendName": "Direct3D11",
                   "contextGeneration": 7, "captureSubmissionFrame": 2000,
                   "screenshotOwnershipComplete": True, "screenshotReadbackTrackingSupported": True,
                   "screenshotReadbacksOutstanding": 0,
                   "resources": {"dynamicIndexBuffers": 1, "dynamicVertexBuffers": 1, "frameBuffers": 2,
                                 "indexBuffers": 1, "occlusionQueries": 0, "programs": 14, "shaders": 28,
                                 "textures": 7, "uniforms": 12, "vertexBuffers": 1, "vertexLayouts": 3},
                   "screenshots": {"supported": True, "waiting": 0, "submitted": 0, "terminal": 0,
                                   "reservedBytes": 0, "pngBytes": 0}},
        "memory": {"textureBytes": 4096, "luaBytes": 1000000, "privateBytes": 200000000,
                   "rssBytes": 180000000, "osHandles": 120},
    }


class QuietPointContractTests(unittest.TestCase):
    def setUp(self):
        self.baseline = measured()
        self.sample = copy.deepcopy(self.baseline)

    def rejected(self, reason):
        errors = check_quiet(self.sample, self.baseline)
        self.assertTrue(errors, reason)
        self.assertTrue(all(isinstance(e, str) and e for e in errors))

    def test_supported_real_quiet_observation_passes_without_mutation(self):
        original = copy.deepcopy((self.sample, self.baseline))
        self.assertEqual(check_quiet(self.sample, self.baseline), [])
        self.assertEqual((self.sample, self.baseline), original)

    def test_unsupported_zeroes_are_not_measured_idle(self):
        for group in ("jobs", "async", "host", "audio", "render"):
            with self.subTest(group=group):
                self.sample = measured(); self.sample[group]["supported"] = False
                self.rejected(group)

    def test_incomplete_or_paused_owner_cannot_pass(self):
        for group, field in (("jobs", "running"), ("async", "running"), ("host", "running"),
            ("host", "initialized"), ("host", "asyncOwnershipComplete"),
            ("host", "audioCompletionTrackingSupported"), ("audio", "running"),
            ("render", "contextInitialized"), ("render", "renderingAvailable"),
            ("render", "resourceCountsAvailable"), ("render", "screenshotOwnershipComplete"),
            ("render", "screenshotReadbackTrackingSupported")):
            with self.subTest(group=group, field=field):
                self.sample = measured(); self.sample[group][field] = False
                self.rejected(field)
        self.sample = measured(); self.sample["host"]["luaPaused"] = True
        self.rejected("paused host")

    def test_placeholder_and_unknown_backends_cannot_pass(self):
        for group, field, values in (("host", "delivery", ("SdlEvents", "Unknown")),
            ("audio", "outputMode", ("Unknown", "ManualMix", "Software")),
            ("render", "backendKind", ("Unknown", "Noop")),
            ("render", "backendName", ("Noop", "OpenGL", "Unknown"))):
            for value in values:
                with self.subTest(field=field, value=value):
                    self.sample = measured(); self.sample[group][field] = value
                    self.rejected(field)

    def test_every_outstanding_ownership_stage_is_rejected(self):
        stages = {
            "jobs": ("workerPending", "queuedCompletions", "dispatchingCompletions"),
            "async": ("pendingWaiters", "inflightKeys", "completedBuffered"),
            "host": ("deferredAsyncPayloads", "drainingAsyncPayloads", "dispatchingAsyncPayloads",
                     "audioCompletionsPending", "audioCompletionsActive", "audioCompletionOwnerRefs"),
            "audio": ("liveVoices", "sessionHandles", "retiringBGM", "retiringVoice",
                      "rawCacheEntries", "voiceCompletionsPending", "restoredSources"),
            "render": ("screenshotReadbacksOutstanding",),
        }
        for group, fields in stages.items():
            for field in fields:
                with self.subTest(group=group, field=field):
                    self.sample = measured(); self.sample[group][field] = 1
                    self.rejected(field)

    def test_ticket_retention_and_native_readback_are_separate_obligations(self):
        for field in ("waiting", "submitted", "terminal", "reservedBytes", "pngBytes"):
            with self.subTest(field=field):
                self.sample = measured(); self.sample["render"]["screenshots"][field] = 1
                self.rejected(field)
        self.sample = measured(); self.sample["render"]["screenshots"]["supported"] = False
        self.rejected("missing queue support")

    def test_each_allocator_counter_must_return_to_exact_baseline(self):
        for field in self.baseline["render"]["resources"]:
            with self.subTest(field=field):
                self.sample = measured(); self.sample["render"]["resources"][field] += 1
                self.rejected(field)
        self.sample = measured(); self.sample["render"]["resources"]["textures"] -= 1
        self.rejected("loss also differs from warmed context")

    def test_context_restart_requires_explicit_new_baseline(self):
        for generation in (0, 6, 8):
            with self.subTest(generation=generation):
                self.sample = measured(); self.sample["render"]["contextGeneration"] = generation
                self.rejected("context mismatch")
        self.baseline["render"]["contextGeneration"] = 0
        self.sample = copy.deepcopy(self.baseline)
        self.rejected("both invalid is not a baseline")

    def test_invalid_numbers_and_booleans_cannot_impersonate_counts(self):
        for value in (-1, True, 0.0, math.nan, math.inf, "0", None):
            with self.subTest(value=value):
                self.sample = measured(); self.sample["jobs"]["workerPending"] = value
                self.rejected("invalid unsigned count")
        self.sample = measured(); self.sample["host"]["supported"] = 1
        self.rejected("integer is not support flag")

    def test_missing_group_or_measurement_never_defaults_to_zero(self):
        for group in measured():
            with self.subTest(group=group):
                self.sample = measured(); del self.sample[group]
                self.rejected("missing group")
        self.sample = measured(); del self.sample["host"]["audioCompletionOwnerRefs"]
        self.rejected("missing debt")

    def test_quiet_caches_cannot_exceed_the_frozen_finite_fixture_bound(self):
        for group, field in (("async", "cacheEntries"), ("async", "cacheBytes"),
            ("audio", "waveCacheEntries"), ("audio", "busVoices"), ("memory", "textureBytes")):
            with self.subTest(field=field):
                self.sample = measured(); self.sample[group][field] += 1
                self.rejected(field)

    def test_lua_and_private_bytes_use_the_frozen_growth_budget(self):
        for field, allowed in (("luaBytes", 1024**2), ("privateBytes", 64*1024**2)):
            with self.subTest(field=field):
                self.sample = measured(); self.sample["memory"][field] += allowed
                self.assertEqual(check_quiet(self.sample, self.baseline), [])
                self.sample["memory"][field] += 1
                self.rejected(field)

    def test_rss_is_recorded_but_not_used_as_a_private_allocation_substitute(self):
        self.sample["memory"]["rssBytes"] *= 2
        self.assertEqual(check_quiet(self.sample, self.baseline), [])
        self.sample["memory"]["rssBytes"] = -1
        self.rejected("invalid RSS measurement")

    def test_bad_baseline_cannot_legalize_unfinished_work(self):
        self.baseline["jobs"]["workerPending"] = 1
        self.sample = copy.deepcopy(self.baseline)
        self.rejected("busy baseline")
        self.baseline = measured(); self.baseline["audio"]["outputMode"] = "ManualMix"
        self.sample = measured()
        self.rejected("baseline backend mismatch")


if __name__ == "__main__":
    unittest.main()
