#pragma once

#include <bgfx/bgfx.h>
#include <vector>
#include "api/IRenderDevice.h"
#include "BgfxQuadBatch.h"
#include <memory>
#include <cstdint>

namespace Caesura {

class BgfxShaderManager;
class BgfxDeviceCore;

// Aggregates all state needed by draw functions.
// Owned by BgfxRenderDevice, passed to BgfxDraw.
struct DrawState {
    bgfx::VertexLayout   posTexLayout;
    std::vector<struct BatchQuad> batchQuads;
    bool                 batching = false;
    BgfxShaderManager*   shaders  = nullptr;
    BgfxDeviceCore*      device   = nullptr;
};

struct BatchQuad {
    uint16_t viewId;
    bgfx::TextureHandle tex;
    float x, y, w, h;
    uint8_t opacity;
};

class BgfxDraw {
public:
    BgfxDraw() = default;
    ~BgfxDraw() = default;

    BgfxDraw(const BgfxDraw&) = delete;
    BgfxDraw& operator=(const BgfxDraw&) = delete;

    void init(DrawState* state);
    void beginBatch();
    void flushBatch();
    void blitTexture(uint16_t targetView, uint32_t textureId, float x, float y, float w, float h, uint8_t opacity);
    void blitTexture(uint16_t targetView, bgfx::TextureHandle tex, float x, float y, float w, float h, uint8_t opacity, bool flipV = false);

    void stretchBlt(uint16_t targetView, uint32_t dstTexId, float dx, float dy, float dw, float dh,
                    uint32_t srcTexId, float sx, float sy, float sw, float sh, int filterType);
    void affineBlt(uint16_t targetView, uint32_t dstTexId, float dx, float dy, float dw, float dh,
                   uint32_t srcTexId, float sx, float sy, float sw, float sh, const float matrix[6]);

    void submitBlend(uint16_t viewId, bgfx::TextureHandle baseTex, bgfx::TextureHandle blendTex,
                     int mode, float baseAlpha, float blendAlpha, float globalAlpha);
    void submitTransition(uint16_t viewId, bgfx::TextureHandle fromTex, bgfx::TextureHandle toTex,
                          bgfx::TextureHandle ruleTex, int method, float progress);
    void submitVFX(uint16_t viewId, bgfx::TextureHandle srcTex, int effect,
                   float fadeAlpha, float fadeR, float fadeG, float fadeB,
                   float blurRadius, float quakeX, float quakeY);
    void fillViewport(ViewportHandle handle, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

private:
    DrawState* m_state = nullptr;
    std::unique_ptr<BgfxQuadBatch> m_quadBatch;
};

} // namespace Caesura
