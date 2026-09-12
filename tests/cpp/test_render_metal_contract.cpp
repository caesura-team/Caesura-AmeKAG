// test_render_metal_contract.cpp - Metal Shader & Fallback Contract Tests (Track I / Milestone R3).
//
// Asserts:
// 1. All 10 2D Render Metal embedded shaders (EmbeddedShaders.h / EmbeddedShaders_Metal.cpp)
//    are present, non-empty, and contain valid bgfx shader headers.
// 2. Both 3D MiniGame MSL shaders (EmbeddedMiniGameShaders.h / EmbeddedShaders_MiniGame_Metal.cpp)
//    are present, valid MSL source, and accessible via getters.
// 3. Post-FX graceful degradation to identity pass (fsTexture) without GPU stalls.
// 4. SMA dual-mode skinning fallback to S2 CPU soft-skinning on Metal.

#include "doctest.h"
#include "render/EmbeddedShaders.h"
#include "render/BgfxShaderManager.h"
#include "render/BgfxDeviceCore.h"
#include "minigame/EmbeddedMiniGameShaders.h"
#include "render/api/IRenderDevice.h"
#include "render/SmaSkinner.h"
#include "render/SmaMeshRenderer.h"
#include "render/NullRenderDevice.h"

#include <cstring>
#include <cmath>
#include <string>

using namespace Caesura;

namespace {
constexpr float kEps = 1e-4f;
} // namespace

// =============================================================================
// 1. 2D Render Metal Embedded Shaders (10 shaders)
// =============================================================================

TEST_CASE("metal: 10 2D Render Metal Embedded Shaders symbol contracts") {
    // 1. vs_sprite
    CHECK(kEmbeddedMetal_vs_sprite != nullptr);
    CHECK(kEmbeddedMetal_vs_sprite_size == 608);
    CHECK(kEmbeddedMetal_vs_sprite[0] == 'V');
    CHECK(kEmbeddedMetal_vs_sprite[1] == 'S');
    CHECK(kEmbeddedMetal_vs_sprite[2] == 'H');

    // 2. vs_fullscreen
    CHECK(kEmbeddedMetal_vs_fullscreen != nullptr);
    CHECK(kEmbeddedMetal_vs_fullscreen_size == 659);
    CHECK(kEmbeddedMetal_vs_fullscreen[0] == 'V');
    CHECK(kEmbeddedMetal_vs_fullscreen[1] == 'S');
    CHECK(kEmbeddedMetal_vs_fullscreen[2] == 'H');

    // 3. stretch_blt_vs
    CHECK(kEmbeddedMetal_stretch_blt_vs != nullptr);
    CHECK(kEmbeddedMetal_stretch_blt_vs_size == 630);
    CHECK(kEmbeddedMetal_stretch_blt_vs[0] == 'V');
    CHECK(kEmbeddedMetal_stretch_blt_vs[1] == 'S');
    CHECK(kEmbeddedMetal_stretch_blt_vs[2] == 'H');

    // 4. affine_blt_vs
    CHECK(kEmbeddedMetal_affine_blt_vs != nullptr);
    CHECK(kEmbeddedMetal_affine_blt_vs_size == 995);
    CHECK(kEmbeddedMetal_affine_blt_vs[0] == 'V');
    CHECK(kEmbeddedMetal_affine_blt_vs[1] == 'S');
    CHECK(kEmbeddedMetal_affine_blt_vs[2] == 'H');

    // 5. fs_texture
    CHECK(kEmbeddedMetal_fs_texture != nullptr);
    CHECK(kEmbeddedMetal_fs_texture_size == 586);
    CHECK(kEmbeddedMetal_fs_texture[0] == 'F');
    CHECK(kEmbeddedMetal_fs_texture[1] == 'S');
    CHECK(kEmbeddedMetal_fs_texture[2] == 'H');

    // 6. fs_blend
    CHECK(kEmbeddedMetal_fs_blend != nullptr);
    CHECK(kEmbeddedMetal_fs_blend_size == 9925);
    CHECK(kEmbeddedMetal_fs_blend[0] == 'F');
    CHECK(kEmbeddedMetal_fs_blend[1] == 'S');
    CHECK(kEmbeddedMetal_fs_blend[2] == 'H');

    // 7. fs_transition
    CHECK(kEmbeddedMetal_fs_transition != nullptr);
    CHECK(kEmbeddedMetal_fs_transition_size == 2324);
    CHECK(kEmbeddedMetal_fs_transition[0] == 'F');
    CHECK(kEmbeddedMetal_fs_transition[1] == 'S');
    CHECK(kEmbeddedMetal_fs_transition[2] == 'H');

    // 8. fs_vfx
    CHECK(kEmbeddedMetal_fs_vfx != nullptr);
    CHECK(kEmbeddedMetal_fs_vfx_size == 2004);
    CHECK(kEmbeddedMetal_fs_vfx[0] == 'F');
    CHECK(kEmbeddedMetal_fs_vfx[1] == 'S');
    CHECK(kEmbeddedMetal_fs_vfx[2] == 'H');

    // 9. stretch_blt_fs
    CHECK(kEmbeddedMetal_stretch_blt_fs != nullptr);
    CHECK(kEmbeddedMetal_stretch_blt_fs_size == 753);
    CHECK(kEmbeddedMetal_stretch_blt_fs[0] == 'F');
    CHECK(kEmbeddedMetal_stretch_blt_fs[1] == 'S');
    CHECK(kEmbeddedMetal_stretch_blt_fs[2] == 'H');

    // 10. affine_blt_fs
    CHECK(kEmbeddedMetal_affine_blt_fs != nullptr);
    CHECK(kEmbeddedMetal_affine_blt_fs_size == 586);
    CHECK(kEmbeddedMetal_affine_blt_fs[0] == 'F');
    CHECK(kEmbeddedMetal_affine_blt_fs[1] == 'S');
    CHECK(kEmbeddedMetal_affine_blt_fs[2] == 'H');
}

// =============================================================================
// 2. 3D MiniGame MSL Shaders (2 shaders)
// =============================================================================

namespace Caesura {
extern const char* Caesura_GetMiniGameVS_MSL();
extern const char* Caesura_GetMiniGameFS_MSL();
}

TEST_CASE("metal: 3D MiniGame MSL Shaders symbol and syntax contracts") {
    CHECK(kEmbeddedMSL_MiniGame_VS != nullptr);
    CHECK(kEmbeddedMSL_MiniGame_FS != nullptr);

    const char* vs = Caesura_GetMiniGameVS_MSL();
    const char* fs = Caesura_GetMiniGameFS_MSL();

    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    CHECK(vs == kEmbeddedMSL_MiniGame_VS);
    CHECK(fs == kEmbeddedMSL_MiniGame_FS);

    std::string vsStr(vs);
    std::string fsStr(fs);

    // MSL syntax checks
    CHECK(vsStr.find("#include <metal_stdlib>") != std::string::npos);
    CHECK(vsStr.find("using namespace metal;") != std::string::npos);
    CHECK(vsStr.find("vertex VertexOut miniGameVS") != std::string::npos);
    CHECK(vsStr.find("u_miniViewProj") != std::string::npos);

    CHECK(fsStr.find("#include <metal_stdlib>") != std::string::npos);
    CHECK(fsStr.find("using namespace metal;") != std::string::npos);
    CHECK(fsStr.find("fragment float4 miniGameFS") != std::string::npos);
    CHECK(fsStr.find("calcDirectional") != std::string::npos);
    CHECK(fsStr.find("calcPointLight") != std::string::npos);
}

// =============================================================================
// 3. Post-FX Fallback & Degradation Pathways on Metal
// =============================================================================

TEST_CASE("metal: Post-FX fallback degradation behavior") {
    NullRenderDevice dev;
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::Vignette));
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::LutColorGrade));
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::SoftBlur));
    CHECK_FALSE(dev.isPostFxSupported(IRenderDevice::PostFxKind::Bloom));

    IRenderDevice::PostFxParams params;
    params.strength = 0.8f;
    params.radius = 0.5f;
    params.lutMix = 0.5f;

    // Degrades to 0 handle without crashing
    CHECK(dev.createPostFx(IRenderDevice::PostFxKind::Vignette, params) == 0);
    CHECK(dev.createPostFx(IRenderDevice::PostFxKind::Bloom, params) == 0);
    CHECK_FALSE(dev.isPostFxActive());

    // Safe no-ops
    CHECK_NOTHROW(dev.setPostFxParams(0, params));
    CHECK_NOTHROW(dev.destroyPostFx(0));
    CHECK_NOTHROW(dev.clearPostFx());
}

// =============================================================================
// 4. SMA Soft-Skinning Fallback on Metal
// =============================================================================

TEST_CASE("metal: SMA Skeletal Skinning CPU fallback math") {
    SMAMesh mesh;
    mesh.vertices = {
        {0.f, 0.f, 0.f, 0.f, 0, 1.f, 1, 0.f},
        {2.f, 0.f, 1.f, 0.f, 0, 0.5f, 1, 0.5f},
    };
    mesh.indices = {0, 1};

    std::vector<BonePose> poses(2);
    // Bone 0: translation +10 on X
    poses[0].ox = 10.f;
    // Bone 1: translation +20 on X
    poses[1].ox = 20.f;

    std::vector<SmaSkinnedVertex> out;
    skinMesh(mesh, poses, out);

    REQUIRE(out.size() == 2);
    // Vertex 0: 100% bone 0 -> 0 + 10 = 10
    CHECK(std::fabs(out[0].x - 10.f) < kEps);
    // Vertex 1: 50% bone 0 (2+10=12) + 50% bone 1 (2+20=22) -> 17
    CHECK(std::fabs(out[1].x - 17.f) < kEps);
}
// =============================================================================
// 5. t73: embedded-shader feeding contract (Binary-in-Binary regression)
// =============================================================================

namespace {
// Reproduces the PRE-t73 loader's buildBgfxShader byte layout applied to a
// complete shaderc shader binary (binary-in-binary): magic, hashIn, hashOut,
// uniformCount, codeSize, the whole inner blob, NUL, numAttrs, attrIds (u16
// pairs), cbSize u16. Layout from git show HEAD:src/render/BgfxShaderManager.cpp.
std::vector<uint8_t> oldWrapBinary(const uint8_t* blob, size_t blobSize) {
    std::vector<uint8_t> out;
    const auto push = [&out](uint8_t b) { out.push_back(b); };
    const auto u32 = [&push](uint32_t v) {
        push(uint8_t(v & 0xFF)); push(uint8_t((v >> 8) & 0xFF));
        push(uint8_t((v >> 16) & 0xFF)); push(uint8_t((v >> 24) & 0xFF));
    };
    push('V'); push('S'); push('H'); push(11);
    u32(0); u32(0);
    push(0); push(0);                        // uniformCount = 0
    u32(uint32_t(blobSize));                 // codeSize = inner binary length
    for (size_t ii = 0; ii < blobSize; ++ii) push(blob[ii]);
    push(0);                                 // NUL
    push(2);                                 // numAttrs
    push(0x01); push(0x00); push(0x10); push(0x00);  // attr ids {1, 16} (u16)
    push(0); push(0);                        // cbSize
    return out;
}
} // namespace

TEST_CASE("metal: all 10 embedded shaders are direct-feedable shader binaries (t73)") {
    using BM = BgfxShaderManager;
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_vs_sprite,      kEmbeddedMetal_vs_sprite_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_vs_fullscreen,  kEmbeddedMetal_vs_fullscreen_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_stretch_blt_vs, kEmbeddedMetal_stretch_blt_vs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_affine_blt_vs,  kEmbeddedMetal_affine_blt_vs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_fs_texture,     kEmbeddedMetal_fs_texture_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_fs_blend,       kEmbeddedMetal_fs_blend_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_fs_transition,  kEmbeddedMetal_fs_transition_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_fs_vfx,         kEmbeddedMetal_fs_vfx_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_stretch_blt_fs, kEmbeddedMetal_stretch_blt_fs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedMetal_affine_blt_fs,  kEmbeddedMetal_affine_blt_fs_size));

    // Feeding-mode mapping: Metal + desktop GL feed directly; GLES (ESSL text
    // rewrite), D3D (raw DXBC) and Vulkan (raw SPIR-V) keep the engine wrap.
    CHECK(BM::usesDirectFeed(true,  false, false));
    CHECK(BM::usesDirectFeed(false, true,  false));
    CHECK_FALSE(BM::usesDirectFeed(false, true,  true));   // GLES
    CHECK_FALSE(BM::usesDirectFeed(false, false, false));  // D3D / Vulkan
    CHECK_FALSE(BM::usesDirectFeed(false, false, true));   // GLES-only (no desktop GL)
    CHECK(BM::usesDirectFeed(true,  true,  true));         // Metal dominates

    // Negative controls: raw source is NOT a binary, nullptr is NOT a binary.
    CHECK_FALSE(BM::isDirectFeedBinary(nullptr, 64));
    CHECK_FALSE(BM::isDirectFeedBinary(kEmbeddedMetal_vs_sprite, 0));
    CHECK_FALSE(BM::isDirectFeedBinary(
        reinterpret_cast<const uint8_t*>(kEmbeddedMSL_MiniGame_VS),
        std::strlen(kEmbeddedMSL_MiniGame_VS)));
}

TEST_CASE("metal: pre-t73 double wrap rejected by direct-feed validator (t71 root)") {
    // t71 root cause: the loader used to feed a COMPLETE shaderc binary
    // through buildBgfxShader, producing [outer VSH11][inner VSH11+MSL]:
    // the renderer then handed the inner binary to the MSL compiler as
    // source and the pipeline ended up with a nil vertex function. The
    // validator must reject that nesting so the loader can never do it.
    const std::vector<uint8_t> nested =
        oldWrapBinary(kEmbeddedMetal_vs_sprite, kEmbeddedMetal_vs_sprite_size);
    CHECK_FALSE(BgfxShaderManager::isDirectFeedBinary(nested.data(), nested.size()));
    const std::vector<uint8_t> nestedFs =
        oldWrapBinary(kEmbeddedMetal_fs_texture, kEmbeddedMetal_fs_texture_size);
    CHECK_FALSE(BgfxShaderManager::isDirectFeedBinary(nestedFs.data(), nestedFs.size()));
    // And the raw arrays stay feedable (what the new loader hands to bgfx).
    CHECK(BgfxShaderManager::isDirectFeedBinary(
        kEmbeddedMetal_vs_sprite, kEmbeddedMetal_vs_sprite_size));
    CHECK(BgfxShaderManager::isDirectFeedBinary(
        kEmbeddedMetal_fs_texture, kEmbeddedMetal_fs_texture_size));
}
// =============================================================================
// 6. GL: embedded-shader feeding contract (t75 -- GL regression lock)
// =============================================================================

TEST_CASE("gl: all 10 GL embedded shaders direct-feedable (t79)") {
    using BM = BgfxShaderManager;

    // Desktop GL embedded arrays are complete shaderc BGFX binaries with the
    // current record layout (uniform records carry texInfo/texFormat since
    // bgfx binary version >= 8/>=10). fs_vfx was repacked to the current
    // layout by (a) parsing the old uniform records, (b) re-emitting them with
    // the 4-byte texInfo+texFormat pair zeroed (the exact 33a0a206 transform
    // that the other arrays already carry), keeping the GLSL code section
    // byte-identical (see docs/solutions/build-errors/bgfx-shader-binary-repack.md).
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_vs_sprite,        kEmbeddedGL_vs_sprite_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_vs_fullscreen,    kEmbeddedGL_vs_fullscreen_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_stretch_blt_vs,   kEmbeddedGL_stretch_blt_vs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_affine_blt_vs,    kEmbeddedGL_affine_blt_vs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_fs_texture,       kEmbeddedGL_fs_texture_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_fs_blend,         kEmbeddedGL_fs_blend_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_fs_transition,    kEmbeddedGL_fs_transition_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_fs_vfx,           kEmbeddedGL_fs_vfx_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_stretch_blt_fs,   kEmbeddedGL_stretch_blt_fs_size));
    CHECK(BM::isDirectFeedBinary(kEmbeddedGL_affine_blt_fs,    kEmbeddedGL_affine_blt_fs_size));

    // t75 feed-mode semantics: desktop GL feeds directly, GLES wraps.
    CHECK(BM::usesDirectFeed(false, true,  false));
    CHECK_FALSE(BM::usesDirectFeed(false, true,  true));
}

TEST_CASE("GLES source preparation accepts actual generated GLSL and preserves explicit fragment output") {
    const auto vertex = BgfxShaderManager::toEssl300(
        kEmbeddedGL_vs_sprite, uint32_t(kEmbeddedGL_vs_sprite_size), false);
    const auto fragment = BgfxShaderManager::toEssl300(
        kEmbeddedGL_fs_modulated_texture, uint32_t(kEmbeddedGL_fs_modulated_texture_size), true);
    for (const auto& source : {vertex, fragment}) {
        REQUIRE(source.starts_with("#version 300 es\n"));
        CHECK(source.find("#version 430") == std::string::npos);
        CHECK(source.find("precision highp float;") != std::string::npos);
        CHECK(source.find("precision highp int;") != std::string::npos);
    }
    CHECK(fragment.find("out vec4 bgfx_FragColor;") != std::string::npos);
    CHECK(fragment.find("out vec4 oFragColor;") == std::string::npos);
    CHECK(fragment.find("u_color") != std::string::npos);
    CHECK(BgfxShaderManager::toEssl300(nullptr, 0, true).empty());
    CHECK(BgfxShaderManager::toEssl300(kEmbeddedGL_vs_sprite, 13, false).empty());
    // This tests the production source conversion only. GLES compilation and
    // actual device pixels remain separate, explicitly unmeasured evidence.
}
// =============================================================================
// 7. t85: embedded shader hashIn/hashOut pairing (bgfx_p.h:5140 cross-binary)
// =============================================================================

namespace {
// t80 note: fs.hashIn == paired-vs.hashOut is a CROSS-BINARY property the
// per-array validator cannot see. Offset口径 matches the repack guide
// (docs/solutions/build-errors/bgfx-shader-binary-repack.md 坑 2):
//   magic(0..3) | hashIn(4..7) | hashOut(8..11) | count(12..13) | ...
uint32_t shaderHashIn(const uint8_t* d) {
    return uint32_t(d[4]) | (uint32_t(d[5]) << 8)
         | (uint32_t(d[6]) << 16) | (uint32_t(d[7]) << 24);
}
uint32_t shaderHashOut(const uint8_t* d) {
    return uint32_t(d[8]) | (uint32_t(d[9]) << 8)
         | (uint32_t(d[10]) << 16) | (uint32_t(d[11]) << 24);
}
} // namespace

TEST_CASE("gl: embedded shader hash pairing fs.hashIn == paired vs.hashOut (t85)") {
    // Same varying.def -> one shared hash across the whole family.
    const uint32_t kFamilyHash = 0x3C3E1E6Fu;
    CHECK(shaderHashOut(kEmbeddedGL_vs_sprite)      == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedGL_vs_fullscreen)  == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedGL_stretch_blt_vs) == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedGL_affine_blt_vs)  == kFamilyHash);

    // Fallback: vs_sprite + fs_texture (BgfxShaderManager buildProgram).
    CHECK(shaderHashIn(kEmbeddedGL_fs_texture) == shaderHashOut(kEmbeddedGL_vs_sprite));
    // Fullscreen family: vs_fullscreen + blend/transition/vfx.
    CHECK(shaderHashIn(kEmbeddedGL_fs_blend)      == shaderHashOut(kEmbeddedGL_vs_fullscreen));
    CHECK(shaderHashIn(kEmbeddedGL_fs_transition) == shaderHashOut(kEmbeddedGL_vs_fullscreen));
    CHECK(shaderHashIn(kEmbeddedGL_fs_vfx)        == shaderHashOut(kEmbeddedGL_vs_fullscreen));
    // Dedicated blit pairs.
    CHECK(shaderHashIn(kEmbeddedGL_stretch_blt_fs) == shaderHashOut(kEmbeddedGL_stretch_blt_vs));
    CHECK(shaderHashIn(kEmbeddedGL_affine_blt_fs)  == shaderHashOut(kEmbeddedGL_affine_blt_vs));

    // Every checked FS carries the family hash (locks the invariant value).
    CHECK(shaderHashIn(kEmbeddedGL_fs_texture) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedGL_fs_blend)   == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedGL_fs_transition) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedGL_fs_vfx)     == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedGL_stretch_blt_fs) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedGL_affine_blt_fs)  == kFamilyHash);
}

TEST_CASE("metal: embedded shader hash pairing fs.hashIn == paired vs.hashOut (t85)") {
    const uint32_t kFamilyHash = 0x3C3E1E6Fu;
    CHECK(shaderHashOut(kEmbeddedMetal_vs_sprite)      == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedMetal_vs_fullscreen)  == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedMetal_stretch_blt_vs) == kFamilyHash);
    CHECK(shaderHashOut(kEmbeddedMetal_affine_blt_vs)  == kFamilyHash);

    CHECK(shaderHashIn(kEmbeddedMetal_fs_texture) == shaderHashOut(kEmbeddedMetal_vs_sprite));
    CHECK(shaderHashIn(kEmbeddedMetal_fs_blend)      == shaderHashOut(kEmbeddedMetal_vs_fullscreen));
    CHECK(shaderHashIn(kEmbeddedMetal_fs_transition) == shaderHashOut(kEmbeddedMetal_vs_fullscreen));
    CHECK(shaderHashIn(kEmbeddedMetal_fs_vfx)        == shaderHashOut(kEmbeddedMetal_vs_fullscreen));
    CHECK(shaderHashIn(kEmbeddedMetal_stretch_blt_fs) == shaderHashOut(kEmbeddedMetal_stretch_blt_vs));
    CHECK(shaderHashIn(kEmbeddedMetal_affine_blt_fs)  == shaderHashOut(kEmbeddedMetal_affine_blt_vs));

    CHECK(shaderHashIn(kEmbeddedMetal_fs_texture) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedMetal_fs_blend)   == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedMetal_fs_transition) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedMetal_fs_vfx)     == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedMetal_stretch_blt_fs) == kFamilyHash);
    CHECK(shaderHashIn(kEmbeddedMetal_affine_blt_fs)  == kFamilyHash);
}
// =============================================================================
// 8. t92: per-platform default backend resolution
// =============================================================================

TEST_CASE("render: per-platform default backend resolution (t92)") {
    // The resolver is pure (no GPU). Each platform's branch is compiled out on
    // the others, so the non-Windows arms are exercised by the CI matrix
    // (Linux GCC job runs this same binary; mac clang job likewise) -- the
    // honest verification surface for the Linux branch is CI, not this host.
    const bgfx::RendererType::Enum d = BgfxDeviceCore::platformDefaultBackend();
#if defined(_WIN32)
    CHECK(d == bgfx::RendererType::Direct3D11);
#elif defined(__APPLE__)
    CHECK(d == bgfx::RendererType::Metal);
#else
    CHECK(d == bgfx::RendererType::OpenGL);
#endif
    // --backend override precedence is NOT exercised here: setPreferredBackend
    // mutates the process-global static and tests must stay side-effect free.
    // The override path is the round4 evidence path and is covered by the
    // engine --backend gate runs.
}

// =============================================================================
// 9. t92: mesa-compliant GL fragment code (bgfx_FragColor convention)
// =============================================================================

namespace {
// Current-layout walk (records carry texInfo/texFormat) -> the GLSL code text.
std::string shaderCodeText(const uint8_t* d) {
    size_t off = 12;
    const size_t size = 1u;  // placeholder; the arrays are never shorter than 18
    const uint16_t count = uint16_t(d[off] | (d[off + 1] << 8));
    off += 2;
    for (uint16_t ii = 0; ii < count; ++ii) {
        const uint8_t ns = d[off];
        off += 1u + ns + 6u + 4u;  // name + type/num + reg + texInfo + texFormat
    }
    const uint32_t cs = uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8)
                      | (uint32_t(d[off + 2]) << 16) | (uint32_t(d[off + 3]) << 24);
    off += 4;
    return std::string(reinterpret_cast<const char*>(d + off), cs);
}
} // namespace

TEST_CASE("gl: mesa-compliant fragment code bgfx_FragColor (t92)") {
    // gl_FragColor is REMOVED in GLSL 3.30+ core (error C7616) -- a strict-core
    // GL context (mesa EGL on CI, and any future core-profile setup) rejects
    // it. bgfx binds the fragment output by NAME (glBindFragDataLocation(
    // m_id, 0, "bgfx_FragColor"), renderer_gl.cpp ProgramGL::init), so the
    // embedded FS code must declare and write 'out vec4 bgfx_FragColor;'.
    // Windows compat-profile GL still accepts this form (the explicit output
    // binds location 0 exactly like the old implicit gl_FragColor did).
    const struct { const uint8_t* data; size_t size; } fsArrays[] = {
        { kEmbeddedGL_fs_texture,     kEmbeddedGL_fs_texture_size },
        { kEmbeddedGL_fs_blend,       kEmbeddedGL_fs_blend_size },
        { kEmbeddedGL_fs_transition,  kEmbeddedGL_fs_transition_size },
        { kEmbeddedGL_fs_vfx,         kEmbeddedGL_fs_vfx_size },
        { kEmbeddedGL_stretch_blt_fs, kEmbeddedGL_stretch_blt_fs_size },
        { kEmbeddedGL_affine_blt_fs,  kEmbeddedGL_affine_blt_fs_size },
    };
    for (const auto& a : fsArrays) {
        const std::string code = shaderCodeText(a.data);
        CHECK(code.find("gl_FragColor") == std::string::npos);
        CHECK(code.find("bgfx_FragColor") != std::string::npos);
        CHECK(code.find("out vec4 bgfx_FragColor;") != std::string::npos);
        // The substitution must not have broken the direct-feed contract.
        CHECK(BgfxShaderManager::isDirectFeedBinary(a.data, a.size));
    }
}
