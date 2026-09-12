#include "BgfxQuadBatch.h"
#include "NdcMath.h"
#include "BgfxDraw.h"
#include "BgfxShaderManager.h"
#include "BgfxDeviceCore.h"  // getWidth/getHeight for pixel->NDC conversion
#include <bgfx/bgfx.h>
#include <cstring>

namespace Caesura {

// ===========================================================================
// Pure batch math (GPU-free) -- extracted so unit tests can pin the NDC
// conversion, merge grouping and index generation without a GPU (P2-10).
// ===========================================================================

BgfxQuadBatch::NdcRect BgfxQuadBatch::quadToNdc(float x, float y, float w, float h,
                                                float screenW, float screenH) {
    // Delegate to the shared pure conversion so every CPU-side blit path
    // agrees on one implementation (see NdcMath.h).
    const Caesura::NdcRect n = pixelToNdc(x, y, w, h, screenW, screenH);
    NdcRect out;
    out.nx0 = n.x0; out.ny0 = n.y0; out.nx1 = n.x1; out.ny1 = n.y1;
    return out;
}

void BgfxQuadBatch::computeMergeGroups(const std::vector<BatchQuad>& quads,
                                       std::vector<MergeGroup>& groups) {
    groups.clear();
    const uint32_t quadCount = (uint32_t)quads.size();
    uint32_t qi = 0;
    while (qi < quadCount) {
        const auto& q = quads[qi];
        uint32_t mergeEnd = qi + 1;
        while (mergeEnd < quadCount &&
               quads[mergeEnd].tex.idx == q.tex.idx &&
               quads[mergeEnd].viewId == q.viewId &&
               quads[mergeEnd].opacity == q.opacity) {
            mergeEnd++;
        }
        groups.push_back(MergeGroup{qi, mergeEnd - qi});
        qi = mergeEnd;
    }
}

void BgfxQuadBatch::buildGroupIndices(uint32_t quadCount,
                                      std::vector<uint16_t>& indices) {
    indices.clear();
    indices.reserve(quadCount * 6);
    for (uint32_t m = 0; m < quadCount; m++) {
        const uint32_t localBase = m * 4;
        indices.push_back((uint16_t)(localBase + 0));
        indices.push_back((uint16_t)(localBase + 1));
        indices.push_back((uint16_t)(localBase + 2));
        indices.push_back((uint16_t)(localBase + 0));
        indices.push_back((uint16_t)(localBase + 2));
        indices.push_back((uint16_t)(localBase + 3));
    }
}

void BgfxQuadBatch::beginBatch() {
    m_state->batching = true;
    m_state->batchQuads.clear();
}

void BgfxQuadBatch::flushBatch() {
    if (!m_state->batching || m_state->batchQuads.empty()) {
        m_state->batching = false;
        return;
    }

    // Ensure vertex layout is initialized (same as blitTexture lazy init)
    if (m_state->posTexLayout.getStride() == 0) {
        m_state->posTexLayout
            .begin()
            .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
    }

    const float sw = (float)m_state->device->getWidth();
    const float sh = (float)m_state->device->getHeight();
    if (sw <= 0.0f || sh <= 0.0f) { m_state->batching = false; return; }

    struct FsVertex { float x, y, u, v; };

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                         | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                                 BGFX_STATE_BLEND_INV_SRC_ALPHA);

    // Pure merge grouping: contiguous quads sharing (tex, viewId, opacity)
    // collapse into one submit.
    std::vector<MergeGroup> groups;
    computeMergeGroups(m_state->batchQuads, groups);

    // t75: never submit an invalid program handle -- the fallback program is
    // the core sprite/quad pipeline, but a failure must not turn this into a
    // poison submit; skip the batch and let the next frame retry.
    const auto modulated = m_state->shaders->getModulatedTextureProgram();
    const auto program = bgfx::isValid(modulated) ? modulated : m_state->shaders->getFallbackProgram();
    if (!bgfx::isValid(program)) {
        m_state->batchQuads.clear();
        m_state->batching = false;
        return;
    }

    for (const auto& g : groups) {
        const auto& q = m_state->batchQuads[g.startQuad];
        if (q.opacity == 0) continue;
        const uint32_t groupVertCount = g.quadCount * 4;
        const uint32_t groupIdxCount  = g.quadCount * 6;

        if (bgfx::getAvailTransientVertexBuffer(groupVertCount, m_state->posTexLayout) < groupVertCount ||
            bgfx::getAvailTransientIndexBuffer(groupIdxCount) < groupIdxCount) {
            continue;
        }

        bgfx::TransientVertexBuffer gtvb;
        bgfx::allocTransientVertexBuffer(&gtvb, groupVertCount, m_state->posTexLayout);
        auto* gv = (FsVertex*)gtvb.data;

        bgfx::TransientIndexBuffer gtib;
        bgfx::allocTransientIndexBuffer(&gtib, groupIdxCount);
        auto* gi = (uint16_t*)gtib.data;

        for (uint32_t qi = 0; qi < g.quadCount; qi++) {
            const auto& quad = m_state->batchQuads[g.startQuad + qi];
            const NdcRect n = quadToNdc(quad.x, quad.y, quad.w, quad.h, sw, sh);
            const uint32_t bVert = qi * 4;
            gv[bVert + 0] = { n.nx0, n.ny0, 0.0f, 0.0f };
            gv[bVert + 1] = { n.nx1, n.ny0, 1.0f, 0.0f };
            gv[bVert + 2] = { n.nx1, n.ny1, 1.0f, 1.0f };
            gv[bVert + 3] = { n.nx0, n.ny1, 0.0f, 1.0f };

            const uint32_t bIdx = qi * 6;
            gi[bIdx + 0] = (uint16_t)(bVert + 0);
            gi[bIdx + 1] = (uint16_t)(bVert + 1);
            gi[bIdx + 2] = (uint16_t)(bVert + 2);
            gi[bIdx + 3] = (uint16_t)(bVert + 0);
            gi[bIdx + 4] = (uint16_t)(bVert + 2);
            gi[bIdx + 5] = (uint16_t)(bVert + 3);
        }

        bgfx::setTexture(0, m_state->shaders->getDefaultSampler(), q.tex);
        if (bgfx::isValid(modulated)) {
            const float color[4] = {1, 1, 1, q.opacity / 255.0f};
            bgfx::setUniform(m_state->shaders->getColorUniform(), color);
        }

        // Submit the vertex/index subset for this texture group
        bgfx::setVertexBuffer(0, &gtvb, 0, groupVertCount);
        bgfx::setIndexBuffer(&gtib, 0, groupIdxCount);
        bgfx::setState(state);
        bgfx::submit(q.viewId, program);
    }

    m_state->batchQuads.clear();
    m_state->batching = false;
}
} // namespace Caesura
