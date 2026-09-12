// =============================================================================
// test_rtt_pool.cpp — GPU-free unit tests for RTTManager pool logic.
//
// Covers the pure-C++ pool behaviours (acquire/reuse, 2D/3D isolation,
// deferred destroys, resize reset, zero-size rejection) using a counting mock
// of IRenderDevice. No real GPU resources are created: every acquire in these
// tests uses clear=false so the bgfx clearRTT() path is never exercised.
//
// NOTE: RTTManager::resizeAll() was previously declared-but-undefined (link
// failure). It is now implemented as a clearAll() delegate; the resizeAll test
// case below is the regression guard for that API.
// =============================================================================

#include "doctest.h"

#include "render/RTTManager.h"
#include "render/api/IRenderDevice.h"

#include <vector>
#include <cstdint>
#include <algorithm>

using namespace Caesura;

namespace {

// -----------------------------------------------------------------------------
// CountingRenderDevice — implements every pure virtual in IRenderDevice as a
// safe no-op EXCEPT the two pool-relevant entry points, which record/allocate:
//   * createRenderTarget(w,h)  -> returns an INCREMENTING ViewportHandle id
//     (starts at 1; id 0 means failure), records the (w,h) pair, bumps createCount
//   * destroyRenderTarget(h)   -> records the destroyed id, bumps destroyCount
// -----------------------------------------------------------------------------
class CountingRenderDevice final : public IRenderDevice {
public:
    // -- lifecycle --
    bool init(void*, int width, int height) override { return true; }
    void setPresentSize(uint32_t, uint32_t) override {}
    bool isInitialized() const override { return true; }
    void beginShutdown() override {}
    void shutdown() override {}
    void flushAllRTT() override {}

    // -- frame management --
    void beginFrame() override {}
    void endFrame() override {}
    void commit_frame() override {}
    void advanceFrame() override {}

    // -- view management --
    void setScreenOffset(int, int) override {}
    void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) override {}
    void touch(uint16_t) override {}

    // -- offscreen render target -- (the interesting bits)
    ViewportHandle createRenderTarget(int width, int height) override {
        ++createCount;
        createdPairs.push_back({width, height});
        ViewportHandle h;
        h.id = static_cast<uint32_t>(nextId++);
        return h; // never returns id 0 (ids start at 1)
    }
    void destroyRenderTarget(ViewportHandle handle) override {
        ++destroyCount;
        destroyedIds.push_back(handle.id);
    }

    void blitViewport(ViewportHandle, uint16_t, float, float, float, float) override {}
    RenderTextureHandle getViewportTexture(ViewportHandle) override { return {}; }
    int getBackbufferWidth() const override { return 1280; }
    int getBackbufferHeight() const override { return 720; }
    void resize(int, int) override {}
    void blitTexture(uint16_t, uint32_t, float, float, float, float, uint8_t) override {}
    void stretchBlt(uint16_t, uint32_t, float, float, float, float,
                    uint32_t, float, float, float, float, int) override {}
    void affineBlt(uint16_t, uint32_t, float, float, float, float,
                   uint32_t, float, float, float, float, const float[6]) override {}
    void beginBatch() override {}
    void flushBatch() override {}
    void setDebugName(uint16_t, const std::string&) override {}
    void drawDebugOverlay(const std::string&) override {}
    bool requestScreenshot(const std::string&) override { return false; }
    Caesura::ScreenshotResult requestScreenshot(const Caesura::ScreenshotOptions&) override {
        Caesura::ScreenshotResult result;
        result.status = Caesura::ScreenshotStatus::Failed;
        result.error = "screenshots unsupported by test renderer";
        return result;
    }
    Caesura::ScreenshotResult takeScreenshot(const Caesura::ScreenshotTicket&) override { return {}; }
    bool cancelScreenshot(const Caesura::ScreenshotTicket&) override { return false; }
    bool recoverDevice(void*, int, int) override { return true; }
    void flagDeviceLost() override {}
    bool consumeDeviceLost() override { return false; }

    // -- text --
    void renderText(uint16_t, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t, float, bool, bool, bool) override {}
    void renderRuby(uint16_t, const std::string&, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void setFont(int) override {}
    bool loadTTF(const char*, float) override { return false; }
    FontRestoreState captureFontState() const override { return {}; }
    FontRestoreState defaultFontState() const override { return {}; }
    std::unique_ptr<IPreparedFontState> prepareFontState(const FontRestoreState&, const uint8_t*, size_t) override { return {}; }
    bool applyFontState(std::unique_ptr<IPreparedFontState>) override { return false; }
    void clearFontState() override {}
    float textLineHeight() const override { return 0.0f; }

    // -- blend / transition / vfx --
    void submitBlend(uint16_t, RenderTextureHandle, RenderTextureHandle, int,
                     float, float, float) override {}
    void submitTransition(uint16_t, RenderTextureHandle, RenderTextureHandle,
                          RenderTextureHandle, int, float) override {}
    void submitVFX(uint16_t, RenderTextureHandle, int, float, float, float, float,
                   float, float, float) override {}
    void fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    bool setColorFilter(ColorFilterPreset) override { return true; }

    // -- post-processing chain (round 102) -- gracefully no-op like Null
    bool isPostFxSupported(PostFxKind) const override { return false; }
    PostFxHandle createPostFx(PostFxKind, const PostFxParams&) override { return 0; }
    void setPostFxParams(PostFxHandle, const PostFxParams&) override {}
    void destroyPostFx(PostFxHandle) override {}
    void clearPostFx() override {}
    bool isPostFxActive() const override { return false; }

    // -- shaders / sampler --
    RenderUniformHandle getDefaultSampler() const override { return {}; }
    RenderProgramHandle getFallbackProgram() const override { return {}; }
    RenderProgramHandle getModulatedTextureProgram() const override { return {}; }

    // -- backend identification --
    const char* getBackendName() const override { return "CountingRenderDevice"; }
    RenderRuntimeInfo getRuntimeInfo() const override {
        return RenderRuntimeInfo{getBackendName(), 1280, 720, 0, true};
    }
    bool setPreferredBackend(const char*) override { return false; }

    // -- recorded state --
    int nextId = 1;                 // next ViewportHandle id to hand out
    int createCount = 0;            // # createRenderTarget calls
    int destroyCount = 0;           // # destroyRenderTarget calls
    std::vector<std::pair<int, int>> createdPairs; // (w,h) as created
    std::vector<uint32_t> destroyedIds;            // ids as destroyed
};

TEST_CASE("RTT pool: acquire creates and reuses matching entries") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle a = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    ViewportHandle b = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);

    // Two distinct handles for two acquires.
    REQUIRE(a.id != 0);
    REQUIRE(b.id != 0);
    REQUIRE(a != b);
    REQUIRE(dev.createCount == 2); // both created fresh

    mgr.releaseCanvas(a);

    // Re-acquiring the same size reuses the released entry, no new create.
    ViewportHandle c = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(c == a);              // same id reused
    REQUIRE(dev.createCount == 2); // createRenderTarget NOT called again
    REQUIRE(dev.destroyCount == 0);
}

TEST_CASE("RTT pool: size mismatch allocates new entry") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle a = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(a.id != 0);
    REQUIRE(dev.createCount == 1);

    mgr.releaseCanvas(a);

    // Different size => the released 64x64 entry cannot be reused.
    ViewportHandle b = mgr.acquireCanvas(128, 128, RTType::RT_2D, false);
    REQUIRE(b.id != 0);
    REQUIRE(b != a);
    REQUIRE(dev.createCount == 2); // createRenderTarget called again
}

TEST_CASE("RTT pool: 2D/3D pools are isolated") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle a2d = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(a2d.id != 0);
    REQUIRE(dev.createCount == 1);

    mgr.releaseCanvas(a2d);

    // Same size but 3D: the 2D free entry must NOT be reused (dual-pool isolation).
    ViewportHandle a3d = mgr.acquireCanvas(64, 64, RTType::RT_3D, false);
    REQUIRE(a3d.id != 0);
    REQUIRE(a3d != a2d);
    REQUIRE(dev.createCount == 2); // createCount increments

    // A 2D acquire of the same size still reuses the 2D entry (id 1).
    ViewportHandle a2dAgain = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(a2dAgain == a2d);
    REQUIRE(dev.createCount == 2);
}

TEST_CASE("RTT pool: release unknown handle is safe") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    // Releasing a handle that was never created must not crash or corrupt state.
    mgr.releaseCanvas(ViewportHandle{9999});
    REQUIRE(dev.createCount == 0);
    REQUIRE(dev.destroyCount == 0);

    // Pool still fully usable afterwards.
    ViewportHandle a = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(a.id != 0);
    REQUIRE(dev.createCount == 1);

    mgr.releaseCanvas(a);
    ViewportHandle b = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(b == a);               // reuse still works
    REQUIRE(dev.createCount == 1);
}

TEST_CASE("RTT pool: deferred destroys flush once per frame") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    constexpr int kNum = 4;
    std::vector<ViewportHandle> handles;
    handles.reserve(kNum);
    for (int i = 0; i < kNum; ++i) {
        ViewportHandle h = mgr.acquireCanvas(32 + i, 32, RTType::RT_2D, false);
        REQUIRE(h.id != 0);
        handles.push_back(h);
    }
    REQUIRE(dev.createCount == kNum);

    // Defer destruction of every acquired canvas.
    for (auto h : handles) {
        mgr.destroyCanvasDeferred(h);
    }
    REQUIRE(dev.destroyCount == 0); // nothing destroyed yet

    mgr.flushDeferredDestroys();
    REQUIRE(dev.destroyCount == kNum); // all destroyed exactly once

    // Every destroyed id matches a previously-created id.
    std::vector<uint32_t> createdIds;
    createdIds.reserve(handles.size());
    for (auto h : handles) createdIds.push_back(h.id);
    for (auto id : dev.destroyedIds) {
        REQUIRE(std::find(createdIds.begin(), createdIds.end(), id) != createdIds.end());
    }

    // Second flush must be a no-op (queue already cleared).
    const int countBefore = dev.destroyCount;
    mgr.flushDeferredDestroys();
    REQUIRE(dev.destroyCount == countBefore);
}

TEST_CASE("RTT pool: resizeAll clears pool") {
    // Regression guard: resizeAll() must clear the pool (implemented as a
    // clearAll() delegate since pooled canvases are re-created on demand).
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle a = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(a.id != 0);
    mgr.releaseCanvas(a);

    const int destroysAfterChange = dev.destroyCount;
    mgr.resizeAll(320, 240);
    REQUIRE(dev.destroyCount > destroysAfterChange); // pooled canvas destroyed

    // Pool emptied: a subsequent acquire must create a brand-new target.
    const int createsAfterChange = dev.createCount;
    ViewportHandle b = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(b.id != 0);
    REQUIRE(dev.createCount == createsAfterChange + 1); // new create, not reuse
}

TEST_CASE("RTT pool: acquire zero-size rejected") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle h = mgr.acquireCanvas(0, 0, RTType::RT_2D);
    REQUIRE(h.id == 0);        // invalid handle
    REQUIRE(dev.createCount == 0); // no device call was made

    ViewportHandle hw = mgr.acquireCanvas(0, 16, RTType::RT_2D);
    REQUIRE(hw.id == 0);
    ViewportHandle hh = mgr.acquireCanvas(16, 0, RTType::RT_2D);
    REQUIRE(hh.id == 0);
    REQUIRE(dev.createCount == 0);
}


// -----------------------------------------------------------------------------
// Regression: deferred destroys must keep the 2D/3D pools' index spaces
// independent. The old fixup decremented EVERY map entry after an erase,
// corrupting the other pool's indices (masked by id checks at release time,
// but causing spurious error logs and slow-path lookups).
// -----------------------------------------------------------------------------
TEST_CASE("RTT pool: interleaved flush keeps 2D/3D indices independent") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    // 2D pool: [a(64), b(128)] ; 3D pool: [c(64), d(128)]
    ViewportHandle a = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    ViewportHandle b = mgr.acquireCanvas(128, 128, RTType::RT_2D, false);
    ViewportHandle c = mgr.acquireCanvas(64, 64, RTType::RT_3D, false);
    ViewportHandle d = mgr.acquireCanvas(128, 128, RTType::RT_3D, false);
    REQUIRE(dev.createCount == 4);

    // Defer destroying the first entry of EACH pool (index 0 in both pools).
    // A fixup that ignores pool identity would decrement b and d together,
    // corrupting d's 3D index (d must stay at 3D index 1).
    mgr.destroyCanvasDeferred(a); // 2D index 0
    mgr.destroyCanvasDeferred(c); // 3D index 0
    mgr.flushDeferredDestroys();
    REQUIRE(dev.destroyCount == 2);

    // Both surviving handles must still release and reuse through the
    // map fast path (i.e. their recorded indices match reality).
    mgr.releaseCanvas(b);
    mgr.releaseCanvas(d);

    ViewportHandle b2 = mgr.acquireCanvas(128, 128, RTType::RT_2D, false);
    ViewportHandle d2 = mgr.acquireCanvas(128, 128, RTType::RT_3D, false);
    REQUIRE(b2 == b); // reused the freed 2D entry
    REQUIRE(d2 == d); // reused the freed 3D entry
    REQUIRE(dev.createCount == 4); // no new creates

    // The two pools must still be isolated after the interleaved flush.
    ViewportHandle b3 = mgr.acquireCanvas(64, 64, RTType::RT_2D, false);
    REQUIRE(b3 != c); // 3D entry must not be reused for a 2D acquire
    REQUIRE(dev.createCount == 5); // fresh 2D create
}

// -----------------------------------------------------------------------------
// Regression: legacy handles (createCanvas) are ALSO pooled entries. The old
// flush never removed the legacy marker, so a later clearAll()/resizeAll()
// destroyed the same GPU target a SECOND time.
// -----------------------------------------------------------------------------
TEST_CASE("RTT pool: legacy handle destroyed exactly once across flush + clearAll") {
    CountingRenderDevice dev;
    RTTManager mgr(dev);

    ViewportHandle h = mgr.createCanvas(64, 64); // legacy API -> pooled + marked
    REQUIRE(h.id != 0);
    REQUIRE(dev.createCount == 1);

    mgr.destroyCanvas(h); // deferred destruction
    mgr.flushDeferredDestroys();
    REQUIRE(dev.destroyCount == 1); // destroyed once by the pool path

    // clearAll() (via resizeAll) must NOT re-destroy the legacy id.
    mgr.resizeAll(320, 240);
    REQUIRE(dev.destroyCount == 1); // old code: 2 (double destroy)
}

} // namespace
