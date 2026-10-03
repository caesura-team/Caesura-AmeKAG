// NullAnimationBackend.cpp - static-image fallback for SDK-less environments.
#include "NullAnimationBackend.h"
#include "../debug/api/DebugLog.h"
#include "../di/BackendRegistry.h"
#include "../render/api/ITextureManager.h"
#include "../render/api/IRenderDevice.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Caesura {

bool NullAnimationBackend::isImagePath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower.ends_with(".png") || lower.ends_with(".jpg") ||
           lower.ends_with(".jpeg") || lower.ends_with(".bmp");
}

bool NullAnimationBackend::init() {
    if (m_initialized) return true;

    auto& registry = BackendRegistry::instance();
    m_textureManager = registry.getTextureManager();
    m_renderDevice = registry.getRenderDevice();
    m_initialized = true;
    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[NullAnimation] PNG fallback initialized.");
    return true;
}

void NullAnimationBackend::shutdown() {
    if (!m_initialized) return;

    clearModels();
    m_textureManager = nullptr;
    m_renderDevice = nullptr;
    m_nextHandle = 1;
    m_initialized = false;
}

int NullAnimationBackend::loadModel(const std::string& path, const std::string& /*name*/) {
    if (!m_initialized || !m_textureManager || !isImagePath(path)) return 0;

    const uint32_t texId = m_textureManager->loadTexture(path);
    if (texId == 0) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[NullAnimation] Failed to load image: %s", path.c_str());
        return 0;
    }

    uint16_t width = 0;
    uint16_t height = 0;
    m_textureManager->getTextureSizeById(texId, width, height);
    if (width == 0 || height == 0) {
        m_textureManager->destroyTexture(texId);
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
                "[NullAnimation] Image has no usable dimensions: %s", path.c_str());
        return 0;
    }

    int handle = m_nextHandle++;
    StaticSprite sprite;
    sprite.textureId = texId;
    sprite.width = width;
    sprite.height = height;
    m_sprites[handle] = sprite;
    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok,
            "[NullAnimation] Loaded static sprite #%d: %s (tex=%u)", handle, path.c_str(), texId);
    return handle;
}

void NullAnimationBackend::unloadModel(int handle) {
    auto it = m_sprites.find(handle);
    if (it == m_sprites.end()) return;

    if (m_textureManager && it->second.textureId != 0) {
        m_textureManager->destroyTexture(it->second.textureId);
    }
    m_sprites.erase(it);
}

bool NullAnimationBackend::isLoaded(int handle) const {
    return m_sprites.find(handle) != m_sprites.end();
}

std::size_t NullAnimationBackend::loadedModelCount() const {
    return m_sprites.size();
}

void NullAnimationBackend::clearModels() {
    while (!m_sprites.empty()) {
        unloadModel(m_sprites.begin()->first);
    }
}

void NullAnimationBackend::showModel(int handle, float x, float y, float scale) {
    auto it = m_sprites.find(handle);
    if (it == m_sprites.end()) return;
    it->second.x = x;
    it->second.y = y;
    it->second.scale = scale;
    it->second.visible = true;
}

void NullAnimationBackend::hideModel(int handle) {
    auto it = m_sprites.find(handle);
    if (it == m_sprites.end()) return;
    it->second.visible = false;
}

void NullAnimationBackend::setOpacity(int handle, float opacity) {
    auto it = m_sprites.find(handle);
    if (it == m_sprites.end()) return;
    it->second.opacity = std::isfinite(opacity)
        ? std::clamp(opacity, 0.0f, 1.0f)
        : 0.0f;
}

void NullAnimationBackend::render(float /*dt*/) {
    if (!m_initialized || !m_textureManager || !m_renderDevice) return;

    for (const auto& [handle, sprite] : m_sprites) {
        (void)handle;
        if (!sprite.visible || sprite.textureId == 0) continue;
        if (!m_textureManager->isValid(sprite.textureId) ||
            sprite.width == 0 || sprite.height == 0 ||
            !std::isfinite(sprite.scale) || sprite.scale <= 0.0f) {
            continue;
        }

        const uint32_t textureHandle =
            m_textureManager->getTextureHandle(sprite.textureId);

        const auto opacity = static_cast<uint8_t>(
            std::lround(sprite.opacity * 255.0f));
        m_renderDevice->blitTexture(
            VIEW_MAIN, textureHandle, sprite.x, sprite.y,
            static_cast<float>(sprite.width) * sprite.scale,
            static_cast<float>(sprite.height) * sprite.scale, opacity);
    }
}

bool NullAnimationBackend::playMotion(int /*handle*/, const std::string& /*name*/) {
    return false; // No animation in null backend
}

void NullAnimationBackend::setExpression(int /*handle*/, const std::string& /*name*/) {
    // No-op in null backend
}

void NullAnimationBackend::setParameter(int /*handle*/, const std::string& /*param*/, float /*value*/) {
    // No-op in null backend
}

bool NullAnimationBackend::setVoiceLipSync(int /*modelHandle*/, bool /*enabled*/) {
    return false; // Static sprites do not have Cubism mouth parameters.
}

} // namespace Caesura
