#pragma once
#include "api/IAnimationBackend.h"
#include <cstdint>  // fixed-width types (GCC strict)
#include <unordered_map>

namespace Caesura {

class IRenderDevice;
class ITextureManager;

// SDK-less animation backend with a static-image fallback.
// When Cubism SDK is unavailable, loads PNG/JPG/BMP as static textures.
class NullAnimationBackend : public IAnimationBackend {
public:
    bool init() override;
    void shutdown() override;
    bool isCubismAvailable() const override { return false; }

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

    const char* name() const override { return "NullAnimation+PNG"; }

private:
    struct StaticSprite {
        uint32_t textureId = 0;
        float x = 0, y = 0;
        float scale = 1.0f;
        float opacity = 1.0f;
        uint16_t width = 0;
        uint16_t height = 0;
        bool visible = false;
    };

    std::unordered_map<int, StaticSprite> m_sprites;
    ITextureManager* m_textureManager = nullptr;
    IRenderDevice* m_renderDevice = nullptr;
    int m_nextHandle = 1;
    bool m_initialized = false;

    static bool isImagePath(const std::string& path);
};

} // namespace Caesura
