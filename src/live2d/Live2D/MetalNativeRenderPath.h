#pragma once
#if defined(CAESURA_LIVE2D) && defined(__APPLE__)

#include "ILive2DRenderPath.h"
#include <cstdint>
#include <memory>

namespace Live2D::Cubism::Framework {
class CubismUserModel;
}

namespace Caesura {

class MetalNativeRenderPath : public ILive2DRenderPath {
public:
    MetalNativeRenderPath();
    ~MetalNativeRenderPath() override;

    bool init(int width, int height) override;
    void shutdown() override;
    CsmRendering::CubismRenderer* createModelRenderer(
        Live2D::Cubism::Framework::CubismUserModel& model, int width, int height);
    bool bindModelTexture(CsmRendering::CubismRenderer* renderer, std::uint32_t index,
                          int width, int height, const unsigned char* pixels);
    void beginFrame(CsmRendering::CubismRenderer* renderer) override;
    void endFrame(CsmRendering::CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) override;
    void releaseModelTarget(CsmRendering::CubismRenderer* renderer) override;
    void resize(int width, int height) override;
    const char* name() const override { return "MetalNative"; }

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Caesura

#endif
