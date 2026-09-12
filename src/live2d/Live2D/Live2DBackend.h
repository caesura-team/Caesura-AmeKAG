#pragma once
#ifdef CAESURA_LIVE2D

#include "../api/IAnimationBackend.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <bgfx/bgfx.h>

namespace Live2D { namespace Cubism { namespace Framework {
    class CubismUserModel;
    class CubismModelSettingJson;
} } }
namespace Csm = Live2D::Cubism::Framework;

struct ID3D11ShaderResourceView;

namespace Caesura {

class ILive2DRenderPath;

class IRenderDevice;

class Live2DBackend : public IAnimationBackend {
public:
    Live2DBackend() = default;
    ~Live2DBackend() override;
    bool init() override;
    void shutdown() override;
    bool isCubismAvailable() const override { return m_initialized; }

    int  loadModel(const std::string& path, const std::string& name) override;
    void unloadModel(int handle) override;
    bool isLoaded(int handle) const override;
    std::size_t loadedModelCount() const override;
    void clearModels() override;

    void showModel(int handle, float x, float y, float scale) override;
    void hideModel(int handle) override;
    void setOpacity(int handle, float opacity) override;
    void render(float dt) override;

    bool playMotion(int handle, const std::string& name) override;
    void setExpression(int handle, const std::string& name) override;
    void setParameter(int handle, const std::string& param, float value) override;

    const char* name() const override { return "Live2D"; }

    void setRenderDevice(IRenderDevice* device);

private:
    struct Live2DModel {
        std::string dir;
        std::string name;
        bool visible = false;
        float x = 0, y = 0, scale = 1.0f, opacity = 1.0f;

        // Cubism
        std::unique_ptr<Csm::CubismUserModel> userModel;
        void* renderer = nullptr;       // CubismRenderer_OpenGLES2*
        int renderWidth = 1280;
        int renderHeight = 720;

        // bgfx texture
        bgfx::TextureHandle bgfxTex = BGFX_INVALID_HANDLE;
        bool bgfxTexValid = false;
        std::vector<bgfx::TextureHandle> textures; // model textures (RGBA8)

        // Cached raw asset data (owned)
        std::vector<char> mocData;     // .moc3 raw bytes (aligned)
        std::vector<char> settingJson; // .model3.json raw bytes
        Csm::CubismModelSettingJson* setting = nullptr;

        // Motion/expression cache
        std::unordered_map<std::string, std::vector<char>> motionCache;
        std::unordered_map<std::string, std::vector<char>> expressionCache;

#ifdef _WIN32
        // D3D11 model-texture SRVs (owned; released in ~Live2DModel)
        std::vector<ID3D11ShaderResourceView*> textureSrvs;
#endif

        ~Live2DModel();
    };

    IRenderDevice* m_renderDevice = nullptr;
    bool m_deviceReady = false;

    ILive2DRenderPath* m_renderPath = nullptr;

    std::unordered_map<int, std::unique_ptr<Live2DModel>> m_models;
    int m_nextHandle = 1;
    bool m_initialized = false;

    bool loadModelInternal(Live2DModel& model);
    bool createRenderer(Live2DModel& model);
#ifdef _WIN32
    void releaseModelTarget(Live2DModel& model);
#endif
};

} // namespace Caesura

#endif
