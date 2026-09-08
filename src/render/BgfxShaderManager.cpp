#include "BgfxShaderManager.h"
#include "EmbeddedShaders.h"
#include "ShaderCache.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include <bx/bx.h>
#include <bx/readerwriter.h>
#include <bx/error.h>
#include <cstdio>
#include <cstring>

namespace Caesura {

bool BgfxShaderManager::testFaultsEnabled() {
#if defined(CAESURA_RENDER_TEST_FAULTS)
    return true;
#else
    return false;
#endif
}

bool BgfxShaderManager::supportsTestFault(ShaderTestFault fault) {
    switch (fault) {
    case ShaderTestFault::None: return true;
    case ShaderTestFault::FallbackMissingFragment:
    case ShaderTestFault::BlendMissingFragment:
    case ShaderTestFault::SoftBlurMissingFragment:
    case ShaderTestFault::TransitionMissingFragment:
        return testFaultsEnabled();
    default: return false;
    }
}

bool BgfxShaderManager::setTestFault(ShaderTestFault fault) {
    if (m_embeddedInit || !supportsTestFault(fault)) return false;
    m_testFault = fault;
    return true;
}

ShaderBuildReport BgfxShaderManager::buildReport() const {
    return {!coreProgramsBroken(), m_buildFailures, m_injectedFaultCount,
        m_testFault, m_failedProgramNames};
}

BgfxShaderManager::~BgfxShaderManager() {
    if (bgfx::isValid(m_fallbackProgram))    bgfx::destroy(m_fallbackProgram);
    if (bgfx::isValid(m_blendProgram))       bgfx::destroy(m_blendProgram);
    if (bgfx::isValid(m_transitionProgram))  bgfx::destroy(m_transitionProgram);
    if (bgfx::isValid(m_vfxProgram))         bgfx::destroy(m_vfxProgram);
    if (bgfx::isValid(m_stretchProgram))     bgfx::destroy(m_stretchProgram);
    if (bgfx::isValid(m_affineProgram))      bgfx::destroy(m_affineProgram);
    if (bgfx::isValid(m_texSampler))         bgfx::destroy(m_texSampler);
    if (bgfx::isValid(m_texSampler1))        bgfx::destroy(m_texSampler1);
    if (bgfx::isValid(m_texSampler2))        bgfx::destroy(m_texSampler2);
    if (bgfx::isValid(m_u_blendParams))      bgfx::destroy(m_u_blendParams);
    if (bgfx::isValid(m_u_transParams))      bgfx::destroy(m_u_transParams);
    if (bgfx::isValid(m_u_vfxParams))        bgfx::destroy(m_u_vfxParams);
    if (bgfx::isValid(m_u_stretchParams))    bgfx::destroy(m_u_stretchParams);
    if (bgfx::isValid(m_u_affineParams))     bgfx::destroy(m_u_affineParams);
    if (bgfx::isValid(m_postfxVignette))     bgfx::destroy(m_postfxVignette);
    if (bgfx::isValid(m_postfxLut))          bgfx::destroy(m_postfxLut);
    if (bgfx::isValid(m_postfxBlur))         bgfx::destroy(m_postfxBlur);
    if (bgfx::isValid(m_postfxBloom))        bgfx::destroy(m_postfxBloom);
    if (bgfx::isValid(m_postfxLut3d))        bgfx::destroy(m_postfxLut3d);
    if (bgfx::isValid(m_u_postfxParams))     bgfx::destroy(m_u_postfxParams);
}

struct ShaderUniformMetadata {
    const char* name = nullptr;
    uint8_t type = 0;
    uint8_t num = 0;
    uint16_t regIndex = 0;
    uint16_t regCount = 0;
    uint16_t constantBufferSize = 0;
};

// Track M device-day: convert GLSL 120 textual bytecode to ESSL 3.00 for
// the OpenGLES renderer (the vendored blobs are desktop GL; the device
// driver rejects attribute/varying/gl_FragColor). Text-only rewrites.
static std::string toEssl300(const uint8_t* code, uint32_t size, bool fragment) {
    // VSH11/FSH11 blobs: binary header then the GLSL text, NUL-terminated
    // before the trailing attribute/uniform metadata. Header length varies
    // (vertex vs fragment), so scan for the first printable ASCII run.
    // VSH11/FSH11 blobs: 4B magic + 8B hashes + uniform table + 4B
    // codeSize, then the GLSL text (NUL-terminated). Walk the uniform
    // records exactly like isDirectFeedBinary to locate the code segment;
    // the payload begins with plain GLSL ("in vec2"/"uniform") because
    // glsl-optimizer trims leading "#version" directives.
    uint32_t pos = 4 + 4 + 4;
    if (pos + 2 > size) return "";
    uint16_t count = static_cast<uint16_t>(code[pos] | (code[pos + 1] << 8));
    pos += 2;
    for (uint16_t ii = 0; ii < count; ++ii) {
        if (pos >= size) return "";
        uint8_t nameSize = code[pos];
        pos += 1u + nameSize;
        if (pos > size) return "";
        pos += 1 /*type*/ + 1 /*num*/ + 2 /*regIndex*/ + 2 /*regCount*/;
        if (code[3] >= 8)  pos += 2;  // texInfo
        if (code[3] >= 10) pos += 2;  // texFormat
        if (pos > size) return "";
    }
    if (pos + 4 > size) return "";
    uint32_t codeSize = static_cast<uint32_t>(
        code[pos] | (code[pos + 1] << 8) | (code[pos + 2] << 16) | (code[pos + 3] << 24));
    pos += 4;
    if (0 == codeSize || pos + codeSize > size) return "";
    uint32_t end = pos;
    while (end < size && end < pos + codeSize && code[end] != 0) ++end;
    std::string t(reinterpret_cast<const char*>(code + pos), end - pos);
    auto rep = [&t](const char* a, const char* b) {
        size_t p = 0;
        while ((p = t.find(a, p)) != std::string::npos) { t.replace(p, strlen(a), b); p += strlen(b); }
    };
    rep("#version 430 core", "#version 300 es");
    rep("#version 120", "#version 300 es");
    rep("#version 110", "#version 300 es");
    rep("#version 100", "#version 300 es");
    rep("attribute", "in");
    rep("varying", fragment ? "in" : "out");
    rep("texture2D", "texture");
    rep("textureCube", "texture");
    rep("gl_FragColor", "oFragColor");
    if (fragment) { rep("#version 300 es", "#version 300 es\nout vec4 oFragColor;"); }
    return t;
}

static bgfx::ShaderHandle buildBgfxShader(
    const uint8_t* bytecode,
    uint32_t codeSize,
    bool fragment,
    uint8_t numAttrs,
    const uint16_t* attrIds,
    const ShaderUniformMetadata* uniform = nullptr) {
    // [10.2.3] Shader safety: reject bytecode > 64 KB (SPIR-V/DXBC limit)
    if (codeSize > 65536) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxShaderManager] Shader rejected: %u bytes exceeds 64 KB limit.", codeSize);
        return BGFX_INVALID_HANDLE;
    }

    const uint16_t uniformCount = uniform ? 1 : 0;
    const uint32_t uniformSize = uniform
        ? 1 + uint32_t(std::strlen(uniform->name)) + 1 + 1 + 2 + 2 + 1 + 1 + 2
        : 0;

    const uint32_t totalSize = 4 + 4 + 4 + 2
                             + uniformSize
                             + 4 + codeSize + 1
                             + 1 + 2 * numAttrs
                             + 2;

    const bgfx::Memory* mem = bgfx::alloc(totalSize);
    if (!mem) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxShaderManager] bgfx::alloc failed for %u bytes", totalSize);
        return BGFX_INVALID_HANDLE;
    }
    bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
    bx::ErrorAssert err;

    const uint32_t magic = fragment
        ? BX_MAKEFOURCC('F', 'S', 'H', 11)
        : BX_MAKEFOURCC('V', 'S', 'H', 11);
    bx::write(&writer, magic, err);
    bx::write(&writer, uint32_t(0), err);
    bx::write(&writer, uint32_t(0), err);
    bx::write(&writer, uniformCount, err);

    if (uniform) {
        const uint8_t nameSize = uint8_t(std::strlen(uniform->name));
        bx::write(&writer, nameSize, err);
        for (uint8_t i = 0; i < nameSize; ++i)
            bx::write(&writer, uniform->name[i], err);
        bx::write(&writer, uniform->type, err);
        bx::write(&writer, uniform->num, err);
        bx::write(&writer, uniform->regIndex, err);
        bx::write(&writer, uniform->regCount, err);
        bx::write(&writer, uint8_t(0), err);
        bx::write(&writer, uint8_t(0), err);
        bx::write(&writer, uint16_t(0), err);
    }

    bx::write(&writer, codeSize, err);
    for (uint32_t i = 0; i < codeSize; ++i)
        bx::write(&writer, bytecode[i], err);
    bx::write(&writer, uint8_t(0), err);

    bx::write(&writer, numAttrs, err);
    for (uint8_t i = 0; i < numAttrs; ++i)
        bx::write(&writer, attrIds[i], err);

    bx::write(&writer, uniform ? uniform->constantBufferSize : uint16_t(0), err);

    bgfx::ShaderHandle handle = bgfx::createShader(mem);
    if (!bgfx::isValid(handle)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxShaderManager] bgfx::createShader FAILED (renderer=%s)",
                  bgfx::getRendererName(bgfx::getCaps()->rendererType));
    }
    return handle;
}


// ---------------------------------------------------------------------------
// t73: embedded-shader feeding contract (pure, GPU-free, unit-tested).
//
// The Metal / desktop-GL embedded arrays spell complete shaderc BGFX
// binaries: [VSH11|FSH11][hashIn][hashOut][uniCount][uni...][codeSize]
// [source text][0][trailing attrs/cb ignored by every parser]. The source
// text (MSL / GLSL) is what ShaderMtl::create / ShaderGL::create feed to
// newLibraryWithSource / glShaderSource. Re-wrapping these arrays inside
// buildBgfxShader nests a second header in front and the renderer hands the
// whole inner binary to the compiler as source -- the t71 macOS SIGABRT
// ("vertexFunction must not be nil") chain.
// ---------------------------------------------------------------------------

bool BgfxShaderManager::usesDirectFeed(bool isMetal, bool isGL, bool isGLES) {
    // Metal + desktop GL: direct. GLES (ESSL text rewrite path), D3D (raw
    // DXBC payload) and Vulkan (raw SPIR-V words) keep the engine wrapper.
    return isMetal || (isGL && !isGLES);
}

bool BgfxShaderManager::isDirectFeedBinary(const uint8_t* data, size_t size) {
    if (nullptr == data || size < 18) {
        return false;
    }
    const bool vsh = ('V' == data[0]);
    const bool fsh = ('F' == data[0]);
    if (!vsh && !fsh) return false;
    if ('S' != data[1] || 'H' != data[2]) return false;
    if (data[3] < 6) return false;  // BGFX binary version (>=6 => hashOut present)

    size_t pos = 12;
    const uint16_t count = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    // Walk the uniform records exactly like ShaderMtl::create / ShaderGL::create:
    // nameSize, name, type, num, regIndex u16, regCount u16,
    // + texInfo u16 (ver>=8), + texFormat u16 (ver>=10).
    for (uint16_t ii = 0; ii < count; ++ii) {
        if (pos >= size) return false;
        const uint8_t nameSize = data[pos];
        pos += 1u + nameSize;
        if (pos > size) return false;
        pos += 1 /*type*/ + 1 /*num*/ + 2 /*regIndex*/ + 2 /*regCount*/;
        if (data[3] >= 8)  pos += 2;  // texInfo
        if (data[3] >= 10) pos += 2;  // texFormat
        if (pos > size) return false;
    }
    if (pos + 4 > size) return false;
    const uint32_t codeSize = static_cast<uint32_t>(
        data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24));
    pos += 4;
    if (0 == codeSize || pos + codeSize + 1 > size) return false;
    // Source payload must NOT start with another bgfx shader binary header
    // (binary-in-binary check). Note: glsl-optimizer output strips the
    // "#version" directive (shaderc_glsl.cpp "Trim all directives"), so the
    // payload legitimately begins with "in vec2"/"out vec4"/"uniform" etc.
    if (pos + 2 < size
    &&  ('V' == data[pos] || 'F' == data[pos])
    &&  'S' == data[pos + 1]
    &&  'H' == data[pos + 2]) { return false; }
    if (0 != data[pos + codeSize]) return false;  // NUL terminator
    return true;
}

bool BgfxShaderManager::coreProgramsBroken() const {
    // CORE = the pipelines the engine cannot render anything with:
    //   - m_fallbackProgram (vsSprite+fsTexture): sprite/quad/text/blit
    //     pipeline -- every BgfxDraw blit, BgfxQuadBatch flush,
    //     TextRenderer pages, ParticleSystem and SmaMeshRenderer fallback
    //     submit this program (BgfxDraw_Blit.cpp:73-79,
    //     BgfxQuadBatch.cpp:141, TextRenderer.cpp:662/730,
    //     ParticleSystem.cpp:43/195, SmaMeshRenderer.cpp:371-408);
    //   - m_blendProgram (vsFullscreen+fsBlend): the composite/blend-mode
    //     pipeline (BgfxDraw_Effects.cpp:60-76).
    // Optional (gracefully skipped by their draw-site guards, so a failure
    // must NOT disable the renderer -- t75): transition, VFX, stretch,
    // affine, postfx stages.
    return !bgfx::isValid(m_fallbackProgram)
        || !bgfx::isValid(m_blendProgram);
}

// initEmbeddedShaders
// Picks the correct embedded bytecode (SPIR-V for Vulkan, DXBC for
// D3D11/D3D12), wraps it in a proper bgfx binary header via
// buildBgfxShader(), and registers the resulting program as the
// engine-wide fallback for 2-D quad rendering and RTT blits.

void BgfxShaderManager::initEmbeddedShaders() {
    // PER-INSTANCE guard: multiple managers (the render device plus e.g.
    // SmaMeshRenderer's own) must each build their own program/uniform
    // handles. A function-static guard here silently skipped every later
    // manager, leaving its handles invalid (SMA CPU draw was a no-op /
    // crash on real GPUs until S5).
    if (m_embeddedInit) return;
    m_embeddedInit = true;

    const bgfx::RendererType::Enum renderer = bgfx::getCaps()->rendererType;

    printf("[BgfxShaderManager] initEmbeddedShaders: renderer=%s\n",
           bgfx::getRendererName(renderer));
    // Initialize ShaderCache before registering any programs
    CompositeShaderCache::instance().init();

    struct Bytecode {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    // Per-renderer bytecode selection. SPIR-V arrays are stored as dwords.
    const bool isVulkan = renderer == bgfx::RendererType::Vulkan;
    const bool isD3D = renderer == bgfx::RendererType::Direct3D11 ||
                       renderer == bgfx::RendererType::Direct3D12;
    const bool isGL = renderer == bgfx::RendererType::OpenGL ||
                      renderer == bgfx::RendererType::OpenGLES;
    const bool isMetal = renderer == bgfx::RendererType::Metal;
    const bool isGLESv = renderer == bgfx::RendererType::OpenGLES;
    // t73: Metal + desktop GL embedded arrays are complete shaderc binaries
    // and are fed to bgfx::createShader untouched; GLES (ESSL text rewrite),
    // D3D (raw DXBC) and Vulkan (raw SPIR-V) keep the engine wrapper.
    const bool directFeed = usesDirectFeed(isMetal, isGL, isGLESv);

    Bytecode vsSprite, fsTexture, vsFullscreen, fsBlend, fsTransition, fsVfx;
    Bytecode stretchVs, stretchFs, affineVs, affineFs;
    // Round-102 post-processing full-screen PS (vignette / LUT grade /
    // soft blur / bloom). Compiled for D3D11/D3D12; other backends fall
    // back to fsTexture (identity copy) so the chain stays functional
    // (graceful degradation, no visual effect on those backends).
    Bytecode fsPostfxVignette, fsPostfxLut, fsPostfxBlur, fsPostfxBloom;
    Bytecode fsPostfxLut3d; // t214: 3D LUT stage (D3D bytecode today)

    if (isVulkan) {
        vsSprite   = { reinterpret_cast<const uint8_t*>(kEmbeddedVS_Sprite),
                       kEmbeddedVS_SpriteSize * sizeof(uint32_t) };
        fsTexture  = { reinterpret_cast<const uint8_t*>(kEmbeddedFS_Texture),
                       kEmbeddedFS_TextureSize * sizeof(uint32_t) };
        vsFullscreen = { reinterpret_cast<const uint8_t*>(kEmbeddedSPIRV_vs_fullscreen),
                         kEmbeddedSPIRV_vs_fullscreen_size * sizeof(uint32_t) };
        fsBlend    = { reinterpret_cast<const uint8_t*>(kEmbeddedSPIRV_fs_blend),
                       kEmbeddedSPIRV_fs_blend_size * sizeof(uint32_t) };
        fsTransition = { reinterpret_cast<const uint8_t*>(kEmbeddedSPIRV_fs_transition),
                         kEmbeddedSPIRV_fs_transition_size * sizeof(uint32_t) };
        fsVfx      = { reinterpret_cast<const uint8_t*>(kEmbeddedSPIRV_fs_vfx),
                       kEmbeddedSPIRV_fs_vfx_size * sizeof(uint32_t) };
    } else if (isD3D) {
        vsSprite   = { kEmbeddedDXBC_VS_Sprite, kEmbeddedDXBC_VS_Sprite_size };
        fsTexture  = { kEmbeddedDXBC_FS_Texture, kEmbeddedDXBC_FS_Texture_size };
        vsFullscreen = { kEmbeddedDXBC_vs_fullscreen, kEmbeddedDXBC_vs_fullscreen_size };
        fsBlend    = { kEmbeddedDXBC_fs_blend, kEmbeddedDXBC_fs_blend_size };
        fsTransition = { kEmbeddedDXBC_fs_transition, kEmbeddedDXBC_fs_transition_size };
        fsVfx      = { kEmbeddedDXBC_fs_vfx, kEmbeddedDXBC_fs_vfx_size };
        stretchVs  = { kEmbeddedDXBC_stretch_blt_vs, kEmbeddedDXBC_stretch_blt_vs_size };
        stretchFs  = { kEmbeddedDXBC_stretch_blt_fs, kEmbeddedDXBC_stretch_blt_fs_size };
        affineVs   = { kEmbeddedDXBC_affine_blt_vs, kEmbeddedDXBC_affine_blt_vs_size };
        affineFs   = { kEmbeddedDXBC_affine_blt_fs, kEmbeddedDXBC_affine_blt_fs_size };
        fsPostfxVignette = { kEmbeddedDXBC_fs_postfx_vignette, kEmbeddedDXBC_fs_postfx_vignette_size };
        fsPostfxLut      = { kEmbeddedDXBC_fs_postfx_lut,      kEmbeddedDXBC_fs_postfx_lut_size };
        fsPostfxBlur     = { kEmbeddedDXBC_fs_postfx_blur,     kEmbeddedDXBC_fs_postfx_blur_size };
        fsPostfxBloom    = { kEmbeddedDXBC_fs_postfx_bloom,    kEmbeddedDXBC_fs_postfx_bloom_size };
        fsPostfxLut3d    = { kEmbeddedDXBC_fs_postfx_lut3d,    kEmbeddedDXBC_fs_postfx_lut3d_size };
    } else if (isGL) {
        if (renderer == bgfx::RendererType::OpenGLES) {
            // device-day: ESSL text conversion on every GL bytecode
            auto conv = [](const uint8_t* d, uint32_t n, bool frag) -> Bytecode {
                std::string e = toEssl300(d, n, frag);
                fprintf(stderr, "[ESSL] %s first=%s\n", frag ? "fs" : "vs", e.substr(0, 120).c_str());
                if (e.find("tmpvar") != std::string::npos) fprintf(stderr, "[ESSL-TMPVAR] %s\n", e.c_str());
                auto* mem = new uint8_t[e.size()];
                memcpy(mem, e.data(), e.size());
                return { mem, e.size() };
            };
            vsSprite   = conv(kEmbeddedGL_vs_sprite,   uint32_t(kEmbeddedGL_vs_sprite_size), false);
            fsTexture  = conv(kEmbeddedGL_fs_texture,  uint32_t(kEmbeddedGL_fs_texture_size), true);
            vsFullscreen = conv(kEmbeddedGL_vs_fullscreen, uint32_t(kEmbeddedGL_vs_fullscreen_size), false);
            fsBlend    = conv(kEmbeddedGL_fs_blend,    uint32_t(kEmbeddedGL_fs_blend_size), true);
            fsTransition = conv(kEmbeddedGL_fs_transition, uint32_t(kEmbeddedGL_fs_transition_size), true);
            fsVfx      = conv(kEmbeddedGL_fs_vfx,      uint32_t(kEmbeddedGL_fs_vfx_size), true);
            stretchVs  = conv(kEmbeddedGL_stretch_blt_vs, uint32_t(kEmbeddedGL_stretch_blt_vs_size), false);
            stretchFs  = conv(kEmbeddedGL_stretch_blt_fs, uint32_t(kEmbeddedGL_stretch_blt_fs_size), true);
            affineVs   = conv(kEmbeddedGL_affine_blt_vs, uint32_t(kEmbeddedGL_affine_blt_vs_size), false);
            affineFs   = conv(kEmbeddedGL_affine_blt_fs, uint32_t(kEmbeddedGL_affine_blt_fs_size), true);
            // t216: GL postfx arrays now ship; convert for ESSL too.
            fsPostfxVignette = conv(kEmbeddedGL_fs_postfx_vignette, uint32_t(kEmbeddedGL_fs_postfx_vignette_size), true);
            fsPostfxLut      = conv(kEmbeddedGL_fs_postfx_lut,      uint32_t(kEmbeddedGL_fs_postfx_lut_size), true);
            fsPostfxBlur     = conv(kEmbeddedGL_fs_postfx_blur,     uint32_t(kEmbeddedGL_fs_postfx_blur_size), true);
            fsPostfxBloom    = conv(kEmbeddedGL_fs_postfx_bloom,    uint32_t(kEmbeddedGL_fs_postfx_bloom_size), true);
            fsPostfxLut3d    = conv(kEmbeddedGL_fs_postfx_lut3d,    uint32_t(kEmbeddedGL_fs_postfx_lut3d_size), true);
        } else {
        vsSprite   = { kEmbeddedGL_vs_sprite, kEmbeddedGL_vs_sprite_size };
        fsTexture  = { kEmbeddedGL_fs_texture, kEmbeddedGL_fs_texture_size };
        vsFullscreen = { kEmbeddedGL_vs_fullscreen, kEmbeddedGL_vs_fullscreen_size };
        fsBlend    = { kEmbeddedGL_fs_blend, kEmbeddedGL_fs_blend_size };
        fsTransition = { kEmbeddedGL_fs_transition, kEmbeddedGL_fs_transition_size };
        fsVfx      = { kEmbeddedGL_fs_vfx, kEmbeddedGL_fs_vfx_size };
        stretchVs  = { kEmbeddedGL_stretch_blt_vs, kEmbeddedGL_stretch_blt_vs_size };
        stretchFs  = { kEmbeddedGL_stretch_blt_fs, kEmbeddedGL_stretch_blt_fs_size };
        affineVs   = { kEmbeddedGL_affine_blt_vs, kEmbeddedGL_affine_blt_vs_size };
        affineFs   = { kEmbeddedGL_affine_blt_fs, kEmbeddedGL_affine_blt_fs_size };
        // t216: GL postfx arrays now ship (profile 130); real effects on GL.
        fsPostfxVignette = { kEmbeddedGL_fs_postfx_vignette, kEmbeddedGL_fs_postfx_vignette_size };
        fsPostfxLut      = { kEmbeddedGL_fs_postfx_lut,      kEmbeddedGL_fs_postfx_lut_size };
        fsPostfxBlur     = { kEmbeddedGL_fs_postfx_blur,     kEmbeddedGL_fs_postfx_blur_size };
        fsPostfxBloom    = { kEmbeddedGL_fs_postfx_bloom,    kEmbeddedGL_fs_postfx_bloom_size };
        fsPostfxLut3d    = { kEmbeddedGL_fs_postfx_lut3d,    kEmbeddedGL_fs_postfx_lut3d_size };
        }
    } else if (isMetal) {
        vsSprite   = { kEmbeddedMetal_vs_sprite, kEmbeddedMetal_vs_sprite_size };
        fsTexture  = { kEmbeddedMetal_fs_texture, kEmbeddedMetal_fs_texture_size };
        vsFullscreen = { kEmbeddedMetal_vs_fullscreen, kEmbeddedMetal_vs_fullscreen_size };
        fsBlend    = { kEmbeddedMetal_fs_blend, kEmbeddedMetal_fs_blend_size };
        fsTransition = { kEmbeddedMetal_fs_transition, kEmbeddedMetal_fs_transition_size };
        fsVfx      = { kEmbeddedMetal_fs_vfx, kEmbeddedMetal_fs_vfx_size };
        stretchVs  = { kEmbeddedMetal_stretch_blt_vs, kEmbeddedMetal_stretch_blt_vs_size };
        stretchFs  = { kEmbeddedMetal_stretch_blt_fs, kEmbeddedMetal_stretch_blt_fs_size };
        affineVs   = { kEmbeddedMetal_affine_blt_vs, kEmbeddedMetal_affine_blt_vs_size };
        affineFs   = { kEmbeddedMetal_affine_blt_fs, kEmbeddedMetal_affine_blt_fs_size };
        // 035: Metal postfx arrays now ship (shaderc osx), wire like D3D/GL.
        fsPostfxVignette = { kEmbeddedMetal_fs_postfx_vignette, kEmbeddedMetal_fs_postfx_vignette_size };
        fsPostfxLut      = { kEmbeddedMetal_fs_postfx_lut,      kEmbeddedMetal_fs_postfx_lut_size };
        fsPostfxBlur     = { kEmbeddedMetal_fs_postfx_blur,     kEmbeddedMetal_fs_postfx_blur_size };
        fsPostfxBloom    = { kEmbeddedMetal_fs_postfx_bloom,    kEmbeddedMetal_fs_postfx_bloom_size };
        fsPostfxLut3d    = { kEmbeddedMetal_fs_postfx_lut3d,    kEmbeddedMetal_fs_postfx_lut3d_size };
    }

    // Stretch/affine fall back to sprite+texture on platforms without
    // dedicated blit bytecode (Vulkan today): no src-rect UV mapping.
    if (stretchVs.size == 0) { stretchVs = vsSprite; stretchFs = fsTexture; }
    if (affineVs.size == 0)  { affineVs  = vsSprite; affineFs  = fsTexture; }

    // Post-processing chain: only D3D ships dedicated PS bytecode today.
    // On GL/Metal/Vulkan the full-screen PS falls back to the plain texture
    // copy (identity) so the chain still runs (stages composite as copies).
    if (fsPostfxVignette.size == 0) fsPostfxVignette = fsTexture;
    if (fsPostfxLut.size == 0)      fsPostfxLut      = fsTexture;
    if (fsPostfxBlur.size == 0)     fsPostfxBlur     = fsTexture;
    if (fsPostfxBloom.size == 0)    fsPostfxBloom    = fsTexture;
    // t214: Lut3D ships D3D bytecode; GL/Metal/Vulkan fall back to the
    // identity copy (same policy as the other postfx PS above) until the
    // shaderc-generated arrays are regen'd (shaders/compile_shaders.bat).
    if (fsPostfxLut3d.size == 0)    fsPostfxLut3d    = fsTexture;

    if (!vsSprite.data || vsSprite.size == 0 ||
        !fsTexture.data || fsTexture.size == 0) {
        printf("[BgfxShaderManager] No embedded shaders for %s. "
               "Debug text only.\n", bgfx::getRendererName(renderer));
        return;
    }

    auto buildProgram = [&](const Bytecode& vs, const Bytecode& selectedFragment,
                            const char* name,
                            const ShaderUniformMetadata* fragmentUniform) -> bgfx::ProgramHandle {
        Bytecode fs = selectedFragment;
#if defined(CAESURA_RENDER_TEST_FAULTS)
        const bool missingFragment =
            (m_testFault == ShaderTestFault::FallbackMissingFragment && std::string(name) == "Fallback")
            || (m_testFault == ShaderTestFault::BlendMissingFragment && std::string(name) == "Blend")
            || (m_testFault == ShaderTestFault::SoftBlurMissingFragment && std::string(name) == "PostFxSoftBlur")
            || (m_testFault == ShaderTestFault::TransitionMissingFragment && std::string(name) == "Transition");
        if (missingFragment) {
            fs = {};
            ++m_injectedFaultCount;
        }
#endif
        if (!vs.data || !fs.data || vs.size == 0 || fs.size == 0) {
            ++m_buildFailures;
            m_failedProgramNames += std::string(name) + " ";
            fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %s: no embedded data.\n", name);
            return BGFX_INVALID_HANDLE;
        }
        // t73: on the direct-feed path validate the blob before submission so a
        // malformed / binary-in-binary embed is caught loudly HERE instead of
        // reaching a renderer pipeline state with a nil vertex function.
        if (directFeed
        &&  (!isDirectFeedBinary(vs.data, vs.size)
         ||  !isDirectFeedBinary(fs.data, fs.size))) {
            ++m_buildFailures;
            m_failedProgramNames += std::string(name) + " ";
            fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %s: embedded data is not a "
                            "direct-feedable shader binary (vs feed=%d fs feed=%d). "
                            "Shader format regression?\n",
                    name, isDirectFeedBinary(vs.data, vs.size),
                    isDirectFeedBinary(fs.data, fs.size));
            return BGFX_INVALID_HANDLE;
        }
        const uint16_t vsAttrs[] = { 0x0001, 0x0010 };
        bgfx::ShaderHandle vsHandle = BGFX_INVALID_HANDLE;
        bgfx::ShaderHandle fsHandle = BGFX_INVALID_HANDLE;
        if (directFeed) {
            // Complete shaderc binary: pass through unchanged. The static
            // embedded arrays outlive every frame, so makeRef (no release)
            // is safe; the trailing attrs/cb bytes are ignored by the parsers.
            vsHandle = bgfx::createShader(bgfx::makeRef(vs.data, (uint32_t)vs.size));
            fsHandle = bgfx::createShader(bgfx::makeRef(fs.data, (uint32_t)fs.size));
        } else {
            vsHandle = buildBgfxShader(
                vs.data, (uint32_t)vs.size, false, 2, vsAttrs);
            fsHandle = buildBgfxShader(
                fs.data, (uint32_t)fs.size, true, 0, nullptr, fragmentUniform);
        }
        if (!bgfx::isValid(vsHandle) || !bgfx::isValid(fsHandle)) {
            ++m_buildFailures;
            m_failedProgramNames += std::string(name) + " ";
            fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %s: shader build failed.\n", name);
            if (bgfx::isValid(vsHandle)) bgfx::destroy(vsHandle);
            if (bgfx::isValid(fsHandle)) bgfx::destroy(fsHandle);
            return BGFX_INVALID_HANDLE;
        }
        bgfx::ProgramHandle prog = bgfx::createProgram(vsHandle, fsHandle, true);
        if (!bgfx::isValid(prog)) {
            ++m_buildFailures;
            m_failedProgramNames += std::string(name) + " ";
            fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %s: program build failed.\n", name);
        }
        printf("[BgfxShaderManager] %s program %s.\n", name,
               bgfx::isValid(prog) ? "READY" : "FAILED");
        return prog;
    };

    constexpr uint8_t kUniformFragmentBit = 0x10;
    const ShaderUniformMetadata vfxParams = {
        "VFXParams",
        uint8_t(bgfx::UniformType::Vec4) | kUniformFragmentBit,
        3, 0, 3, 48
    };

    m_fallbackProgram = buildProgram(vsSprite, fsTexture, "Fallback", nullptr);
    m_blendProgram = buildProgram(vsFullscreen, fsBlend, "Blend", nullptr);
    m_transitionProgram = buildProgram(vsFullscreen, fsTransition, "Transition", nullptr);
    m_vfxProgram = buildProgram(vsFullscreen, fsVfx, "VFX", &vfxParams);
    m_stretchProgram = buildProgram(stretchVs, stretchFs, "StretchBlt", nullptr);
    m_affineProgram = buildProgram(affineVs, affineFs, "AffineBlt", nullptr);

    // Round-102 post-processing chain: shared 4x vec4 params uniform.
    const ShaderUniformMetadata postfxParams = {
        "PostFxParams",
        uint8_t(bgfx::UniformType::Vec4) | kUniformFragmentBit,
        4, 0, 4, 64
    };
    m_postfxVignette = buildProgram(vsFullscreen, fsPostfxVignette, "PostFxVignette", &postfxParams);
    m_postfxLut      = buildProgram(vsFullscreen, fsPostfxLut,      "PostFxLutGrade",  &postfxParams);
    m_postfxBlur     = buildProgram(vsFullscreen, fsPostfxBlur,     "PostFxSoftBlur",  &postfxParams);
    m_postfxBloom    = buildProgram(vsFullscreen, fsPostfxBloom,    "PostFxBloom",     &postfxParams);
    m_postfxLut3d    = buildProgram(vsFullscreen, fsPostfxLut3d,    "PostFxLut3D",     &postfxParams);
    m_u_postfxParams = bgfx::createUniform("PostFxParams", bgfx::UniformType::Vec4, 4);

    // Verify fallback program is valid before registering
    if (!bgfx::isValid(m_fallbackProgram)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxShaderManager] FALLBACK PROGRAM INVALID, all rendering disabled!");
    }

    // -- Create uniform handles for effect cbuffers -------------------
    m_u_blendParams = bgfx::createUniform("BlendParams",  bgfx::UniformType::Vec4, 2);
    m_u_transParams = bgfx::createUniform("TransParams",  bgfx::UniformType::Vec4, 1);
    m_u_vfxParams   = bgfx::createUniform("VFXParams",    bgfx::UniformType::Vec4, 3);
    m_u_stretchParams = bgfx::createUniform("StretchParams", bgfx::UniformType::Vec4, 1);
    m_u_affineParams  = bgfx::createUniform("AffineParams",  bgfx::UniformType::Vec4, 4);

    // -- Register with ShaderCache -------------------------------------
    if (bgfx::isValid(m_blendProgram)) {
        static const int kAlphaModes[] = {
            (int)BlendMode::Normal,    (int)BlendMode::Multiply,
            (int)BlendMode::Screen,    (int)BlendMode::Overlay,
            (int)BlendMode::Darken,    (int)BlendMode::Lighten,
            (int)BlendMode::ColorDodge,(int)BlendMode::ColorBurn,
            (int)BlendMode::HardLight, (int)BlendMode::SoftLight
        };
        for (int mode : kAlphaModes) {
            CompositeShaderKey key = { (uint8_t)mode };
            CompositeShaderCache::instance().registerProgram(key, m_blendProgram);
        }
        printf("[BgfxShaderManager] Registered 10 blend modes with ShaderCache.\n");
    }
    CompositeShaderCache::instance().precompileCommon();
    if (bgfx::isValid(m_fallbackProgram)) {
        // Reserved (non-enum) key: the fallback program is NOT a blend-mode
        // variant. Registering it under the default {0,false} key would clobber
        // the Normal blend entry (mode semantics travel via u_blendParams on the
        // blend program), and the alias branch prefers Normal -- so keep the
        // fallback on its own slot.
        CompositeShaderKey fk;
        fk.blendMode  = -1;
        fk.usePalette = false;
        CompositeShaderCache::instance().registerProgram(fk, m_fallbackProgram);
    }

    // t73/t75 degrade contract (b): FAIL LOUD once, keep the frame loop alive.
    if (coreProgramsBroken()) {
        fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %d embedded shader build "
                        "failure(s) - CORE programs broken: rendering disabled "
                        "(IFH), engine keeps running, no half-broken program "
                        "will be submitted.\n", m_buildFailures);
    } else if (m_buildFailures > 0) {
        fprintf(stderr, "[RENDER][ERROR] [BgfxShaderManager] %d non-core embedded shader "
                        "build failure(s): %s - affected effects will be skipped, "
                        "rendering continues.\n", m_buildFailures, m_failedProgramNames.c_str());
    } else if (directFeed) {
        printf("[BgfxShaderManager] Embedded shader feed mode: direct "
               "(%s).\n", bgfx::getRendererName(renderer));
    }
}
} // namespace Caesura
