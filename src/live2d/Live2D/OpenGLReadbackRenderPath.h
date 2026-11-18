#pragma once
#if defined(CAESURA_LIVE2D) && defined(__linux__) && !defined(__ANDROID__)

#include "ILive2DRenderPath.h"
#include <cstdint>
#include <memory>

namespace Live2D::Cubism::Framework {
class CubismUserModel;
}

namespace Caesura {

class OpenGLReadbackRenderPath : public ILive2DRenderPath {
public:
    OpenGLReadbackRenderPath();
    ~OpenGLReadbackRenderPath() override;

    bool init(int width, int height) override;
    void shutdown() override;
    CsmRendering::CubismRenderer* createModelRenderer(
        Live2D::Cubism::Framework::CubismUserModel& model, int width, int height);
    bool bindModelTexture(CsmRendering::CubismRenderer* renderer, std::uint32_t index,
                          int width, int height, const unsigned char* pixels);
    void destroyModelRenderer(std::unique_ptr<Live2D::Cubism::Framework::CubismUserModel>& model,
                              CsmRendering::CubismRenderer* renderer);
    bool disposeFramework();
    void beginFrame(CsmRendering::CubismRenderer* renderer) override;
    void endFrame(CsmRendering::CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) override;
    void resize(int width, int height) override;
    const char* name() const override { return "OpenGLReadback"; }

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace Caesura

#endif
