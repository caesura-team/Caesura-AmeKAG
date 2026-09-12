// BgfxDraw_Effects.cpp - Post-processing effects (blend, transition, VFX, fillViewport)
#include "BgfxDraw.h"
#include "BgfxShaderManager.h"
#include "ColorFilterMath.h"
#include "BgfxDeviceCore.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <cstdint>
#include <cstdio>

namespace Caesura {

static void submitFullscreenQuad(uint16_t viewId, bgfx::ProgramHandle program,
                                  float x, float y, float w, float h,
                                  bgfx::TextureHandle tex, bgfx::UniformHandle sampler,
                                  bgfx::UniformHandle /*params*/, const float* /*paramData*/, uint16_t /*paramVec4s*/) {
    if (!bgfx::isValid(program)) return;

    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) return;
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
    auto* v = (FsVertex*)tvb.data;

    const bgfx::Caps* caps = bgfx::getCaps();
    v[0] = { -1.0f,  1.0f, 0.0f, 0.0f };
    v[1] = {  1.0f,  1.0f, 1.0f, 0.0f };
    v[2] = {  1.0f, -1.0f, 1.0f, 1.0f };
    v[3] = { -1.0f, -1.0f, 0.0f, 1.0f };

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    if (bgfx::isValid(tex) && bgfx::isValid(sampler))
        bgfx::setTexture(0, sampler, tex);

    bgfx::submit(viewId, program);
}

void BgfxDraw::submitBlend(uint16_t viewId, bgfx::TextureHandle baseTex,
                                    bgfx::TextureHandle blendTex, int mode,
                                    float baseAlpha, float blendAlpha, float globalAlpha) {
    if (!bgfx::isValid(m_state->shaders->getBlendProgram())) {
        static bool once = false;
        if (!once) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[BgfxRenderDevice] submitBlend: blend program not loaded.");
            once = true;
        }
        return;
    }

    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), baseTex);
    bgfx::setTexture(1, m_state->shaders->getSampler1(), blendTex);

    float params[8] = { baseAlpha, blendAlpha, globalAlpha, (float)mode, 0, 0, 0, 0 };
    bgfx::setUniform(m_state->shaders->getBlendParams(), params, 2);

    submitFullscreenQuad(viewId, m_state->shaders->getBlendProgram(), 0, 0, (float)m_state->device->getWidth(), (float)m_state->device->getHeight(), BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, nullptr, 0);
}

void BgfxDraw::submitTransition(uint16_t viewId, bgfx::TextureHandle fromTex,
                                         bgfx::TextureHandle toTex,
                                         bgfx::TextureHandle ruleTex,
                                         int method, float progress) {
    if (!bgfx::isValid(m_state->shaders->getTransitionProgram())) {
        static bool once = false;
        if (!once) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[BgfxRenderDevice] submitTransition: transition program not loaded.");
            once = true;
        }
        return;
    }

    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), fromTex);
    bgfx::setTexture(1, m_state->shaders->getSampler1(), toTex);
    if (bgfx::isValid(ruleTex))
        bgfx::setTexture(2, m_state->shaders->getSampler2(), ruleTex);

    float params[4] = { progress, (float)method, 0, 0 };
    bgfx::setUniform(m_state->shaders->getTransParams(), params, 1);

    submitFullscreenQuad(viewId, m_state->shaders->getTransitionProgram(), 0, 0, (float)m_state->device->getWidth(), (float)m_state->device->getHeight(), BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, nullptr, 0);
}

void BgfxDraw::submitVFX(uint16_t viewId, bgfx::TextureHandle srcTex,
                                  int effect, float fadeAlpha,
                                  float fadeR, float fadeG, float fadeB,
                                  float blurRadius, float quakeX, float quakeY) {
    if (!bgfx::isValid(m_state->shaders->getVFXProgram())) {
        static bool once = false;
        if (!once) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[BgfxRenderDevice] submitVFX: VFX program not loaded.");
            once = true;
        }
        return;
    }

    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), srcTex);

    struct VFXParams {
        float color[4];
        float blurQuake[4];
        int32_t effect;
        float padding[3];
    };
    static_assert(sizeof(VFXParams) == 48);

    const VFXParams params = {
        { fadeR, fadeG, fadeB, fadeAlpha },
        { blurRadius, blurRadius, quakeX, quakeY },
        effect,
        { 0.0f, 0.0f, 0.0f }
    };
    // Effect 4 (colorblind/contrast filter): C++ fills the matrix rows
    // from the active preset (setColorFilter) via the shared pure packer.
    VFXParams filtered = params;
    if (effect == 4) {
        const float* m = m_state->device->getColorFilterMatrix();
        if (m) {
            const VfxColorFilterPack p = packVfxColorFilter(m, fadeAlpha);
            for (int i = 0; i < 4; ++i) filtered.color[i] = p.color[i];
            for (int i = 0; i < 4; ++i) filtered.blurQuake[i] = p.blurQuake[i];
            for (int i = 0; i < 3; ++i) filtered.padding[i] = p.padding[i];
        }
    }
    bgfx::setUniform(m_state->shaders->getVFXParams(), &filtered, 3);

    submitFullscreenQuad(viewId, m_state->shaders->getVFXProgram(), 0, 0, (float)m_state->device->getWidth(), (float)m_state->device->getHeight(), srcTex, m_state->shaders->getDefaultSampler(), BGFX_INVALID_HANDLE, nullptr, 0);
}

void BgfxDraw::fillViewport(ViewportHandle handle,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    bgfx::FrameBufferHandle fb = m_state->device->getRttFb(handle);
    if (!bgfx::isValid(fb)) return;
    uint16_t vpView = BgfxDeviceCore::VIEW_RTT;
    bgfx::setViewFrameBuffer(vpView, fb);

    bgfx::TextureHandle colorTex = m_state->device->getSolidPixel(r, g, b, a);

    if (!bgfx::isValid(colorTex) || !bgfx::isValid(m_state->shaders->getFallbackProgram())) {
        bgfx::setViewFrameBuffer(vpView, BGFX_INVALID_HANDLE);
        return;
    }

    struct FsVertex { float x, y, u, v; };
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    if (bgfx::getAvailTransientVertexBuffer(4, layout) < 4) {
        bgfx::setViewFrameBuffer(vpView, BGFX_INVALID_HANDLE);
        return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
    auto* v = (FsVertex*)tvb.data;
    const bgfx::Caps* caps = bgfx::getCaps();
    v[0] = { -1.0f,  1.0f, 0.0f, 0.0f };
    v[1] = {  1.0f,  1.0f, 1.0f, 0.0f };
    v[2] = {  1.0f, -1.0f, 1.0f, 1.0f };
    v[3] = { -1.0f, -1.0f, 0.0f, 1.0f };

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) {
        bgfx::setViewFrameBuffer(vpView, BGFX_INVALID_HANDLE);
        return;
    }
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), colorTex);
    bgfx::setState(state);
    bgfx::submit(vpView, m_state->shaders->getFallbackProgram());

    // The core owns this cached texture and releases it on color replacement
    // or shutdown. This draw only borrows the handle, including same-color reuse.

    printf("[BgfxRenderDevice] fillViewport #%u: (%d,%d,%d,%d) -> view %u\n",
           handle.id, r, g, b, a, (unsigned)vpView);
}

} // namespace Caesura
