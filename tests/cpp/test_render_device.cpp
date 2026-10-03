// test_render_device.cpp - BgfxRenderDevice + RTTManager + EmbeddedShaders tests
#include "doctest.h"
#include "render/BgfxRenderDevice.h"
#include "render/RTTManager.h"
#include "render/EmbeddedShaders.h"
#include <cstring>
#include <string>
#include "render/NullRenderDevice.h"

using namespace Caesura;

TEST_CASE("BgfxRenderDevice::name on null") {
    BgfxRenderDevice rd;
    CHECK(rd.getBackendName() != nullptr);
}

TEST_CASE("BgfxRenderDevice::backbuffer dimensions default") {
    BgfxRenderDevice rd;
    CHECK(rd.getBackbufferWidth() == 1280);
    CHECK(rd.getBackbufferHeight() == 720);
}

TEST_CASE("BgfxRenderDevice::shutdown without init is safe") {
    BgfxRenderDevice rd;
    rd.shutdown();

    BgfxDeviceCore core;
    core.shutdown();
    core.shutdown();
}

TEST_CASE("BgfxRenderDevice::double shutdown is idempotent") {
    BgfxRenderDevice rd;
    rd.shutdown();
    rd.shutdown();
}

TEST_CASE("Bgfx frame entry points are safe outside an initialized lifetime") {
    BgfxRenderDevice rd;
    CHECK_FALSE(rd.getRuntimeInfo().shaderReady);
    CHECK_FALSE(rd.getModulatedTextureProgram().isValid());
    CHECK_NOTHROW(rd.beginFrame());
    CHECK_NOTHROW(rd.endFrame());
    CHECK_NOTHROW(rd.commit_frame());
    CHECK_NOTHROW(rd.advanceFrame());

    rd.shutdown();
    CHECK_FALSE(rd.getRuntimeInfo().shaderReady);
    CHECK_FALSE(rd.getModulatedTextureProgram().isValid());
    CHECK_NOTHROW(rd.beginFrame());
    CHECK_NOTHROW(rd.endFrame());
    CHECK_NOTHROW(rd.commit_frame());
    CHECK_NOTHROW(rd.advanceFrame());

    BgfxDeviceCore core;
    CHECK_NOTHROW(core.beginFrame());
    CHECK_NOTHROW(core.endFrame());
    CHECK_NOTHROW(core.commit_frame());
}

TEST_CASE("RTTManager::construct with device") {
    BgfxRenderDevice rd;
    RTTManager mgr(rd);
    (void)mgr;
}

TEST_CASE("EmbeddedShaders::DX11 VS binary present") {
    CHECK(kEmbeddedVS_SpriteSize > 0);
    CHECK(static_cast<const void*>(kEmbeddedVS_Sprite) != nullptr);
}

TEST_CASE("EmbeddedShaders::DX11 FS binary present") {
    CHECK(kEmbeddedFS_TextureSize > 0);
    CHECK(static_cast<const void*>(kEmbeddedFS_Texture) != nullptr);
}

TEST_CASE("Render: TTF load rejects missing file without GPU") {
    BgfxRenderDevice rd;
    // loadTTF performs FreeType initialization and file open before any
    // bgfx texture upload; a missing path must fail cleanly.
    CHECK_FALSE(rd.loadTTF("__missing_font__.ttf", 24.0f));
}

TEST_CASE("Render: NullRenderDevice loadTTF is a safe no-op") {
    NullRenderDevice rd;
    CHECK_FALSE(rd.loadTTF("any.ttf", 24.0f));
}

TEST_CASE("Render: embedded GL/Metal shaders are valid bgfx binaries") {
    // bgfx compiled shader binaries start with the shader magic: 0x0B485346
    // for fragment shaders and 0x0B485356 for vertex shaders.
    constexpr uint32_t kShaderMagicMask = 0xFFFFFF00u;
    constexpr uint32_t kShaderMagicBase = 0x0B485300u;
    struct Entry { const uint8_t* data; size_t size; };
    const Entry gl[] = {
        { kEmbeddedGL_vs_sprite,        kEmbeddedGL_vs_sprite_size },
        { kEmbeddedGL_vs_fullscreen,    kEmbeddedGL_vs_fullscreen_size },
        { kEmbeddedGL_stretch_blt_vs,   kEmbeddedGL_stretch_blt_vs_size },
        { kEmbeddedGL_affine_blt_vs,    kEmbeddedGL_affine_blt_vs_size },
        { kEmbeddedGL_fs_texture,       kEmbeddedGL_fs_texture_size },
        { kEmbeddedGL_fs_blend,         kEmbeddedGL_fs_blend_size },
        { kEmbeddedGL_fs_transition,    kEmbeddedGL_fs_transition_size },
        { kEmbeddedGL_fs_vfx,           kEmbeddedGL_fs_vfx_size },
        { kEmbeddedGL_stretch_blt_fs,   kEmbeddedGL_stretch_blt_fs_size },
        { kEmbeddedGL_affine_blt_fs,    kEmbeddedGL_affine_blt_fs_size },
    };
    for (const auto& entry : gl) {
        REQUIRE(entry.data != nullptr);
        CHECK(entry.size > 16);
        if (entry.size >= 4) {
            uint32_t magic = 0;
            std::memcpy(&magic, entry.data, 4);
            CHECK((magic & kShaderMagicMask) == kShaderMagicBase);
        }
    }
    const Entry metal[] = {
        { kEmbeddedMetal_vs_sprite,      kEmbeddedMetal_vs_sprite_size },
        { kEmbeddedMetal_vs_fullscreen,  kEmbeddedMetal_vs_fullscreen_size },
        { kEmbeddedMetal_stretch_blt_vs, kEmbeddedMetal_stretch_blt_vs_size },
        { kEmbeddedMetal_affine_blt_vs,  kEmbeddedMetal_affine_blt_vs_size },
        { kEmbeddedMetal_fs_texture,     kEmbeddedMetal_fs_texture_size },
        { kEmbeddedMetal_fs_blend,       kEmbeddedMetal_fs_blend_size },
        { kEmbeddedMetal_fs_transition,  kEmbeddedMetal_fs_transition_size },
        { kEmbeddedMetal_fs_vfx,         kEmbeddedMetal_fs_vfx_size },
        { kEmbeddedMetal_stretch_blt_fs, kEmbeddedMetal_stretch_blt_fs_size },
        { kEmbeddedMetal_affine_blt_fs,  kEmbeddedMetal_affine_blt_fs_size },
    };
    for (const auto& entry : metal) {
        REQUIRE(entry.data != nullptr);
        CHECK(entry.size > 16);
        if (entry.size >= 4) {
            uint32_t magic = 0;
            std::memcpy(&magic, entry.data, 4);
            CHECK((magic & kShaderMagicMask) == kShaderMagicBase);
        }
    }
}

TEST_CASE("Render: GL effect shaders match C++ uniform and sampler names") {
    // bgfx binds uniforms and samplers by name; the embedded GLSL must declare
    // the same names the engine creates at runtime.
    const auto has = [](const uint8_t* data, size_t size, const char* needle) {
        if (!data || !needle) return false;
        return std::string(reinterpret_cast<const char*>(data), size)
                   .find(needle) != std::string::npos;
    };
    CHECK(has(kEmbeddedGL_fs_blend, kEmbeddedGL_fs_blend_size, "BlendParams"));
    CHECK(has(kEmbeddedGL_fs_blend, kEmbeddedGL_fs_blend_size, "s_texture"));
    CHECK(has(kEmbeddedGL_fs_blend, kEmbeddedGL_fs_blend_size, "s_texture1"));
    CHECK(has(kEmbeddedGL_fs_vfx, kEmbeddedGL_fs_vfx_size, "VFXParams"));
    CHECK(has(kEmbeddedGL_fs_vfx, kEmbeddedGL_fs_vfx_size, "s_texture"));
    CHECK(has(kEmbeddedGL_fs_transition, kEmbeddedGL_fs_transition_size, "TransParams"));
    CHECK(has(kEmbeddedGL_fs_transition, kEmbeddedGL_fs_transition_size, "s_texture1"));
    CHECK(has(kEmbeddedGL_fs_transition, kEmbeddedGL_fs_transition_size, "s_texture2"));
    CHECK(has(kEmbeddedGL_stretch_blt_fs, kEmbeddedGL_stretch_blt_fs_size, "StretchParams"));
}

namespace {
std::vector<uint64_t> u27ResourceVector(const RenderResourceCounts& value) {
    return {value.dynamicIndexBuffers, value.dynamicVertexBuffers, value.frameBuffers,
            value.indexBuffers, value.occlusionQueries, value.programs, value.shaders,
            value.textures, value.uniforms, value.vertexBuffers, value.vertexLayouts};
}
void u27CheckUnavailable(const RenderSnapshot& value) {
    CHECK_FALSE(value.contextInitialized);
    CHECK_FALSE(value.renderingAvailable);
    CHECK_FALSE(value.resourceCountsAvailable);
    CHECK(value.backendKind == RenderBackendKind::Unknown);
    CHECK(value.contextGeneration == 0);
    CHECK(value.captureSubmissionFrame == 0);
    CHECK(u27ResourceVector(value.resources) == std::vector<uint64_t>(11, 0));
    CHECK_FALSE(value.screenshotOwnershipComplete);
    CHECK_FALSE(value.screenshotReadbackTrackingSupported);
    CHECK(value.screenshotReadbacksOutstanding == 0);
    CHECK(value.screenshots.waiting == 0);
    CHECK(value.screenshots.submitted == 0);
    CHECK(value.screenshots.terminal == 0);
    CHECK(value.screenshots.reservedBytes == 0);
    CHECK(value.screenshots.pngBytes == 0);
}
}

TEST_CASE("U27 renderer snapshot: Null remains unsupported across its no-op lifecycle") {
    NullRenderDevice device;
    const IRenderDevice& api = device;
    for (int stage = 0; stage != 3; ++stage) {
        const auto first = api.getSnapshot();
        const auto second = api.getSnapshot();
        CHECK_FALSE(first.supported);
        CHECK_FALSE(second.supported);
        CHECK_FALSE(first.screenshots.supported);
        CHECK_FALSE(second.screenshots.supported);
        u27CheckUnavailable(first);
        u27CheckUnavailable(second);
        CHECK(first.backendName.empty());
        CHECK(second.backendName.empty());
        if (stage == 0) {
            REQUIRE(device.init(nullptr, 3, 5));
            CHECK(device.getBackbufferWidth() == 3);
            CHECK(device.getBackbufferHeight() == 5);
            device.beginFrame();
            device.endFrame();
        } else {
            device.beginShutdown();
            device.shutdown();
        }
    }
}

TEST_CASE("U27 renderer snapshot: concrete uninitialized reads do not create or advance a context") {
    BgfxRenderDevice device;
    const IRenderDevice& api = device;
    for (int stage = 0; stage != 4; ++stage) {
        const auto before = device.getRuntimeInfo();
        const auto first = api.getSnapshot();
        const auto second = api.getSnapshot();
        CHECK(first.supported);
        CHECK(second.supported);
        CHECK(first.screenshots.supported);
        CHECK(second.screenshots.supported);
        CHECK(first.backendName == "bgfx");
        CHECK(second.backendName == first.backendName);
        u27CheckUnavailable(first);
        u27CheckUnavailable(second);
        CHECK_FALSE(device.isInitialized());
        const auto after = device.getRuntimeInfo();
        CHECK(after.backendName == before.backendName);
        CHECK(after.width == before.width);
        CHECK(after.height == before.height);
        CHECK(after.viewCount == before.viewCount);
        CHECK(after.shaderReady == before.shaderReady);
        CHECK_FALSE(after.shaderReady);
        if (stage == 0) {
            // Invalid dimensions return before bgfx/Core initialization.
            REQUIRE_FALSE(device.init(nullptr, 0, 0));
        } else if (stage == 1) {
            device.beginFrame();
            device.commit_frame();
            device.advanceFrame();
            device.beginShutdown();
        } else {
            device.shutdown();
        }
    }
}
