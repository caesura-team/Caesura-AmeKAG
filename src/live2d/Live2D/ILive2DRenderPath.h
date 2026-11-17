#pragma once
#ifdef CAESURA_LIVE2D

#include <bgfx/bgfx.h>

namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {
    class CubismRenderer;
} } } }
namespace CsmRendering = Live2D::Cubism::Framework::Rendering;

namespace Caesura {

// Abstract render path: maps Cubism output to a bgfx texture.
// Each GPU backend (OpenGL/D3D11/Metal/Vulkan) gets its own implementation.
class ILive2DRenderPath {
public:
    virtual ~ILive2DRenderPath() = default;

    // One-time setup, typically inside a CubismFramework::Initialize() call
    virtual bool init(int width, int height) = 0;

    // Tear down GPU resources
    virtual void shutdown() = 0;

    // Begin a Cubism frame (set render target, clear)
    virtual void beginFrame(CsmRendering::CubismRenderer* renderer) = 0;

    // End a Cubism frame and transfer pixels to bgfx
    virtual void endFrame(CsmRendering::CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) = 0;

    // Shared render paths have no renderer-specific target.
    virtual void releaseModelTarget(CsmRendering::CubismRenderer*) {}

    // Resize render target
    virtual void resize(int width, int height) = 0;

    // Human-readable name
    virtual const char* name() const = 0;
};

} // namespace Caesura

#endif
