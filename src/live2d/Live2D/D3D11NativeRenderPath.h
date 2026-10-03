#pragma once
#if defined(CAESURA_LIVE2D) && defined(_WIN32)

#include "ILive2DRenderPath.h"

#include <unordered_map>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace Caesura {

// Windows optimal: Cubism D3D11 → shared texture → bgfx D3D11 (zero copy).
// Shares bgfx's D3D11 device, renders Cubism directly into a bgfx-visible texture.
class D3D11NativeRenderPath : public ILive2DRenderPath {
public:
    D3D11NativeRenderPath() = default;
    ~D3D11NativeRenderPath() override { shutdown(); }
    D3D11NativeRenderPath(const D3D11NativeRenderPath&) = delete;
    D3D11NativeRenderPath& operator=(const D3D11NativeRenderPath&) = delete;
    bool init(int width, int height) override;
    void shutdown() override;

    // Cubism renderer allocation alone does not report shader compilation failure.
    bool ensureShadersReady();

    ID3D11ShaderResourceView* createModelTexture(int width, int height,
                                                 const unsigned char* pixels);
    // Release the per-model render target when the model is unloaded.
    void releaseModelTarget(CsmRendering::CubismRenderer* renderer);
    void beginFrame(CsmRendering::CubismRenderer* renderer) override;
    void endFrame(CsmRendering::CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) override;
    void resize(int width, int height) override;
    const char* name() const override { return "D3D11Native"; }

private:
    enum class ShaderReadiness { Unknown, Ready, Failed };
    ShaderReadiness m_shaderReadiness = ShaderReadiness::Unknown;
    friend struct D3D11LibraryLifetimeProbeAccess;
    bool retainLoadedRuntime();
    // One render target per model so simultaneous visible models do not
    // overwrite each other (each model keeps its own texture/rtv pair).
    struct ModelTarget {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        bgfx::TextureHandle boundBgfxTex = BGFX_INVALID_HANDLE;
    };
    bool createModelTarget(CsmRendering::CubismRenderer* renderer, int width, int height);

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    // Counted loader reference, not a process-lifetime pin. Keep code mapped
    // while external COM references survive bgfx's device-loss fallback.
    void* m_runtimeModule = nullptr;
    std::unordered_map<CsmRendering::CubismRenderer*, ModelTarget> m_targets;

    int m_width = 1280;
    int m_height = 720;
};

} // namespace Caesura

#endif
