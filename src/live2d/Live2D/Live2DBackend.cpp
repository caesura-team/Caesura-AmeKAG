#ifdef CAESURA_LIVE2D

#include "Live2DBackend.h"

// Cubism Framework
#include <CubismFramework.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Model/CubismUserModel.hpp>
#include <ICubismModelSetting.hpp>
#include <CubismModelSettingJson.hpp>
#include <stb_image.h>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismMotionQueueManager.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Rendering/CubismRenderer.hpp>
#ifdef _WIN32
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Rendering/D3D11/CubismDeviceInfo_D3D11.hpp>
#include <d3d11.h>
#else
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#endif

// Engine
#include "Live2DUserModel.h"
#include "ILive2DRenderPath.h"
#include "../PathConfinement.h"
#include "../../render/api/IRenderDevice.h"
#include "debug/api/DebugLog.h"
#ifdef _WIN32
#include "D3D11NativeRenderPath.h"
#else
#include "OpenGLReadbackRenderPath.h"
#include "OpenGLSharedRenderPath.h"
#ifdef __APPLE__
#include "MetalNativeRenderPath.h"
#endif
#endif

#include <fstream>
#include <vector>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <limits>
#include <SDL3/SDL.h>

namespace Caesura {

using namespace Csm;
using namespace Csm::Rendering;
using namespace Live2D::Cubism::Core;

// ============================================================
// File helpers
// ============================================================

static std::vector<char> readFile(const std::string& path) {
    const std::string confined = confineToModelRoot(path);
    if (confined.empty()) return {};
    // Cap the size before allocating: a malicious (or zip-bomb style) file
    // inside the root must not force a multi-GB allocation or a narrowing
    // truncation downstream.
    constexpr uint64_t kMaxModelFileBytes = 256ull * 1024ull * 1024ull;
    std::ifstream file(confined, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const uint64_t size = static_cast<uint64_t>(file.tellg());
    if (size == 0 || size > kMaxModelFileBytes) return {};
    file.seekg(0);
    std::vector<char> data(static_cast<size_t>(size));
    file.read(data.data(), static_cast<std::streamsize>(size));
    if (!file.good()) return {};
    return data;
}

static std::string dirName(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos + 1);
}

static std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

// ============================================================
// Cubism allocator
// ============================================================
namespace {
    class EngineAllocator : public ICubismAllocator {
    public:
        void* Allocate(const csmSizeType size) override { return SDL_malloc(size); }
        void  Deallocate(void* memory) override { SDL_free(memory); }
        void* AllocateAligned(const csmSizeType size, csmUint32 alignment) override {
            return SDL_aligned_alloc(alignment, size);
        }
        void  DeallocateAligned(void* alignedMemory) override { SDL_aligned_free(alignedMemory); }
    };

    static void cubismLog(const csmChar* message) {
        DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] %s", message);
    }

    // CubismFramework file loader (used for FrameworkShaders/*.fx).
    static csmByte* cubismLoadFile(const std::string filePath, csmSizeInt* size) {
        auto data = readFile(filePath);  // confined to model root inside readFile
        if (data.empty()) {
            if (size) *size = 0;
            return nullptr;
        }
        // shaders/motions are small; cap to avoid csmSizeInt truncation and
        // unbounded allocation from a malicious file.
        constexpr size_t kMaxCubismFileBytes = 64u * 1024u * 1024u;
        if (data.size() > kMaxCubismFileBytes ||
            data.size() > static_cast<size_t>(std::numeric_limits<csmSizeInt>::max())) {
            if (size) *size = 0;
            return nullptr;
        }
        csmByte* buf = static_cast<csmByte*>(SDL_malloc(data.size()));
        if (!buf) {
            if (size) *size = 0;
            return nullptr;
        }
        std::memcpy(buf, data.data(), data.size());
        if (size) *size = static_cast<csmSizeInt>(data.size());
        return buf;
    }

    static void cubismReleaseBytes(csmByte* buffer) {
        SDL_free(buffer);
    }

    // Create an RGBA8 bgfx texture from decoded pixels (model texture).
    static bgfx::TextureHandle createBgfxTexture(int width, int height,
                                                 const unsigned char* pixels) {
        if (width <= 0 || height <= 0 || !pixels) return BGFX_INVALID_HANDLE;
        // stb dimensions come from an untrusted texture header: validate before
        // narrowing to uint16 / computing the block size (no wrap, no truncation).
        constexpr int kMaxTextureDim = 0x7FFF;  // createTexture2D takes uint16
        constexpr uint64_t kMaxTextureBytes = 256ull * 1024ull * 1024ull;
        if (width > kMaxTextureDim || height > kMaxTextureDim) return BGFX_INVALID_HANDLE;
        const uint64_t total = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
        if (total == 0 || total > kMaxTextureBytes) return BGFX_INVALID_HANDLE;
        const bgfx::Memory* mem = bgfx::copy(pixels, static_cast<uint32_t>(total));
        return bgfx::createTexture2D(static_cast<uint16_t>(width),
            static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT, mem);
    }
}

// ============================================================
// Live2DModel destructor
// ============================================================
Live2DBackend::Live2DModel::~Live2DModel() {
    if (setting) {
        delete setting;
        setting = nullptr;
    }
    // renderer is owned by CubismUserModel (~CubismUserModel calls DeleteRenderer);
    // an explicit CubismRenderer::Delete here would double-free.
    userModel.reset();
    if (bgfxTexValid && bgfx::isValid(bgfxTex)) {
        bgfx::destroy(bgfxTex);
        bgfxTexValid = false;
    }
    for (bgfx::TextureHandle tex : textures) {
        if (bgfx::isValid(tex)) bgfx::destroy(tex);
    }
    textures.clear();
#ifdef _WIN32
    for (ID3D11ShaderResourceView* srv : textureSrvs) {
        if (srv) srv->Release();
    }
    textureSrvs.clear();
#endif
}

// ============================================================
// init / shutdown
// ============================================================
bool Live2DBackend::init() {
    const auto rendererType = bgfx::getRendererType();
#ifdef _WIN32
    if (rendererType != bgfx::RendererType::Direct3D11) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D] Windows build requires the bgfx D3D11 renderer");
        return false;
    }
#elif defined(__APPLE__)
    if (rendererType != bgfx::RendererType::OpenGL &&
        rendererType != bgfx::RendererType::OpenGLES &&
        rendererType != bgfx::RendererType::Metal) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D] Unsupported macOS bgfx renderer: %s",
            bgfx::getRendererName(rendererType));
        return false;
    }
#else
    if (rendererType != bgfx::RendererType::OpenGL &&
        rendererType != bgfx::RendererType::OpenGLES) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D] Linux build requires the bgfx OpenGL renderer");
        return false;
    }
#endif

    static EngineAllocator allocator;
    static CubismFramework::Option option;
    option.LogFunction = cubismLog;
    option.LoggingLevel = CubismFramework::Option::LogLevel_Verbose;
    option.LoadFileFunction = cubismLoadFile;
    option.ReleaseBytesFunction = cubismReleaseBytes;

    if (!CubismFramework::StartUp(&allocator, &option)) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D] StartUp failed");
        return false;
    }
    CubismFramework::Initialize();
    m_initialized = true;

#ifdef _WIN32
    auto* d3dPath = new D3D11NativeRenderPath();
    if (!d3dPath->init(1280, 720)) {
        delete d3dPath;
        shutdown();
        return false;
    }
    delete m_renderPath;
    m_renderPath = d3dPath;
#else
#ifdef __APPLE__
    if (rendererType == bgfx::RendererType::Metal) {
        auto* metalPath = new MetalNativeRenderPath();
        if (!metalPath->init(1280, 720)) {
            delete metalPath;
            shutdown();
            return false;
        }
        delete m_renderPath;
        m_renderPath = metalPath;
    } else
#endif
    {
        ILive2DRenderPath* glPath = new OpenGLSharedRenderPath();
        if (!glPath->init(1280, 720)) {
            delete glPath;
            auto* readbackPath = new OpenGLReadbackRenderPath();
            if (!readbackPath->init(1280, 720)) {
                delete readbackPath;
                shutdown();
                return false;
            }
            glPath = readbackPath;
        }
        delete m_renderPath;
        m_renderPath = glPath;
    }
#endif

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] CubismFramework 5 initialized (render path: %s)", m_renderPath->name());
    return true;
}

Live2DBackend::~Live2DBackend() = default;

void Live2DBackend::shutdown() {
    clearModels();
    if (m_renderPath) {
        m_renderPath->shutdown();
        delete m_renderPath;
        m_renderPath = nullptr;
    }
    if (m_initialized) {
#ifdef _WIN32
        // CubismFramework::Dispose() does not release the D3D11 render-state
        // / shader objects held in the static device-info map; release them
        // while the device is still alive.
        CubismDeviceInfo_D3D11::ReleaseAllDeviceInfo();
#endif
        CubismFramework::Dispose();
        m_initialized = false;
    }
    m_deviceReady = false;
}

void Live2DBackend::setRenderDevice(IRenderDevice* device) {
    m_renderDevice = device;
    m_deviceReady = (device != nullptr);
}

// ============================================================
// Model loading (Cubism 5 API)
// ============================================================
bool Live2DBackend::loadModelInternal(Live2DModel& model) {
    std::string dir = dirName(model.dir);

    // 1. Load .model3.json
    model.settingJson = readFile(model.dir);
    if (model.settingJson.empty()) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D] Cannot read: %s", model.dir.c_str());
        // The .model3.json read failed: bail out before allocating model.setting.
        // model.setting is still null here, so the null guard in ~Live2DModel()
        // handles cleanup on the way out.
        return false;
    }
    model.setting = new CubismModelSettingJson(
        reinterpret_cast<const csmByte*>(model.settingJson.data()),
        static_cast<csmSizeInt>(model.settingJson.size())
    );

    // 2. Load .moc3
    std::string mocPath = joinPath(dir, model.setting->GetModelFileName());
    model.mocData = readFile(mocPath);
    if (model.mocData.empty()) return false;

    // 3. Create user model (Live2DUserModel for protected member access)
    model.userModel = std::make_unique<Live2DUserModel>();
    model.userModel->LoadModel(
        reinterpret_cast<const csmByte*>(model.mocData.data()),
        static_cast<csmSizeInt>(model.mocData.size())
    );

    // 4. Create renderer + bgfx texture
    if (!createRenderer(model)) return false;

    // 5. Load textures
    model.textures.resize(static_cast<size_t>(model.setting->GetTextureCount()),
                          BGFX_INVALID_HANDLE);
    for (csmInt32 i = 0; i < model.setting->GetTextureCount(); ++i) {
        std::string texPath = joinPath(dir, model.setting->GetTextureFileName(i));
        auto texData = readFile(texPath);
        if (!texData.empty()) {
            // Validate the image header before decoding: D3D11 caps at 16384
            // and the decoded w*h*4 must stay within the texture budget.
            int iw = 0, ih = 0, ic = 0;
            if (!stbi_info_from_memory(
                    reinterpret_cast<const stbi_uc*>(texData.data()),
                    static_cast<int>(texData.size()), &iw, &ih, &ic)) {
                DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Texture %d: invalid image header: %s", i, texPath.c_str());
                continue;
            }
            constexpr int kMaxTextureDim = 16384;
            constexpr uint64_t kMaxTextureBytes = 256ull * 1024ull * 1024ull;
            if (iw <= 0 || ih <= 0 || iw > kMaxTextureDim || ih > kMaxTextureDim ||
                static_cast<uint64_t>(iw) * static_cast<uint64_t>(ih) * 4u > kMaxTextureBytes) {
                DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Texture %d: dimensions out of range: %dx%d", i, iw, ih);
                continue;
            }
            // Create Cubism texture from loaded PNG data
            int w, h, comp;
            unsigned char* pixels = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(texData.data()),
                static_cast<int>(texData.size()), &w, &h, &comp, 4);
            if (pixels) {
#ifdef _WIN32
                // D3D11: hand the model texture to Cubism as a shader-resource view.
                if (m_renderPath) {
                    auto* d3dPath = static_cast<D3D11NativeRenderPath*>(m_renderPath);
                    ID3D11ShaderResourceView* srv = d3dPath->createModelTexture(w, h, pixels);
                    if (srv) {
                        model.textureSrvs.push_back(srv);  // owned by the model
                        if (model.renderer) {
                            static_cast<CubismRenderer_D3D11*>(model.renderer)->BindTexture(i, srv);
                        }
                    }
                }
#else
                model.textures[i] = createBgfxTexture(w, h, pixels);
#endif
                stbi_image_free(pixels);
                DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Texture %d loaded: %dx%d", i, w, h);
            } else {
                DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Texture %d failed to decode: %s", i, texPath.c_str());
            }
        }
    }

    // 7. Cache expressions
    if (model.setting->GetExpressionCount() > 0) {
        for (csmInt32 i = 0; i < model.setting->GetExpressionCount(); ++i) {
            std::string exprName = model.setting->GetExpressionName(i);
            std::string exprPath = joinPath(dir, model.setting->GetExpressionFileName(i));
            auto exprData = readFile(exprPath);
            if (!exprData.empty()) {
                model.expressionCache[exprName] = std::move(exprData);
            }
        }
    }

    // 7b. Cache motions (P1-1: motionCache was never populated, so
    // playMotion() could never find any clip).
    const csmInt32 motionGroupCount = model.setting->GetMotionGroupCount();
    if (motionGroupCount > 0) {
        for (csmInt32 i = 0; i < motionGroupCount; ++i) {
            const char* groupName = model.setting->GetMotionGroupName(i);
            if (!groupName) continue;
            const std::string group(groupName);
            const csmInt32 groupMotionCount = model.setting->GetMotionCount(groupName);
            for (csmInt32 j = 0; j < groupMotionCount; ++j) {
                const char* fileName = model.setting->GetMotionFileName(groupName, j);
                if (!fileName) continue;
                std::string motionPath = joinPath(dir, fileName);
                auto motionData = readFile(motionPath);
                if (motionData.empty()) continue;
                // Key the clip by its file stem and by "group/index" so
                // playMotion(name) and playMotion("group/index") both hit.
                const std::string stem = std::filesystem::path(fileName).stem().string();
                if (model.motionCache.find(stem) == model.motionCache.end()) {
                    model.motionCache[stem] = motionData;
                }
                model.motionCache[group + "/" + std::to_string(j)] = motionData;
            }
        }
    }

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Model loaded: %s", model.name.c_str());
    return true;
}

bool Live2DBackend::createRenderer(Live2DModel& model) {
    if (!model.userModel) return false;

    model.userModel->CreateRenderer(model.renderWidth, model.renderHeight);
#ifdef _WIN32
    model.renderer = model.userModel->GetRenderer<CubismRenderer_D3D11>();
#else
    model.renderer = model.userModel->GetRenderer<CubismRenderer_OpenGLES2>();
#endif

    if (!model.renderer) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D] Failed to create OpenGL renderer");
        return false;
    }

    // Create bgfx texture for output
    model.bgfxTex = bgfx::createTexture2D(
        model.renderWidth, model.renderHeight,
        false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_POINT
    );
    model.bgfxTexValid = bgfx::isValid(model.bgfxTex);

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D] Renderer created (%dx%d)", model.renderWidth, model.renderHeight);
    return true;
}

// ============================================================
// Per-frame: Cubism render via the pluggable render path to bgfx
// ============================================================
void Live2DBackend::render(float dt) {
    if (!m_renderPath) return;
    for (auto& [handle, model] : m_models) {
        if (!model->visible || !model->renderer || !model->userModel) continue;

        auto* cubismModel = model->userModel->GetModel();
        if (!cubismModel) continue;

        // Update model (motions, expressions)
        static_cast<Live2DUserModel*>(model->userModel.get())->motionManager()->UpdateMotion(cubismModel, dt);
        static_cast<Live2DUserModel*>(model->userModel.get())->expressionManager()->UpdateMotion(cubismModel, dt);
        // Recompute model vertices/deformations before drawing (csmUpdateModel).
        cubismModel->Update();

        // Cubism render to bgfx (via pluggable render path)
        m_renderPath->beginFrame(static_cast<CubismRenderer*>(model->renderer));
        m_renderPath->endFrame(static_cast<CubismRenderer*>(model->renderer), model->bgfxTex);

        // Blit bgfx texture to screen
        if (m_renderDevice && model->bgfxTexValid) {
            m_renderDevice->blitTexture(0, model->bgfxTex.idx,
                model->x, model->y,
                static_cast<float>(model->renderWidth)  * model->scale,
                static_cast<float>(model->renderHeight) * model->scale,
                static_cast<uint8_t>(model->opacity * 255.0f));
        }
    }
}

// ============================================================
// Motion playback (Cubism 5 API)
// ============================================================
bool Live2DBackend::playMotion(int handle, const std::string& name) {
    auto it = m_models.find(handle);
    if (it == m_models.end() || !it->second->userModel || !it->second->setting) return false;

    auto& model = *it->second;

    auto mit = model.motionCache.find(name);
    if (mit == model.motionCache.end()) {
        // Fallback: substring match over cached keys (e.g. "wave" -> "wave/0").
        for (auto it = model.motionCache.begin(); it != model.motionCache.end(); ++it) {
            if (it->first.find(name) != std::string::npos) { mit = it; break; }
        }
    }
    if (mit == model.motionCache.end()) {
        DEBUG_WARN(SubSys::Live2D, ErrCode::Ok, "[Live2D] Motion not found: %s", name.c_str());
        return false;
    }

    auto& data = mit->second;
    auto* motion = model.userModel->LoadMotion(
        reinterpret_cast<const csmByte*>(data.data()),
        static_cast<csmSizeInt>(data.size()),
        name.c_str(),
        nullptr, nullptr,
        model.setting
    );
    if (!motion) return false;

    static_cast<Live2DUserModel*>(model.userModel.get())->motionManager()->StartMotion(motion, false);
    return true;
}

// ============================================================
// Expression
// ============================================================
void Live2DBackend::setExpression(int handle, const std::string& name) {
    auto it = m_models.find(handle);
    if (it == m_models.end() || !it->second->userModel) return;

    auto& model = *it->second;
    auto eit = model.expressionCache.find(name);
    if (eit == model.expressionCache.end()) return;

    auto& data = eit->second;
    auto* expression = model.userModel->LoadExpression(
        reinterpret_cast<const csmByte*>(data.data()),
        static_cast<csmSizeInt>(data.size()),
        name.c_str()
    );
    if (!expression) return;

    static_cast<Live2DUserModel*>(model.userModel.get())->expressionManager()->StartMotion(expression, false);
}

// ============================================================
// Parameter control (Cubism 5 API)
// ============================================================
void Live2DBackend::setParameter(int handle, const std::string& param, float value) {
    auto it = m_models.find(handle);
    if (it == m_models.end() || !it->second->userModel) return;
    auto* cubismModel = it->second->userModel->GetModel();
    if (!cubismModel) return;
    auto* rawModel = cubismModel->GetModel();
    if (!rawModel) return;

    csmInt32 count = csmGetParameterCount(rawModel);
    const char** ids = csmGetParameterIds(rawModel);
    float* values = csmGetParameterValues(rawModel);
    for (csmInt32 i = 0; i < count; ++i) {
        if (ids[i] && param == ids[i]) {
            values[i] = value;
            return;
        }
    }
}

// ============================================================
// Model lifecycle
// ============================================================
int Live2DBackend::loadModel(const std::string& path, const std::string& name) {
    const std::string confined = confineToModelRoot(path);
    if (confined.empty()) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D] Model path outside model root: %s", path.c_str());
        return -1;
    }
    int handle = m_nextHandle++;
    auto model = std::make_unique<Live2DModel>();
    model->dir = confined;
    model->name = name;
    if (!loadModelInternal(*model)) {
#ifdef _WIN32
        releaseModelTarget(*model);
#endif
        return -1;
    }
    m_models[handle] = std::move(model);
    return handle;
}

void Live2DBackend::unloadModel(int handle) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;
#ifdef _WIN32
    releaseModelTarget(*it->second);
#endif
    m_models.erase(handle);
}

#ifdef _WIN32
void Live2DBackend::releaseModelTarget(Live2DModel& model) {
    if (m_renderPath && model.renderer) {
        static_cast<D3D11NativeRenderPath*>(m_renderPath)->releaseModelTarget(
            static_cast<CubismRenderer*>(model.renderer));
    }
}
#endif
bool Live2DBackend::isLoaded(int handle) const { return m_models.count(handle) > 0; }

std::size_t Live2DBackend::loadedModelCount() const {
    return m_models.size();
}

void Live2DBackend::clearModels() {
    while (!m_models.empty()) {
        unloadModel(m_models.begin()->first);
    }
}

void Live2DBackend::showModel(int handle, float x, float y, float scale) {
    auto it = m_models.find(handle);
    if (it == m_models.end()) return;
    it->second->visible = true;
    it->second->x = x; it->second->y = y; it->second->scale = scale;
}

void Live2DBackend::hideModel(int handle) {
    auto it = m_models.find(handle);
    if (it != m_models.end()) it->second->visible = false;
}

void Live2DBackend::setOpacity(int handle, float opacity) {
    auto it = m_models.find(handle);
    if (it != m_models.end()) it->second->opacity = opacity;
}

} // namespace Caesura

#endif
