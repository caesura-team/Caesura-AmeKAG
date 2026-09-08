#include "doctest.h"
#include "TestPaths.h"

#include "di/BackendRegistry.h"
#include "live2d/NullAnimationBackend.h"
#include "live2d/PathConfinement.h"
#include "live2d/api/IAnimationBackend.h"
#include "render/api/IRenderDevice.h"
#include "render/api/ITextureManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Caesura;

namespace {

class FakeTextureManager final : public ITextureManager {
public:
    bool initialize() override { return true; }
    bool initialize(bool) override { return true; }
    void shutdown() override {}
    void setDevMode(bool) override {}

    uint32_t loadTexture(const std::string& path) override {
        loadedPaths.push_back(path);
        if (failLoads) return 0;
        const uint32_t id = nextId++;
        liveIds.insert(id);
        return id;
    }

    uint32_t loadTextureFromMemory(const uint8_t*, uint32_t,
                                   const std::string&) override {
        return 0;
    }
    uint32_t loadTextureFromRGBA(const uint8_t*, uint16_t, uint16_t,
                                 const std::string&) override {
        return 0;
    }
    uint32_t createSolidTexture(uint8_t, uint8_t, uint8_t, uint8_t) override {
        return 0;
    }
    uint32_t getPlaceholderTexture() override { return 0; }

    void destroyTexture(uint32_t id) override {
        destroyedIds.push_back(id);
        liveIds.erase(id);
    }

    uint32_t getTextureHandle(uint32_t id) const override {
        return isValid(id) ? rawHandleBase + id : 0;
    }

    void getTextureSizeById(uint32_t id, uint16_t& width,
                            uint16_t& height) const override {
        if (!isValid(id)) {
            width = 0;
            height = 0;
            return;
        }
        width = textureWidth;
        height = textureHeight;
    }

    bool isValid(uint32_t id) const override {
        return liveIds.contains(id);
    }
    bool describeTexture(uint32_t, TextureSourceInfo&) const override { return false; }

    uint64_t totalTextureBytes() const override { return 0; }
    bool checkBudget(uint32_t, uint16_t, uint16_t) override { return true; }
    void trackTexture(uint32_t, uint64_t) override {}
    void untrackTexture(uint32_t) override {}

    bool failLoads = false;
    uint32_t nextId = 11;
    uint32_t rawHandleBase = 80;
    uint16_t textureWidth = 100;
    uint16_t textureHeight = 200;
    std::vector<std::string> loadedPaths;
    std::vector<uint32_t> destroyedIds;
    std::unordered_set<uint32_t> liveIds;
};

class RecordingRenderDevice final : public IRenderDevice {
public:
    struct Blit {
        uint16_t view = 0;
        uint32_t texture = 0;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        uint8_t opacity = 0;
    };

    void setPresentSize(uint32_t, uint32_t) override {}
    bool init(void*, int width, int height) override {
        backbufferWidth = width;
        backbufferHeight = height;
        return true;
    }
    bool isInitialized() const override { return true; }
    void beginShutdown() override {}
    void shutdown() override {}
    void flushAllRTT() override {}
    void beginFrame() override {}
    void endFrame() override {}
    void commit_frame() override {}
    void advanceFrame() override {}
    void setScreenOffset(int, int) override {}
    void setViewRect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) override {}
    void setViewClear(uint16_t, uint16_t, uint32_t, float, uint8_t) override {}
    void touch(uint16_t) override {}
    ViewportHandle createRenderTarget(int, int) override { return {}; }
    void destroyRenderTarget(ViewportHandle) override {}
    void blitViewport(ViewportHandle, uint16_t, float, float, float, float) override {}
    RenderTextureHandle getViewportTexture(ViewportHandle) override { return {}; }
    int getBackbufferWidth() const override { return backbufferWidth; }
    int getBackbufferHeight() const override { return backbufferHeight; }
    void resize(int width, int height) override {
        backbufferWidth = width;
        backbufferHeight = height;
    }
    void blitTexture(uint16_t view, uint32_t texture, float x, float y,
                     float width, float height, uint8_t opacity) override {
        blits.push_back({view, texture, x, y, width, height, opacity});
    }
    void stretchBlt(uint16_t, uint32_t, float, float, float, float,
                    uint32_t, float, float, float, float, int) override {}
    void affineBlt(uint16_t, uint32_t, float, float, float, float,
                   uint32_t, float, float, float, float,
                   const float[6]) override {}
    void beginBatch() override {}
    void flushBatch() override {}
    void setDebugName(uint16_t, const std::string&) override {}
    void drawDebugOverlay(const std::string&) override {}
    bool requestScreenshot(const std::string&) override { return false; }
    bool recoverDevice(void*, int width, int height) override {
        resize(width, height);
        return true;
    }
    void flagDeviceLost() override {}
    bool consumeDeviceLost() override { return false; }
    void renderText(uint16_t, const std::string&, float, float,
                    uint8_t, uint8_t, uint8_t, uint8_t,
                    float, bool, bool, bool) override {}
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
    void submitBlend(uint16_t, RenderTextureHandle, RenderTextureHandle, int,
                     float, float, float) override {}
    void submitTransition(uint16_t, RenderTextureHandle, RenderTextureHandle,
                          RenderTextureHandle, int, float) override {}
    void submitVFX(uint16_t, RenderTextureHandle, int, float, float, float,
                   float, float, float, float) override {}
    void fillViewport(ViewportHandle, uint8_t, uint8_t, uint8_t, uint8_t) override {}
    bool setColorFilter(ColorFilterPreset) override { return true; }
    // -- Round-102 post-processing chain (interface conformance: recording mock
    // runs headless, so post-effects are unsupported / inert).
    bool isPostFxSupported(PostFxKind) const override { return false; }
    PostFxHandle createPostFx(PostFxKind, const PostFxParams&) override { return 0; }
    void setPostFxParams(PostFxHandle, const PostFxParams&) override {}
    void destroyPostFx(PostFxHandle) override {}
    void clearPostFx() override {}
    bool isPostFxActive() const override { return false; }
    RenderUniformHandle getDefaultSampler() const override { return {}; }
    RenderProgramHandle getFallbackProgram() const override { return {}; }
    const char* getBackendName() const override { return "RecordingRender"; }
    RenderRuntimeInfo getRuntimeInfo() const override {
        return {getBackendName(), backbufferWidth, backbufferHeight, 0, true};
    }
    bool setPreferredBackend(const char*) override { return false; }

    int backbufferWidth = 1280;
    int backbufferHeight = 720;
    std::vector<Blit> blits;
};

class RegistryScope final {
public:
    RegistryScope(ITextureManager* textureManager, IRenderDevice* renderDevice)
        : registry(BackendRegistry::instance())
        , previousTextureManager(registry.getTextureManager())
        , previousRenderDevice(registry.getRenderDevice()) {
        registry.setTextureManager(textureManager);
        registry.setRenderDevice(renderDevice);
    }

    ~RegistryScope() {
        registry.setTextureManager(previousTextureManager);
        registry.setRenderDevice(previousRenderDevice);
    }

private:
    BackendRegistry& registry;
    ITextureManager* previousTextureManager = nullptr;
    IRenderDevice* previousRenderDevice = nullptr;
};

struct NullAnimationFixture {
    FakeTextureManager textures;
    RecordingRenderDevice renderer;
    RegistryScope registry{&textures, &renderer};
    NullAnimationBackend animation;
};

} // namespace

TEST_CASE("NullAnimationBackend requires init and recognizes static images") {
    NullAnimationFixture fixture;

    CHECK(fixture.animation.loadModel("hero.png", "hero") == 0);
    CHECK(fixture.textures.loadedPaths.empty());
    CHECK(fixture.animation.init());
    CHECK(fixture.animation.init());
    CHECK(std::string(fixture.animation.name()) == "NullAnimation+PNG");

    const int handle = fixture.animation.loadModel("hero.PNG", "hero");
    CHECK(handle == 1);
    CHECK(fixture.animation.isLoaded(handle));
    CHECK(fixture.textures.loadedPaths == std::vector<std::string>{"hero.PNG"});

    CHECK(fixture.animation.loadModel("hero.model3.json", "hero") == 0);
    CHECK(fixture.animation.loadModel("", "hero") == 0);
    CHECK(fixture.textures.loadedPaths.size() == 1);
    fixture.animation.shutdown();
}

TEST_CASE("NullAnimationBackend exposes load failures without allocating handles") {
    NullAnimationFixture fixture;
    fixture.textures.failLoads = true;
    REQUIRE(fixture.animation.init());

    CHECK(fixture.animation.loadModel("missing.png", "missing") == 0);
    CHECK_FALSE(fixture.animation.isLoaded(0));
    CHECK(fixture.textures.destroyedIds.empty());
    fixture.animation.shutdown();
}

TEST_CASE("NullAnimationBackend rejects and releases textures without dimensions") {
    NullAnimationFixture fixture;
    fixture.textures.textureWidth = 0;
    REQUIRE(fixture.animation.init());

    CHECK(fixture.animation.loadModel("empty.png", "empty") == 0);
    CHECK_FALSE(fixture.animation.isLoaded(1));
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11});
    CHECK(fixture.textures.liveIds.empty());
    fixture.animation.shutdown();
    CHECK(fixture.textures.destroyedIds.size() == 1);
}

TEST_CASE("NullAnimationBackend stays optional without a texture service") {
    RecordingRenderDevice renderer;
    RegistryScope registry(nullptr, &renderer);
    NullAnimationBackend animation;

    CHECK(animation.init());
    CHECK(animation.loadModel("hero.png", "hero") == 0);
    CHECK_FALSE(animation.isLoaded(1));
    animation.shutdown();
    animation.shutdown();
}

TEST_CASE("NullAnimationBackend renders PNG state through abstract backends") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    const int handle = fixture.animation.loadModel("hero.png", "hero");
    REQUIRE(handle == 1);

    fixture.animation.showModel(handle, 10.0f, 20.0f, 1.5f);
    fixture.animation.setOpacity(handle, 0.25f);
    fixture.animation.render(0.016f);

    REQUIRE(fixture.renderer.blits.size() == 1);
    const auto& first = fixture.renderer.blits.front();
    CHECK(first.view == VIEW_MAIN);
    CHECK(first.texture == fixture.textures.rawHandleBase + 11);
    CHECK(first.x == doctest::Approx(10.0f));
    CHECK(first.y == doctest::Approx(20.0f));
    CHECK(first.width == doctest::Approx(150.0f));
    CHECK(first.height == doctest::Approx(300.0f));
    CHECK(first.opacity == 64);

    fixture.animation.hideModel(handle);
    fixture.animation.render(0.016f);
    CHECK(fixture.renderer.blits.size() == 1);

    fixture.animation.showModel(handle, 30.0f, 40.0f, 2.0f);
    fixture.animation.setOpacity(handle, 2.0f);
    fixture.animation.render(0.016f);
    REQUIRE(fixture.renderer.blits.size() == 2);
    CHECK(fixture.renderer.blits.back().opacity == 255);
    CHECK(fixture.renderer.blits.back().width == doctest::Approx(200.0f));
    CHECK(fixture.renderer.blits.back().height == doctest::Approx(400.0f));

    fixture.animation.unloadModel(handle);
    CHECK_FALSE(fixture.animation.isLoaded(handle));
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11});
    fixture.animation.unloadModel(handle);
    CHECK(fixture.textures.destroyedIds.size() == 1);
    fixture.animation.shutdown();
    CHECK(fixture.textures.destroyedIds.size() == 1);
}

TEST_CASE("NullAnimationBackend shutdown releases each PNG once and resets handles") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    const int first = fixture.animation.loadModel("left.png", "left");
    const int second = fixture.animation.loadModel("right.png", "right");
    REQUIRE(first == 1);
    REQUIRE(second == 2);

    fixture.animation.shutdown();
    CHECK_FALSE(fixture.animation.isLoaded(first));
    CHECK_FALSE(fixture.animation.isLoaded(second));
    std::sort(fixture.textures.destroyedIds.begin(), fixture.textures.destroyedIds.end());
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11, 12});

    fixture.animation.shutdown();
    CHECK(fixture.textures.destroyedIds.size() == 2);

    REQUIRE(fixture.animation.init());
    CHECK(fixture.animation.loadModel("again.png", "again") == 1);
    fixture.animation.shutdown();
    CHECK(fixture.textures.destroyedIds.size() == 3);
}

TEST_CASE("NullAnimationBackend remains safe through IAnimationBackend") {
    NullAnimationFixture fixture;
    IAnimationBackend* animation = &fixture.animation;

    REQUIRE(animation->init());
    CHECK_FALSE(animation->playMotion(999, "idle"));
    animation->showModel(999, 0.0f, 0.0f, 1.0f);
    animation->setOpacity(999, 0.5f);
    animation->hideModel(999);
    animation->unloadModel(999);
    animation->render(0.0f);
    CHECK(fixture.renderer.blits.empty());
    animation->shutdown();
}

TEST_CASE("NullAnimationBackend counts loaded models regardless of visibility") {
    NullAnimationFixture fixture;
    IAnimationBackend& animation = fixture.animation;
    const IAnimationBackend& query = animation;
    CHECK(query.loadedModelCount() == 0);
    animation.clearModels();
    REQUIRE(animation.init());

    const int visible = animation.loadModel("visible.png", "visible");
    const int hidden = animation.loadModel("hidden.png", "hidden");
    REQUIRE(visible > 0);
    REQUIRE(hidden > 0);
    animation.showModel(visible, 0.0f, 0.0f, 1.0f);
    CHECK(query.loadedModelCount() == 2);
    animation.hideModel(visible);
    CHECK(query.loadedModelCount() == 2);
    fixture.textures.failLoads = true;
    CHECK(animation.loadModel("missing.png", "missing") == 0);
    CHECK(query.loadedModelCount() == 2);

    animation.unloadModel(hidden);
    CHECK(query.loadedModelCount() == 1);
    animation.unloadModel(hidden);
    CHECK(query.loadedModelCount() == 1);
    animation.shutdown();
    CHECK(query.loadedModelCount() == 0);
    animation.clearModels();
    CHECK(fixture.textures.destroyedIds.size() == 2);
}

TEST_CASE("NullAnimationBackend clearModels releases visible and hidden models once") {
    NullAnimationFixture fixture;
    IAnimationBackend& animation = fixture.animation;
    REQUIRE(animation.init());
    const int visible = animation.loadModel("visible.png", "visible");
    const int hidden = animation.loadModel("hidden.png", "hidden");
    REQUIRE(visible > 0);
    REQUIRE(hidden > 0);
    animation.showModel(visible, 0.0f, 0.0f, 1.0f);
    animation.render(0.016f);
    REQUIRE(fixture.renderer.blits.size() == 1);

    animation.clearModels();
    CHECK(animation.loadedModelCount() == 0);
    CHECK_FALSE(animation.isLoaded(visible));
    CHECK_FALSE(animation.isLoaded(hidden));
    CHECK(fixture.textures.liveIds.empty());
    const std::unordered_set<uint32_t> released(
        fixture.textures.destroyedIds.begin(), fixture.textures.destroyedIds.end());
    CHECK(released == std::unordered_set<uint32_t>{11, 12});
    CHECK(fixture.textures.destroyedIds.size() == 2);
    animation.render(0.016f);
    CHECK(fixture.renderer.blits.size() == 1);

    animation.clearModels();
    animation.unloadModel(visible);
    animation.unloadModel(hidden);
    animation.shutdown();
    CHECK(fixture.textures.destroyedIds.size() == 2);
}

TEST_CASE("NullAnimationBackend clearModels keeps the backend ready without reusing handles") {
    NullAnimationFixture fixture;
    IAnimationBackend& animation = fixture.animation;
    REQUIRE(animation.init());
    const int stale = animation.loadModel("hero.png", "hero");
    REQUIRE(stale > 0);
    animation.clearModels();
    animation.clearModels();

    const int fresh = animation.loadModel("hero.png", "hero");
    REQUIRE(fresh > 0);
    CHECK(fresh != stale);
    CHECK(animation.loadedModelCount() == 1);
    animation.showModel(fresh, 10.0f, 20.0f, 1.0f);
    animation.hideModel(stale);
    animation.unloadModel(stale);
    CHECK(animation.isLoaded(fresh));
    animation.render(0.016f);
    REQUIRE(fixture.renderer.blits.size() == 1);
    CHECK(fixture.renderer.blits.front().texture == fixture.textures.rawHandleBase + 12);
    CHECK(fixture.textures.liveIds == std::unordered_set<uint32_t>{12});

    animation.shutdown();
    CHECK(animation.loadedModelCount() == 0);
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11, 12});
}

TEST_CASE("confineToModelRoot rejects absolute paths outside the working directory") {
    // Absolute paths outside the process CWD (model root) must be rejected.
#ifdef _WIN32
    CHECK(Caesura::confineToModelRoot("C:/Windows/win.ini").empty());
    CHECK(Caesura::confineToModelRoot("C:/Program Files").empty());
    // Root-relative drive path (no drive letter) resolves relative to CWD drive.
    CHECK(Caesura::confineToModelRoot("\\server\\share\\file").empty());
#else
    CHECK(Caesura::confineToModelRoot("/etc/passwd").empty());
    CHECK(Caesura::confineToModelRoot("/usr/share").empty());
#endif
}

TEST_CASE("confineToModelRoot rejects dot-dot escapes") {
    // Escaping upward from the CWD via .. is rejected.
    CHECK(Caesura::confineToModelRoot("../escape.txt").empty());
    CHECK(Caesura::confineToModelRoot("../../escape.txt").empty());
    CHECK(Caesura::confineToModelRoot("a/../../escape.txt").empty());
}

TEST_CASE("confineToModelRoot handles dot components") {
    // "." resolves to the working directory itself, which is the model root
    // and is allowed (the guard explicitly permits the root).
    const std::string root = Caesura::confineToModelRoot(".");
    CHECK_FALSE(root.empty());
    // ".." resolves to the parent of the root and must be rejected.
    CHECK(Caesura::confineToModelRoot("..").empty());
}

TEST_CASE("confineToModelRoot accepts paths inside the working directory") {
    // A path under the CWD is confined and returned non-empty.
    const std::string confined = Caesura::confineToModelRoot(".");
    CHECK_FALSE(confined.empty());
    // A plain relative file name under the root passes (may not exist on disk,
    // but containment is a lexical/canonical property of the prefix).
    const std::string file = Caesura::confineToModelRoot("live2d_test/Haru/Haru.model3.json");
    CHECK_FALSE(file.empty());
}

TEST_CASE("confineToModelRoot rejects prefix look-alike outside the root") {
    // A sibling whose name starts with the root string must not pass the
    // naive prefix check (boundary: root + "/" is required).
    const std::string root = std::filesystem::current_path().string();
    const std::string lookalike = root + "X";
    if (std::filesystem::exists(lookalike)) {
        CHECK(Caesura::confineToModelRoot(lookalike).empty());
    } else {
        // Lookalike does not exist; containment still resolves canonically
        // outside the root and must be rejected.
        CHECK(Caesura::confineToModelRoot(lookalike + "/f.txt").empty());
    }
}

TEST_CASE("confineToModelRoot resolves symlinks and keeps containment") {
    // A simulator may place its temp directory beneath the original CWD.
    // An explicit model root makes the sibling target outside on every host.
    namespace fs = std::filesystem;
    TestPaths::ScopedTempDir fixture("model_root_symlinks");
    struct RestoreWorkingDirectory {
        fs::path original = fs::current_path();
        ~RestoreWorkingDirectory() {
            std::error_code ignored;
            fs::current_path(original, ignored);
        }
    } restore;
    const fs::path root = fixture.path() / "model-root";
    const fs::path outside = fixture.path() / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    const fs::path insideTarget = root / "inside.txt";
    const fs::path outsideTarget = outside / "target.txt";
    { std::ofstream(insideTarget) << "inside"; }
    { std::ofstream(outsideTarget) << "outside"; }
    REQUIRE(fs::is_regular_file(insideTarget));
    REQUIRE(fs::is_regular_file(outsideTarget));
    fs::current_path(root);
    CHECK_FALSE(Caesura::confineToModelRoot(insideTarget.string()).empty());
    CHECK(Caesura::confineToModelRoot(outsideTarget.string()).empty());

    const fs::path escapeLink = root / "escape.txt";
    std::error_code ec;
    fs::create_symlink(outsideTarget, escapeLink, ec);
#ifdef _WIN32
    if (ec) {
        MESSAGE("Symlink creation unavailable on this Windows host: ", ec.message());
        return; // Direct inside/outside checks above still ran.
    }
#endif
    REQUIRE_MESSAGE(!ec, ec.message());
    CHECK(Caesura::confineToModelRoot(escapeLink.string()).empty());

    const fs::path insideLink = root / "inside-link.txt";
    fs::create_symlink(insideTarget, insideLink, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    CHECK(Caesura::confineToModelRoot(insideLink.string()) == fs::canonical(insideTarget).string());
}

// ---------------------------------------------------------------------------
// G3: Conditional-compilation guard stubs for the Live2D Cubism SDK path.
//
// The Cubism SDK is proprietary and absent from CI, so the SDK backend
// (Live2DBackend + render paths) is never compiled by default. These static
// source assertions keep that boundary honest: they fail before a release if
// (a) the no-SDK PNG fallback (already covered above) stops being the build's
// default animation backend, or (b) an SDK-only include leaks into a no-SDK
// build, or (c) the composition root regresses its "no SDK -> NullAnimation"
// fallback wiring.
// ---------------------------------------------------------------------------

#include <sstream>

namespace {

std::string readSourceRelative(const std::string& relative) {
#ifdef CAESURA_SOURCE_DIR
    // Out-of-tree builds: prefer the CMake-injected source root.
    const std::filesystem::path fromMacro(CAESURA_SOURCE_DIR);
    if (std::filesystem::exists(fromMacro / "src") &&
        std::filesystem::exists(fromMacro / "tests" / "cpp")) {
        const auto full = fromMacro / relative;
        std::ifstream file(full, std::ios::binary);
        std::ostringstream out;
        out << file.rdbuf();
        return out.str();
    }
#endif
    auto path = std::filesystem::current_path();
    while (!path.empty()) {
        if (std::filesystem::exists(path / "src") &&
            std::filesystem::exists(path / "tests" / "cpp")) {
            break;
        }
        const auto parent = path.parent_path();
        if (parent == path) {
            path.clear();
            break;
        }
        path = parent;
    }
    REQUIRE_FALSE(path.empty());
    const auto full = path / relative;
    std::ifstream file(full, std::ios::binary);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

} // namespace

TEST_CASE("Live2D SDK backend is guard-wrapped and never leaks SDK headers") {
    const std::string header = readSourceRelative("src/live2d/Live2D/Live2DBackend.h");
    const std::string impl = readSourceRelative("src/live2d/Live2D/Live2DBackend.cpp");

    // The SDK backend must be entirely inside a CAESURA_LIVE2D guard (the
    // header and .cpp both carry it before any SDK content).
    CHECK(header.find("#ifdef CAESURA_LIVE2D") != std::string::npos);
    CHECK(header.find("#include \"../api/IAnimationBackend.h\"") != std::string::npos);
    CHECK(impl.find("#ifdef CAESURA_LIVE2D") != std::string::npos);
    // The SDK include (Cubism framework/core) lives only inside the guard.
    CHECK(impl.find("#include <CubismFramework.hpp>") != std::string::npos);
    CHECK(impl.find("CubismFramework::StartUp") != std::string::npos);
    CHECK(impl.find("CubismFramework::Initialize()") != std::string::npos);
}

TEST_CASE("Live2D null PNG fallback is the always-compiled default") {
    // NullAnimationBackend + PathConfinement must be unconditionally listed in
    // the Live2D module (no build-time exclusion), so the no-SDK path is always
    // present and testable.
    const std::string modules = readSourceRelative("cmake/CaesuraModules.cmake");
    const std::string root = readSourceRelative("CMakeLists.txt");

    // The static module lists the fallback unconditionally ...
    CHECK(modules.find("src/live2d/NullAnimationBackend.cpp") != std::string::npos);
    CHECK(modules.find("src/live2d/PathConfinement.cpp") != std::string::npos);
    // ... while the SDK backend source is only added behind CAESURA_LIVE2D.
    CHECK(root.find("src/live2d/Live2D/Live2DBackend.cpp") != std::string::npos);
    CHECK(root.find("option(CAESURA_LIVE2D ") != std::string::npos);
}

TEST_CASE("Live2D no-SDK composition root wires NullAnimation fallback") {
    // Composition root (entry) must select NullAnimationBackend when the SDK
    // is not available, never constructing the SDK backend unconditionally.
    const std::string engine = readSourceRelative("src/entry/Engine_Backends.cpp");
    CHECK(engine.find("std::make_unique<NullAnimationBackend>()") != std::string::npos);
    CHECK(engine.find("std::make_unique<Live2DBackend>()") != std::string::npos);
}


// ---------------------------------------------------------------------------
// Boundary lifecycle for the Null PNG fallback (G3 SDK-less path)
// ---------------------------------------------------------------------------

TEST_CASE("NullAnimationBackend render is safe with no loaded model") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    // No model loaded: render must be a no-op and must not crash or emit blits.
    fixture.animation.render(0.016f);
    CHECK(fixture.renderer.blits.empty());
    fixture.animation.shutdown();
}

TEST_CASE("NullAnimationBackend load-then-unload frees the texture immediately") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    const int h = fixture.animation.loadModel("sprite.png", "sprite");
    REQUIRE(h == 1);
    CHECK(fixture.animation.isLoaded(h));
    fixture.animation.unloadModel(h);
    CHECK_FALSE(fixture.animation.isLoaded(h));
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11});

    // Reloading the same path yields a fresh handle AND a fresh texture id.
    const int h2 = fixture.animation.loadModel("sprite.png", "sprite");
    REQUIRE(h2 == 2);
    CHECK(fixture.animation.isLoaded(h2));
    CHECK(fixture.textures.liveIds == std::unordered_set<uint32_t>{12});

    fixture.animation.unloadModel(h2);
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11, 12});
    fixture.animation.shutdown();
    // shutdown after manual unloads frees nothing extra.
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11, 12});
}

TEST_CASE("NullAnimationBackend reload of the same animation yields independent sprites") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    const int first = fixture.animation.loadModel("face.png", "face");
    const int second = fixture.animation.loadModel("face.png", "face");
    REQUIRE(first == 1);
    REQUIRE(second == 2);
    CHECK(first != second);
    CHECK(fixture.animation.isLoaded(first));
    CHECK(fixture.animation.isLoaded(second));
    CHECK(fixture.textures.loadedPaths ==
          std::vector<std::string>{"face.png", "face.png"});
    CHECK(fixture.textures.liveIds == std::unordered_set<uint32_t>{11, 12});

    // Unloading the first leaves the second intact.
    fixture.animation.unloadModel(first);
    CHECK_FALSE(fixture.animation.isLoaded(first));
    CHECK(fixture.animation.isLoaded(second));
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11});
    fixture.animation.shutdown();
    CHECK(fixture.textures.destroyedIds == std::vector<uint32_t>{11, 12});
}

// ---------------------------------------------------------------------------
// Boundary degrade: animation controls & params on the Null PNG fallback
// ---------------------------------------------------------------------------

TEST_CASE("NullAnimationBackend degrades gracefully for motion/expression/params") {
    NullAnimationFixture fixture;
    REQUIRE(fixture.animation.init());
    const int h = fixture.animation.loadModel("hero.png", "hero");
    REQUIRE(h == 1);

    // Motion degrades to "not supported" (false); expression & parameter are
    // accepted as no-ops (must not crash, must not corrupt sprite state).
    CHECK_FALSE(fixture.animation.playMotion(h, "idle"));
    fixture.animation.setExpression(h, "angry");
    fixture.animation.setParameter(h, "ParamAngleX", -32.0f);
    fixture.animation.setParameter(h, "ParamAngleY", 40.0f);

    // Unknown handles are no-ops too.
    CHECK_FALSE(fixture.animation.playMotion(999, "idle"));
    fixture.animation.setExpression(999, "x");
    fixture.animation.setParameter(999, "p", 1.0f);

    // The sprite still renders with its default state after all the above.
    fixture.animation.showModel(h, 5.0f, 5.0f, 1.0f);
    fixture.animation.setOpacity(h, 12.0f);  // clamped to 1.0
    fixture.animation.render(0.016f);
    REQUIRE(fixture.renderer.blits.size() == 1);
    CHECK(fixture.renderer.blits.front().opacity == 255);
    fixture.animation.shutdown();
}

// ---------------------------------------------------------------------------
// Boundary: PathConfinement depth (empty, dot-prefix, backslash, Unicode)
// ---------------------------------------------------------------------------

TEST_CASE("confineToModelRoot handles empty path without crashing (fail closed)") {
    // An empty path cannot be verified as inside the model root, so the guard
    // must fail closed (return empty) rather than resolving it to the root.
    // Correct secure behavior: an empty string is rejected, never crashes.
    const std::string empty = Caesura::confineToModelRoot("");
    CHECK(empty.empty());
}

TEST_CASE("confineToModelRoot accepts dot-prefix and backslash relative paths under root") {
    namespace fs = std::filesystem;
    // A real file under the CWD makes canonicalization deterministic.
    const fs::path folder = fs::current_path() / "pc_depth_tmp";
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path file = folder / "sprite.png";
    if (!ec) {
        std::ofstream(file) << "x";
    }

    const std::string relName = file.filename().string();
    const std::string dotRel = "./pc_depth_tmp/" + relName;   // "./" prefix
    const std::string confinedDot = Caesura::confineToModelRoot(dotRel);
    CHECK_FALSE(confinedDot.empty());
    CHECK(confinedDot.find("pc_depth_tmp") != std::string::npos);

#ifdef _WIN32
    // Backslash separators must be normalized to forward slashes so the
    // containment prefix comparison still passes.
    const std::string backRel = "pc_depth_tmp\\" + relName;
    const std::string confinedBack = Caesura::confineToModelRoot(backRel);
    CHECK_FALSE(confinedBack.empty());
    CHECK(confinedBack.find("pc_depth_tmp") != std::string::npos);
    // Windows dot-dot escape with backslashes is still rejected.
    CHECK(Caesura::confineToModelRoot("..\\escape.png").empty());
    CHECK(Caesura::confineToModelRoot("..\\..\\escape.png").empty());
#endif

    fs::remove_all(folder, ec);
}

TEST_CASE("confineToModelRoot keeps Unicode relative paths under the root") {
    namespace fs = std::filesystem;
    const std::string token = "模型" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path folder = fs::current_path() / token;
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (!ec) {
        const std::string confined = Caesura::confineToModelRoot(token + "/model.model3.json");
        CHECK_FALSE(confined.empty());
        CHECK(confined.find(token) != std::string::npos);
    }
    fs::remove_all(folder, ec);
}

// ---------------------------------------------------------------------------
// Boundary: BackendRegistry integration for animation backend
// ---------------------------------------------------------------------------

TEST_CASE("BackendRegistry getAnimationBackend round-trip") {
    auto& reg = BackendRegistry::instance();
    NullAnimationBackend backend;
    reg.setAnimationBackend(&backend);
    CHECK(reg.getAnimationBackend() == &backend);
    reg.setAnimationBackend(nullptr);
    CHECK(reg.getAnimationBackend() == nullptr);
}
