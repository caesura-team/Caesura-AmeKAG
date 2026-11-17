// MetalNativeRenderPath.h — macOS Metal render path (C1/R1).
#pragma once
#if defined(CAESURA_LIVE2D) && defined(__APPLE__)

#include "ILive2DRenderPath.h"
#include <cstdint>  // fixed-width types (GCC strict)
#include <vector>
#include <unordered_map>

#import <Metal/Metal.h>

namespace Caesura {

// Renders Cubism models to an offscreen MTLTexture, reads it back
// synchronously, and uploads into the bgfx texture (same contract as the
// OpenGL readback path). Requires a macOS host to validate.
class MetalNativeRenderPath : public ILive2DRenderPath {
public:
    bool init(int width, int height) override;
    void shutdown() override;
    void beginFrame(CsmRendering::CubismRenderer* renderer) override;
    void endFrame(CsmRendering::CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) override;
    void releaseModelTarget(CsmRendering::CubismRenderer* renderer) override;
    void resize(int width, int height) override;
    const char* name() const override { return "MetalNative"; }

private:
    struct OffscreenEntry {
        Csm::Rendering::CubismOffscreenRenderTarget_Metal target;
        std::vector<uint8_t> pixels;
    };
    bool ensureTarget(CsmRendering::CubismRenderer* renderer);

    id<MTLDevice>           m_device = nil;
    id<MTLCommandQueue>     m_commandQueue = nil;
    id<MTLCommandBuffer>    m_activeCommand = nil;
    int                     m_width = 0;
    int                     m_height = 0;
    std::unordered_map<CsmRendering::CubismRenderer*, OffscreenEntry> m_targets;
};

} // namespace Caesura

#endif
