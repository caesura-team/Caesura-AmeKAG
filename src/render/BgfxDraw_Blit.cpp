// BgfxDraw_Blit.cpp - Texture blit operations (blitTexture, stretchBlt, affineBlt)
#include "BgfxDraw.h"
#include "NdcMath.h"
#include "BgfxShaderManager.h"
#include "BgfxDeviceCore.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <cstdio>

namespace Caesura {

void BgfxDraw::blitTexture(uint16_t targetView, uint32_t textureId,
                                    float x, float y, float w, float h, uint8_t opacity) {
    if (m_state->batching) {
        m_state->batchQuads.push_back({targetView, { static_cast<uint16_t>(textureId) }, x, y, w, h, opacity});
        return;
    }
    bgfx::TextureHandle tex = { uint16_t(textureId) };
    blitTexture(targetView, tex, x, y, w, h, opacity);
}

void BgfxDraw::blitTexture(uint16_t targetView, bgfx::TextureHandle tex,
                                    float x, float y, float w, float h, uint8_t opacity, bool flipV) {
    if (!bgfx::isValid(tex)) return;

    float ortho[16];
    const bgfx::Caps* caps = bgfx::getCaps();
    bx::mtxOrtho(ortho, 0.0f, float(m_state->device->getWidth()), float(m_state->device->getHeight()), 0.0f,
                 -1.0f, 1.0f, 0.0f, caps ? caps->homogeneousDepth : false,
                 bx::Handedness::Left);
    bgfx::setViewTransform(targetView, nullptr, ortho);

    if (m_state->posTexLayout.getStride() == 0) {
        m_state->posTexLayout
            .begin()
            .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
    }

    float sw = (float)m_state->device->getWidth();
    float sh = (float)m_state->device->getHeight();
    if (sw <= 0.0f || sh <= 0.0f) return;  // guard zero-size backbuffer

    // Shared pure pixel->NDC conversion (see NdcMath.h).
    const NdcRect n = pixelToNdc(x, y, w, h, sw, sh);

    struct PosTexVertex { float x, y; float u, v; };

    const float topV = flipV ? 1.0f : 0.0f, bottomV = 1.0f - topV;
    PosTexVertex quad[4] = {
        { n.x0, n.y0, 0.0f, topV },
        { n.x1, n.y0, 1.0f, topV },
        { n.x1, n.y1, 1.0f, bottomV },
        { n.x0, n.y1, 0.0f, bottomV },
    };

    bgfx::TransientVertexBuffer tvb;
    if (bgfx::getAvailTransientVertexBuffer(4, m_state->posTexLayout) < 4) return;
    bgfx::allocTransientVertexBuffer(&tvb, 4, m_state->posTexLayout);
    bx::memCopy(tvb.data, quad, sizeof(quad));

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    const auto modulated = m_state->shaders->getModulatedTextureProgram();
    const auto program = bgfx::isValid(modulated) ? modulated : m_state->shaders->getFallbackProgram();
    if (!bgfx::isValid(program) || opacity == 0) return;
    if (bgfx::isValid(modulated)) {
        const float color[4] = {1, 1, 1, opacity / 255.0f};
        bgfx::setUniform(m_state->shaders->getColorUniform(), color);
    }

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), tex);
    bgfx::setState(state);
    bgfx::submit(targetView, program);
}

void BgfxDraw::stretchBlt(uint16_t targetView, uint32_t dstTexId,
                                   float dx, float dy, float dw, float dh,
                                   uint32_t srcTexId,
                                   float sx, float sy, float sw, float sh,
                                   int /*filterType*/) {
    bgfx::TextureHandle srcTex = { uint16_t(srcTexId) };
    (void)dstTexId;

    if (!bgfx::isValid(srcTex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxRenderDevice] stretchBlt: invalid src tex");
        return;
    }

    const float swB = (float)m_state->device->getWidth();
    const float shB = (float)m_state->device->getHeight();
    if (swB <= 0.0f || shB <= 0.0f) return;

    // Shared pure pixel->NDC conversion (see NdcMath.h).
    const NdcRect n = pixelToNdc(dx, dy, dw, dh, swB, shB);

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

    v[0] = { n.x0, n.y0, sx,      sy };
    v[1] = { n.x1, n.y0, sx + sw, sy };
    v[2] = { n.x1, n.y1, sx + sw, sy + sh };
    v[3] = { n.x0, n.y1, sx,      sy + sh };

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    float stretchData[4] = { sx, sy, sw, sh };
    bgfx::setUniform(m_state->shaders->getStretchParams(), stretchData, 1);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), srcTex);

    bgfx::ProgramHandle prog = bgfx::isValid(m_state->shaders->getStretchProgram()) ? m_state->shaders->getStretchProgram() : m_state->shaders->getFallbackProgram();
    bgfx::submit(targetView, prog);
}

void BgfxDraw::affineBlt(uint16_t targetView, uint32_t dstTexId,
                                  float dx, float dy, float dw, float dh,
                                  uint32_t srcTexId,
                                  float sx, float sy, float sw, float sh,
                                  const float matrix[6]) {
    bgfx::TextureHandle srcTex = { uint16_t(srcTexId) };
    (void)dstTexId;

    if (!bgfx::isValid(srcTex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[BgfxRenderDevice] affineBlt: invalid src tex");
        return;
    }

    const float swB = (float)m_state->device->getWidth();
    const float shB = (float)m_state->device->getHeight();
    if (swB <= 0.0f || shB <= 0.0f) return;

    // Shared pure pixel->NDC conversion (see NdcMath.h).
    const NdcRect n = pixelToNdc(dx, dy, dw, dh, swB, shB);

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

    v[0] = { n.x0, n.y0, sx,      sy };
    v[1] = { n.x1, n.y0, sx + sw, sy };
    v[2] = { n.x1, n.y1, sx + sw, sy + sh };
    v[3] = { n.x0, n.y1, sx,      sy + sh };

    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer(6) < 6) return;
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tib.data, indices, sizeof(indices));

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    float affineData[16] = {
        matrix[0], matrix[2], matrix[4], 0.0f,
        matrix[1], matrix[3], matrix[5], 0.0f,
        sx, sy, sw, sh,
        0.0f, 0.0f, 0.0f, 0.0f
    };
    bgfx::setUniform(m_state->shaders->getAffineParams(), affineData, 4);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), srcTex);

    bgfx::ProgramHandle prog = bgfx::isValid(m_state->shaders->getAffineProgram()) ? m_state->shaders->getAffineProgram() : m_state->shaders->getFallbackProgram();
    bgfx::submit(targetView, prog);
}

} // namespace Caesura
