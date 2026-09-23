#include "TextureManager.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include <bimg/decode.h>
#include <bx/file.h>
#include <bx/allocator.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <new>
#include <vector>
#include <algorithm>

#include "../di/BackendRegistry.h"
#include "../di/api/ISandboxQuota.h"
#include "../di/api/ITextureBudget.h"
#include "../di/api/ThreadAssert.h"
#include <stb/stb_image.h>   // declarations only; impl lives in stb_impl.cpp

namespace Caesura {

class TextureManager::QuotaReservation {
public:
    QuotaReservation()
        : m_quota(BackendRegistry::instance().getSandboxQuota()) {
        m_allowed = !m_quota || m_quota->tryAlloc("textures");
        m_reserved = m_quota && m_allowed;
    }

    ~QuotaReservation() {
        if (m_reserved) {
            m_quota->release("textures");
        }
    }

    explicit operator bool() const {
        return m_allowed;
    }

    bool commit(std::unordered_map<uint32_t, ISandboxQuota*>& reservations,
                uint32_t id) noexcept {
        if (!m_allowed) {
            return false;
        }
        if (!m_reserved) {
            return true;
        }
        try {
            const auto [it, inserted] = reservations.emplace(id, m_quota);
            if (!inserted) {
                return false;
            }
        } catch (...) {
            return false;
        }
        m_reserved = false;
        return true;
    }

private:
    ISandboxQuota* m_quota = nullptr;
    bool m_allowed = false;
    bool m_reserved = false;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool TextureManager::initialize() {
    return initialize(true);
}

bool TextureManager::initialize(bool gpuAvailable) {
    if (m_initialized) return true;
    m_nextId = 1;
    m_cache.clear();
    m_textureDimensions.clear();
    m_gpuAvailable = gpuAvailable;
    if (m_gpuAvailable) {
        buildCheckerboardTexture();
    } else {
        printf("[TextureManager] Initialized without GPU placeholder.\n");
    }
    BackendRegistry::instance().registerDeviceLostListener(this);
    m_initialized = true;
    printf("[TextureManager] Initialized.\n");
    return true;
}

void TextureManager::shutdown() {
    if (!m_initialized) return;

    BackendRegistry::instance().unregisterDeviceLostListener(this);

    for (auto& [id, tex] : m_cache) {
        if (bgfx::isValid(tex))
            bgfx::destroy(tex);
    }
    for (const auto& [id, quota] : m_quotaReservations) {
        if (quota) {
            quota->release("textures");
        }
    }
    m_quotaReservations.clear();
    m_cache.clear();
    m_textureDimensions.clear();
    m_restoreSources.clear();
    m_textureLRU.clear();
    // P1-2 (audit g0_render): clear the dedup/path caches too — stale ids from
    // a previous session must not be re-returned after re-initialization.
    m_solidCache.clear();
    m_pathToId.clear();
    m_totalBytes = 0;
    m_gpuAvailable = true;
    if (bgfx::isValid(m_placeholderTex)) {
        bgfx::destroy(m_placeholderTex);
        m_placeholderTex = BGFX_INVALID_HANDLE;
    }

    m_initialized = false;
    printf("[TextureManager] Shutdown complete.\n");
}

// ---------------------------------------------------------------------------
// [10.2.57] Dev/Release mode placeholder selection
// ---------------------------------------------------------------------------

void TextureManager::setDevMode(bool dev) {
    if (m_devMode == dev) return;
    m_devMode = dev;
    if (!m_gpuAvailable) {
        printf("[TextureManager] Placeholder mode deferred until GPU is available: %s\n",
               dev ? "checkerboard (dev)" : "transparent (release)");
        return;
    }
    if (bgfx::isValid(m_placeholderTex)) {
        bgfx::destroy(m_placeholderTex);
        m_placeholderTex = BGFX_INVALID_HANDLE;
    }
    buildCheckerboardTexture();
    printf("[TextureManager] Placeholder mode: %s\n", dev ? "checkerboard (dev)" : "transparent (release)");
}


// ---------------------------------------------------------------------------
// Placeholder texture -- 16x16 purple/black checkerboard
// ---------------------------------------------------------------------------

bgfx::TextureHandle TextureManager::buildCheckerboardTexture() {
    if (!m_gpuAvailable)
        return BGFX_INVALID_HANDLE;
    if (bgfx::isValid(m_placeholderTex))
        return m_placeholderTex;

    const uint16_t size = 16;
    uint32_t data[size * size];
    if (m_devMode) {
        // Dev: purple/black checkerboard for visible debugging
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                bool white = ((x / 4) + (y / 4)) % 2 == 0;
                data[y * size + x] = white ? 0xFF8000FFu : 0xFF000000u;
            }
        }
    } else {
        // Release: fully transparent placeholder
        for (int i = 0; i < size * size; ++i) data[i] = 0x00000000u;
    }
    const bgfx::Memory* mem = bgfx::copy(data, sizeof(data));
    m_placeholderTex = bgfx::createTexture2D(
        size, size, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);

    if (!bgfx::isValid(m_placeholderTex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Failed to create placeholder texture.");
    } else {
        printf("[TextureManager] Placeholder texture created (16x16 %s).\n", m_devMode ? "checkerboard" : "transparent");
    }
    return m_placeholderTex;
}

uint32_t TextureManager::getPlaceholderTexture() {
    if (!m_gpuAvailable)
        return 0;
    if (!bgfx::isValid(m_placeholderTex))
        buildCheckerboardTexture();
    return m_placeholderTex.idx;
}

// ---------------------------------------------------------------------------
// Load from file
// ---------------------------------------------------------------------------

bgfx::TextureHandle TextureManager::loadFromFile(
    const std::string& path, uint16_t& width, uint16_t& height,
    std::vector<uint8_t>& encodedBytes) {
    width = 0;
    height = 0;
    encodedBytes.clear();
    // Try multiple base paths to support running from build directory
    const char* bases[] = { "", "../../", "../" };
    for (const char* base : bases) {
        std::string fullPath = std::string(base) + path;
        std::ifstream file(std::filesystem::path(std::u8string(fullPath.begin(), fullPath.end())), std::ios::binary | std::ios::ate);
        if (!file.is_open()) continue;
        std::streamsize sz = file.tellg();
        if (sz <= 0) continue;
        if (static_cast<uint64_t>(sz) >
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                      "[TextureManager] Texture file is too large: %s",
                      path.c_str());
            return BGFX_INVALID_HANDLE;
        }
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        if (!file.read(reinterpret_cast<char*>(buf.data()), sz)) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[TextureManager] Failed to read: %s",
                      path.c_str());
            return BGFX_INVALID_HANDLE;
        }
        bgfx::TextureHandle texture = loadFromMemory(
            buf.data(), static_cast<uint32_t>(buf.size()), width, height);
        if (bgfx::isValid(texture)) {
            encodedBytes = std::move(buf);
        }
        return texture;
    }
    DEBUG_ERR(SubSys::Render, ErrCode::Ok,
              "[TextureManager] File not found: %s", path.c_str());
    return BGFX_INVALID_HANDLE;
}

bool TextureManager::validateTextureDimensions(
    uint32_t width, uint32_t height, uint64_t& rgbaBytes) const {
    rgbaBytes = uint64_t(width) * uint64_t(height) * 4ULL;
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<uint16_t>::max() ||
        height > std::numeric_limits<uint16_t>::max() ||
        rgbaBytes > std::numeric_limits<uint32_t>::max()) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Unsupported texture dimensions: %ux%u.",
                  width, height);
        return false;
    }

    if (m_gpuAvailable) {
        const bgfx::Caps* caps = bgfx::getCaps();
        const uint32_t maxTextureSize =
            caps ? caps->limits.maxTextureSize : 0;
        if (maxTextureSize != 0 &&
            (width > maxTextureSize || height > maxTextureSize)) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[TextureManager] Texture %ux%u exceeds GPU limit %u.",
                      width, height, maxTextureSize);
            return false;
        }
    }
    return true;
}

bgfx::TextureHandle TextureManager::loadFromMemory(
    const uint8_t* data, uint32_t size, uint16_t& width, uint16_t& height) {
    static bx::DefaultAllocator allocator;
    width = 0;
    height = 0;
    if (!data || size == 0) {
        return BGFX_INVALID_HANDLE;
    }

    // Force RGBA8 — bimg may decode to R8/RGB8/etc without explicit format
    bimg::ImageContainer* img = bimg::imageParse(&allocator, data, size,
        bimg::TextureFormat::RGBA8);

    if (!img) {
        if (size > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                      "[TextureManager] Encoded texture exceeds decoder limit.");
            return BGFX_INVALID_HANDLE;
        }
        int iw = 0, ih = 0, channels = 0;
        unsigned char* stbData = stbi_load_from_memory(
            data, static_cast<int>(size), &iw, &ih, &channels, 4);
        if (!stbData) {
            DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                      "[TextureManager] Decode failed.");
            return BGFX_INVALID_HANDLE;
        }
        uint64_t rgbaBytes = 0;
        if (iw <= 0 || ih <= 0 ||
            !validateTextureDimensions(static_cast<uint32_t>(iw),
                                       static_cast<uint32_t>(ih),
                                       rgbaBytes)) {
            stbi_image_free(stbData);
            return BGFX_INVALID_HANDLE;
        }
        width = static_cast<uint16_t>(iw);
        height = static_cast<uint16_t>(ih);
        const bgfx::Memory* mem = bgfx::copy(
            stbData, static_cast<uint32_t>(rgbaBytes));
        stbi_image_free(stbData);
        return bgfx::createTexture2D(width, height, false, 1,
            bgfx::TextureFormat::RGBA8,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
    }

    uint64_t rgbaBytes = 0;
    const bool unsupportedLayout =
        img->m_depth > 1 || img->m_numLayers > 1 ||
        img->m_numMips > 1 || img->m_cubeMap;
    if (unsupportedLayout ||
        !validateTextureDimensions(img->m_width, img->m_height, rgbaBytes) ||
        img->m_size < rgbaBytes) {
        bimg::imageFree(img);
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Unsupported texture layout.");
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* mem = bgfx::copy(
        img->m_data, static_cast<uint32_t>(rgbaBytes));
    width = static_cast<uint16_t>(img->m_width);
    height = static_cast<uint16_t>(img->m_height);
    bimg::imageFree(img);
    return bgfx::createTexture2D(width, height, false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
}

bgfx::TextureHandle TextureManager::createFromRGBA(
    const uint8_t* rgba, uint16_t width, uint16_t height) {
    uint64_t rgbaBytes = 0;
    if (!rgba ||
        !validateTextureDimensions(width, height, rgbaBytes)) {
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem =
        bgfx::copy(rgba, static_cast<uint32_t>(rgbaBytes));
    return bgfx::createTexture2D(
        width, height, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem);
}

bgfx::TextureHandle TextureManager::restoreTexture(
    const RestoreSource& source, uint16_t& width, uint16_t& height) {
    width = 0;
    height = 0;
    if (source.bytes.empty()) {
        return BGFX_INVALID_HANDLE;
    }
    if (source.kind == RestoreSourceKind::Encoded) {
        if (source.bytes.size() >
            static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
            return BGFX_INVALID_HANDLE;
        }
        return loadFromMemory(
            source.bytes.data(), static_cast<uint32_t>(source.bytes.size()),
            width, height);
    }
    width = source.width;
    height = source.height;
    return createFromRGBA(source.bytes.data(), width, height);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Budget enforcement ([10.2.65])
// ---------------------------------------------------------------------------

void TextureManager::trackTexture(uint32_t id, uint64_t bytes) {
    untrackTexture(id);
    m_textureSizes[id] = bytes;
    m_totalBytes += bytes;
    m_textureLRU.push_front(id);
}

void TextureManager::untrackTexture(uint32_t id) {
    auto it = m_textureSizes.find(id);
    if (it != m_textureSizes.end()) {
        m_totalBytes -= it->second;
        m_textureSizes.erase(it);
    }
    m_textureLRU.remove(id);
}

bool TextureManager::checkBudget(uint32_t id, uint16_t w, uint16_t h) {
    const uint64_t bytes = uint64_t(w) * uint64_t(h) * 4ULL;
    uint64_t previousBytes = 0;
    const auto previous = m_textureSizes.find(id);
    if (previous != m_textureSizes.end()) {
        previousBytes = previous->second;
        untrackTexture(id);
    }

    auto* textureBudget = BackendRegistry::instance().getTextureBudget();
    if (!textureBudget) {
        trackTexture(id, bytes);
        return true;
    }
    const uint64_t budget = textureBudget->getBudgetBytes();
    if (bytes > budget) {
        if (previousBytes != 0) {
            trackTexture(id, previousBytes);
        }
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Texture requires %llu bytes, budget is %llu bytes.",
                  static_cast<unsigned long long>(bytes),
                  static_cast<unsigned long long>(budget));
        return false;
    }

    while (m_totalBytes > budget - bytes && !m_textureLRU.empty()) {
        const uint32_t victimId = m_textureLRU.back();
        const uint64_t requestedTotal =
            m_totalBytes > std::numeric_limits<uint64_t>::max() - bytes
                ? std::numeric_limits<uint64_t>::max()
                : m_totalBytes + bytes;
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Budget exceeded (%llu/%llu MB), evicting texture %u",
                  static_cast<unsigned long long>(requestedTotal / (1024 * 1024)),
                  static_cast<unsigned long long>(budget / (1024 * 1024)), victimId);
        destroyTexture(victimId);
    }

    if (m_totalBytes > budget - bytes) {
        if (previousBytes != 0) {
            trackTexture(id, previousBytes);
        }
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Cannot fit texture within %llu MB budget",
                  static_cast<unsigned long long>(budget / (1024 * 1024)));
        return false;
    }

    trackTexture(id, bytes);
    printf("[TextureManager] Budget: %llu / %llu MB (tier %d)\n",
           static_cast<unsigned long long>(m_totalBytes / (1024 * 1024)),
           static_cast<unsigned long long>(budget / (1024 * 1024)),
           textureBudget->getTier());
    return true;
}
// Public load API
// ---------------------------------------------------------------------------

uint32_t TextureManager::loadTexture(const std::string& path) {
    if (!m_initialized) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Not initialized.");
        return 0;
    }
    // Reject path traversal
    if (path.find("..") != std::string::npos) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Path traversal blocked: %s", path.c_str());
        return 0;
    }

    if (path.empty()) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Empty path.");
        return 0;

    }

    // Cache hit: same path already loaded and still alive -> reuse the id
    // (avoids re-decode + GPU upload for repeated backgrounds/sprites).
    auto hit = m_pathToId.find(path);
    if (hit != m_pathToId.end() && m_cache.count(hit->second)) {
        return hit->second;
    }

    QuotaReservation quotaReservation;
    if (!quotaReservation) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Texture quota exceeded: %s", path.c_str());
        return 0;
    }
    if (!m_gpuAvailable) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] GPU unavailable; cannot load texture: %s", path.c_str());
        return 0;
    }

    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint8_t> encodedBytes;
    RestoreSource restoreSource;
    bgfx::TextureHandle tex = BGFX_INVALID_HANDLE;
    try {
        restoreSource.assetPath = path;
        tex = loadFromFile(path, width, height, encodedBytes);
    } catch (const std::bad_alloc&) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Out of memory while loading: %s",
                  path.c_str());
        return 0;
    }
    if (!bgfx::isValid(tex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Failed to load: %s", path.c_str());
        return 0;
    }

    restoreSource.kind = RestoreSourceKind::Encoded;
    restoreSource.bytes = std::move(encodedBytes);
    restoreSource.width = width;
    restoreSource.height = height;
    const uint32_t id = registerTexture(
        tex, width, height, std::move(restoreSource), quotaReservation);
    if (id == 0) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Failed to register: %s", path.c_str());
        return 0;
    }
    printf("[TextureManager] Loaded: %s -> id=%u\n", path.c_str(), id);
    m_pathToId[path] = id;
    return id;
}

uint32_t TextureManager::loadTextureFromRGBA(const uint8_t* rgba, uint16_t w, uint16_t h,
                                             const std::string& cacheKey) {
    CAESURA_ASSERT_MAIN_THREAD();
    if (!m_initialized || !rgba || w == 0 || h == 0) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Invalid RGBA input.");
        return 0;
    }

    QuotaReservation quotaReservation;
    if (!quotaReservation) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Texture quota exceeded (RGBA).");
        return 0;
    }
    if (!m_gpuAvailable) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] GPU unavailable; cannot create RGBA texture.");
        return 0;
    }

    uint64_t rgbaBytes = 0;
    if (!validateTextureDimensions(w, h, rgbaBytes)) {
        return 0;
    }

    RestoreSource restoreSource;
    restoreSource.kind = RestoreSourceKind::Rgba;
    restoreSource.width = w;
    restoreSource.height = h;
    try {
        restoreSource.assetPath = cacheKey;
        restoreSource.bytes.assign(
            rgba, rgba + static_cast<size_t>(rgbaBytes));
    } catch (const std::bad_alloc&) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Out of memory copying RGBA texture.");
        return 0;
    }

    bgfx::TextureHandle tex =
        createFromRGBA(restoreSource.bytes.data(), w, h);
    if (!bgfx::isValid(tex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] GPU texture creation failed (RGBA).");
        return 0;
    }

    const uint32_t id = registerTexture(
        tex, w, h, std::move(restoreSource), quotaReservation);
    if (id == 0) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Failed to register RGBA texture.");
        return 0;
    }
    if (!cacheKey.empty()) {
        printf("[TextureManager] Loaded from RGBA (key=%s) -> id=%u\n", cacheKey.c_str(), id);
    } else {
        printf("[TextureManager] Loaded from RGBA -> id=%u\n", id);
    }
    return id;
}

uint32_t TextureManager::loadTextureFromMemory(const uint8_t* data, uint32_t size,
                                               const std::string& cacheKey) {
    if (!m_initialized) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Not initialized.");
        return 0;
    }
    if (!data || size == 0) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Invalid texture data.");
        return 0;
    }

    QuotaReservation quotaReservation;
    if (!quotaReservation) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Texture quota exceeded (memory).");
        return 0;
    }
    if (!m_gpuAvailable) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] GPU unavailable; cannot load texture from memory.");
        return 0;
    }

    RestoreSource restoreSource;
    restoreSource.kind = RestoreSourceKind::Encoded;
    try {
        restoreSource.assetPath = cacheKey;
        restoreSource.bytes.assign(data, data + static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Out of memory copying encoded texture.");
        return 0;
    }

    uint16_t width = 0;
    uint16_t height = 0;
    bgfx::TextureHandle tex = loadFromMemory(
        restoreSource.bytes.data(),
        static_cast<uint32_t>(restoreSource.bytes.size()), width, height);
    if (!bgfx::isValid(tex)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_TextureCreateFailed,
                  "[TextureManager] Failed to load from memory.");
        return 0;
    }

    restoreSource.width = width;
    restoreSource.height = height;
    const uint32_t id = registerTexture(
        tex, width, height, std::move(restoreSource), quotaReservation);
    if (id == 0) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Failed to register memory texture.");
        return 0;
    }
    if (!cacheKey.empty()) {
        printf("[TextureManager] Loaded from memory (key=%s) -> id=%u\n",
               cacheKey.c_str(), id);
    } else {
        printf("[TextureManager] Loaded from memory -> id=%u\n", id);
    }
    return id;
}

// ---------------------------------------------------------------------------
// Solid colour texture — creates bgfx texture + registers in cache.
// Merged createSolidTexture + registerTexture into one call returning TM ID.
// ---------------------------------------------------------------------------

uint32_t TextureManager::createSolidTexture(uint8_t r, uint8_t g,
                                             uint8_t b, uint8_t a) {
    if (!m_initialized) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Not initialized.");
        return 0;
    }
    // Solid-color dedup: UIs re-request the same RGBA every open (settings,
    // history, gallery, flash effects). Cache the registered id so repeated
    // calls cost zero GPU allocations/uploads (and never leak textures).
    const uint32_t key = static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
                         (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
    auto cached = m_solidCache.find(key);
    if (cached != m_solidCache.end()) {
        return cached->second;
    }
    QuotaReservation quotaReservation;
    if (!quotaReservation) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] Texture quota exceeded (solid).");
        return 0;
    }
    if (!m_gpuAvailable) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextureManager] GPU unavailable; cannot create solid texture.");
        return 0;
    }
    RestoreSource restoreSource;
    restoreSource.kind = RestoreSourceKind::Rgba;
    restoreSource.bytes = {r, g, b, a};
    restoreSource.width = 1;
    restoreSource.height = 1;
    bgfx::TextureHandle tex =
        createFromRGBA(restoreSource.bytes.data(), 1, 1);
    const uint32_t id = registerTexture(
        tex, 1, 1, std::move(restoreSource), quotaReservation);
    if (id != 0) {
        m_solidCache[key] = id;
    }
    return id;
}

// ---------------------------------------------------------------------------
// Register externally-created texture (private helper)
// ---------------------------------------------------------------------------

uint32_t TextureManager::registerTexture(bgfx::TextureHandle tex,
                                         uint16_t width, uint16_t height,
                                         RestoreSource&& restoreSource,
                                         QuotaReservation& quotaReservation) {
    if (!bgfx::isValid(tex)) {
        return 0;
    }
    if (restoreSource.bytes.empty() ||
        restoreSource.width != width ||
        restoreSource.height != height) {
        bgfx::destroy(tex);
        return 0;
    }

    const uint32_t id = m_nextId++;
    try {
        const bool cacheInserted = m_cache.emplace(id, tex).second;
        const bool dimensionsInserted =
            m_textureDimensions.emplace(id, std::make_pair(width, height)).second;
        const bool sourceInserted =
            m_restoreSources.emplace(id, std::move(restoreSource)).second;
        if (!cacheInserted || !dimensionsInserted || !sourceInserted) {
            m_cache.erase(id);
            m_textureDimensions.erase(id);
            m_restoreSources.erase(id);
            bgfx::destroy(tex);
            return 0;
        }
    } catch (...) {
        m_cache.erase(id);
        m_textureDimensions.erase(id);
        m_restoreSources.erase(id);
        bgfx::destroy(tex);
        return 0;
    }

    if (!quotaReservation.commit(m_quotaReservations, id)) {
        m_cache.erase(id);
        m_textureDimensions.erase(id);
        m_restoreSources.erase(id);
        bgfx::destroy(tex);
        return 0;
    }
    bool accepted = false;
    try {
        accepted = checkBudget(id, width, height);
    } catch (...) {
        // Registration already owns the GPU handle and quota. A failed
        // accounting allocation/backend call must retire that ownership here;
        // the caller has not received an ID it could destroy.
    }
    if (!accepted) {
        destroyTexture(id);
        return 0;
    }
    return id;
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------

void TextureManager::destroyTexture(uint32_t id) {
    auto it = m_cache.find(id);
    const bool hadTexture = it != m_cache.end();
    if (hadTexture) {
        if (bgfx::isValid(it->second))
            bgfx::destroy(it->second);
        m_cache.erase(it);
    }

    untrackTexture(id);
    m_textureDimensions.erase(id);
    for (auto it = m_pathToId.begin(); it != m_pathToId.end();) {
        if (it->second == id) it = m_pathToId.erase(it);
        else ++it;
    }
    // A destroyed color must not remain a successful dedup hit. Keep other
    // live colors cached; a later request for this color allocates a fresh ID.
    for (auto it = m_solidCache.begin(); it != m_solidCache.end();) {
        if (it->second == id) it = m_solidCache.erase(it);
        else ++it;
    }
    const bool hadRestoreSource = m_restoreSources.erase(id) != 0;
    bool hadQuota = false;
    const auto quotaReservation = m_quotaReservations.find(id);
    if (quotaReservation != m_quotaReservations.end()) {
        ISandboxQuota* quota = quotaReservation->second;
        m_quotaReservations.erase(quotaReservation);
        if (quota) {
            quota->release("textures");
        }
        hadQuota = true;
    }
    if (hadTexture || hadRestoreSource || hadQuota) {
        printf("[TextureManager] Texture %u destroyed.\n", id);
    }
}

// ---------------------------------------------------------------------------
// Lookup — returns raw bgfx TextureHandle.idx as uint32_t
// ---------------------------------------------------------------------------

uint32_t TextureManager::getTextureHandle(uint32_t id) const {
    auto it = m_cache.find(id);
    if (it != m_cache.end() && bgfx::isValid(it->second))
        return it->second.idx;
    return 0;
}

bool TextureManager::describeTexture(uint32_t id, TextureSourceInfo& output) const {
    const auto found = m_restoreSources.find(id);
    if (found == m_restoreSources.end()) return false;
    const auto& source = found->second;
    TextureSourceInfo description;
    if (!source.assetPath.empty()) {
        description.path = source.assetPath;
    } else if (source.kind == RestoreSourceKind::Rgba && source.width == 1
               && source.height == 1 && source.bytes.size() == 4) {
        description.kind = TextureSourceKind::Color;
        std::copy(source.bytes.begin(), source.bytes.end(), description.color.begin());
    } else {
        return false;
    }
    output = std::move(description);
    return true;
}

bool TextureManager::isValid(uint32_t id) const {
    auto it = m_cache.find(id);
    return it != m_cache.end() && bgfx::isValid(it->second);
}

// ---------------------------------------------------------------------------
// Size query (private — takes bgfx handle)
// ---------------------------------------------------------------------------

void TextureManager::getTextureSizeById(uint32_t id,
                                         uint16_t& width, uint16_t& height) const {
    const auto texture = m_cache.find(id);
    const auto dimensions = m_textureDimensions.find(id);
    if (texture != m_cache.end() &&
        dimensions != m_textureDimensions.end()) {
        width = dimensions->second.first;
        height = dimensions->second.second;
        return;
    }
    width = 0; height = 0;
}

// ---------------------------------------------------------------------------
// IDeviceLostListener — GPU device loss recovery
// ---------------------------------------------------------------------------

void TextureManager::onDeviceLost() {
    m_gpuAvailable = false;
    const size_t texCount = m_cache.size();
    std::vector<uint32_t> unrecoverableIds;
    unrecoverableIds.reserve(m_cache.size());
    for (auto& [id, handle] : m_cache) {
        if (bgfx::isValid(handle)) {
            bgfx::destroy(handle);
        }
        handle = BGFX_INVALID_HANDLE;
        if (m_restoreSources.find(id) == m_restoreSources.end()) {
            unrecoverableIds.push_back(id);
        }
    }
    for (const uint32_t id : unrecoverableIds) {
        destroyTexture(id);
    }
    if (bgfx::isValid(m_placeholderTex)) {
        bgfx::destroy(m_placeholderTex);
    }
    m_placeholderTex = BGFX_INVALID_HANDLE;
    printf("[TextureManager] Device lost - %zu GPU textures released, %zu recoverable.\n",
           texCount, m_restoreSources.size());
}

void TextureManager::onDeviceRestored() {
    m_gpuAvailable = true;
    buildCheckerboardTexture();

    std::vector<uint32_t> ids;
    ids.reserve(m_restoreSources.size());
    for (const auto& [id, source] : m_restoreSources) {
        ids.push_back(id);
    }

    size_t restoredCount = 0;
    size_t droppedCount = 0;
    for (const uint32_t id : ids) {
        auto cache = m_cache.find(id);
        const auto source = m_restoreSources.find(id);
        if (cache == m_cache.end() || source == m_restoreSources.end()) {
            destroyTexture(id);
            ++droppedCount;
            continue;
        }
        if (bgfx::isValid(cache->second)) {
            ++restoredCount;
            continue;
        }

        uint16_t width = 0;
        uint16_t height = 0;
        bgfx::TextureHandle restored =
            restoreTexture(source->second, width, height);
        if (!bgfx::isValid(restored)) {
            destroyTexture(id);
            ++droppedCount;
            continue;
        }

        const auto dimensions = m_textureDimensions.find(id);
        const bool dimensionsChanged =
            dimensions == m_textureDimensions.end() ||
            dimensions->second.first != width ||
            dimensions->second.second != height;
        const bool missingBudgetRecord =
            m_textureSizes.find(id) == m_textureSizes.end();
        if (dimensionsChanged || missingBudgetRecord) {
            untrackTexture(id);
            if (!checkBudget(id, width, height)) {
                bgfx::destroy(restored);
                destroyTexture(id);
                ++droppedCount;
                continue;
            }
        }

        cache = m_cache.find(id);
        if (cache == m_cache.end()) {
            bgfx::destroy(restored);
            ++droppedCount;
            continue;
        }
        cache->second = restored;
        m_textureDimensions[id] = {width, height};
        ++restoredCount;
    }

    printf("[TextureManager] Device restored - %zu textures restored, %zu dropped.\n",
           restoredCount, droppedCount);
}

} // namespace Caesura
