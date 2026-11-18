#if defined(CAESURA_LIVE2D) && defined(__linux__) && !defined(__ANDROID__)

#include "OpenGLReadbackRenderPath.h"

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <CubismFramework.hpp>
#include <Model/CubismUserModel.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include "debug/api/DebugLog.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <GL/glx.h>

namespace Caesura {
namespace {

using CsmRendering::CubismRenderer;
using CsmRendering::CubismRenderer_OpenGLES2;
using Live2D::Cubism::Framework::CubismFramework;
using Live2D::Cubism::Framework::CubismUserModel;

// SDL only tracks the contexts it made current. bgfx can also leave a native
// GLX or EGL context current on the engine thread.
class CurrentContext {
public:
    CurrentContext() {
        if (m_previousContext) return;
        m_eglApi = eglQueryAPI();
        m_glxContext = glXGetCurrentContext();
        if (m_glxContext) {
            m_glxDisplay = glXGetCurrentDisplay();
            m_glxDraw = glXGetCurrentDrawable();
            m_glxRead = glXGetCurrentReadDrawable();
        }
        m_eglContext = eglGetCurrentContext();
        if (m_eglContext != EGL_NO_CONTEXT) {
            m_eglDisplay = eglGetCurrentDisplay();
            m_eglDraw = eglGetCurrentSurface(EGL_DRAW);
            m_eglRead = eglGetCurrentSurface(EGL_READ);
        }
    }
    CurrentContext(const CurrentContext&) = delete;
    CurrentContext& operator=(const CurrentContext&) = delete;

    bool prepare() {
        if (m_previousContext) {
            m_restore = true;
            return true;
        }
        const bool hasEgl = m_eglContext != EGL_NO_CONTEXT;
        if ((m_glxContext && hasEgl)
            || (!m_glxContext && !hasEgl && glGetString(GL_VERSION) != nullptr)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Unexpected current native context");
            return false;
        }
        if (m_glxContext &&
            (!m_glxDisplay || !glXMakeContextCurrent(m_glxDisplay, None, None, nullptr))) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not release prior GLX context");
            return false;
        }
        if (hasEgl && (m_eglDisplay == EGL_NO_DISPLAY ||
            !eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not release prior EGL context");
            return false;
        }
        m_restore = true;
        return true;
    }

    bool activate(SDL_Window* window, SDL_GLContext context) {
        if (!window || !context) return false;
        if (m_previousContext == context) return true;
        if (!prepare()) return false;
        if (SDL_GL_MakeCurrent(window, context)) return true;
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not activate Cubism context: %s",
            SDL_GetError());
        return false;
    }

    ~CurrentContext() {
        if (!m_restore) return;
        if (m_previousContext) {
            if (m_previousContext != SDL_GL_GetCurrentContext() &&
                !SDL_GL_MakeCurrent(m_previousWindow, m_previousContext)) {
                DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not restore prior SDL context: %s",
                    SDL_GetError());
            }
            return;
        }
        // Drop SDL's thread-local ownership before restoring a native context.
        if (SDL_GL_GetCurrentContext() && !SDL_GL_MakeCurrent(nullptr, nullptr)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not release Cubism context: %s",
                SDL_GetError());
            return;
        }
        if (m_eglApi != EGL_NONE && m_eglApi != eglQueryAPI() && !eglBindAPI(m_eglApi)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not restore prior EGL client API");
        }
        if (m_glxContext && !glXMakeContextCurrent(m_glxDisplay, m_glxDraw, m_glxRead, m_glxContext)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not restore prior GLX context");
        }
        if (m_eglContext != EGL_NO_CONTEXT &&
            !eglMakeCurrent(m_eglDisplay, m_eglDraw, m_eglRead, m_eglContext)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not restore prior EGL context");
        }
    }

private:
    SDL_Window* m_previousWindow = SDL_GL_GetCurrentWindow();
    SDL_GLContext m_previousContext = SDL_GL_GetCurrentContext();
    Display* m_glxDisplay = nullptr;
    GLXContext m_glxContext = nullptr;
    GLXDrawable m_glxDraw = None;
    GLXDrawable m_glxRead = None;
    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglDraw = EGL_NO_SURFACE;
    EGLSurface m_eglRead = EGL_NO_SURFACE;
    EGLenum m_eglApi = EGL_NONE;
    bool m_restore = false;
};

class ContextAttributes {
public:
    ContextAttributes() = default;
    ContextAttributes(const ContextAttributes&) = delete;
    ContextAttributes& operator=(const ContextAttributes&) = delete;

    bool setForCubism() {
        for (auto& item : m_attributes) {
            if (!SDL_GL_GetAttribute(item.attribute, &item.previous)) return false;
        }
        m_restore = true;
        for (const auto& item : m_attributes) {
            if (!SDL_GL_SetAttribute(item.attribute, item.requested)) return false;
        }
        return true;
    }

    ~ContextAttributes() {
        if (!m_restore) return;
        for (const auto& item : m_attributes) {
            SDL_GL_SetAttribute(item.attribute, item.previous);
        }
    }

private:
    struct Setting {
        SDL_GLAttr attribute;
        int requested;
        int previous = 0;
    };
    // The SDK's desktop shaders use GLSL 1.20 and legacy attributes, with
    // modern framebuffer entry points for composition.
    std::array<Setting, 5> m_attributes{{
        {SDL_GL_CONTEXT_MAJOR_VERSION, 3},
        {SDL_GL_CONTEXT_MINOR_VERSION, 3},
        {SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY},
        {SDL_GL_CONTEXT_FLAGS, 0},
        {SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0},
    }};
    bool m_restore = false;
};

} // namespace

struct OpenGLReadbackRenderPath::State {
    bool ensureTarget() {
        if (targetWidth == width && targetHeight == height) return true;
        glBindTexture(GL_TEXTURE_2D, color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        const GLenum error = glGetError();
        if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
            targetWidth = targetHeight = 0;
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
                "[Live2D/GL] Cubism target creation failed (framebuffer 0x%04X, GL error 0x%04X)",
                status, error);
            return false;
        }
        targetWidth = width;
        targetHeight = height;
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        return true;
    }

    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    GLuint fbo = 0;
    GLuint color = 0;
    int width = 0;
    int height = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    bool reportedDrawError = false;
    CubismRenderer* completedRenderer = nullptr;
    std::vector<unsigned char> pixels;
    std::unordered_map<CubismRenderer*, std::vector<GLuint>> textures;
    std::vector<std::unique_ptr<CubismUserModel>> pendingDestruction;
};

OpenGLReadbackRenderPath::OpenGLReadbackRenderPath() : m_state(std::make_unique<State>()) {}
OpenGLReadbackRenderPath::~OpenGLReadbackRenderPath() { shutdown(); }

bool OpenGLReadbackRenderPath::init(int width, int height) {
    CurrentContext restoreCurrent;
    if (!restoreCurrent.prepare()) return false;
    ContextAttributes attributes;
    if (!attributes.setForCubism()) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not set context attributes: %s",
            SDL_GetError());
        return false;
    }
    auto& state = *m_state;
    state.window = SDL_CreateWindow("Caesura Live2D", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (state.window) state.context = SDL_GL_CreateContext(state.window);
    if (!state.context) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Could not create hidden context: %s",
            SDL_GetError());
        return false;
    }
    const GLenum glewError = glewInit();
    bool glewReady = glewError == GLEW_OK;
#ifdef GLEW_ERROR_NO_GLX_DISPLAY
    // With SDL's EGL backend the GL entry points are already initialized even
    // though GLEW cannot initialize its optional GLX helpers.
    glewReady = glewReady || glewError == GLEW_ERROR_NO_GLX_DISPLAY;
#endif
    if (!glewReady || !GLEW_VERSION_3_3) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] OpenGL 3.3 setup failed (GLEW result %u)",
            glewError);
        return false;
    }
    // Some GLEW builds leave a compatibility-only query error behind.
    glGetError();
    GLint profileMask = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
    if ((profileMask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) == 0) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Cubism requires an OpenGL compatibility profile");
        return false;
    }
    resize(width, height);
    if (!state.width || !state.height) return false;
    glGenFramebuffers(1, &state.fbo);
    glGenTextures(1, &state.color);
    return state.fbo && state.color && state.ensureTarget();
}

void OpenGLReadbackRenderPath::shutdown() {
    auto& state = *m_state;
    if (!state.window) return;
    // The dedicated GL context owns the framebuffer and all remaining texture
    // names. SDL destroys that namespace without touching another context.
    if (state.context) SDL_GL_DestroyContext(state.context);
    SDL_DestroyWindow(state.window);
    state.window = nullptr;
    state.context = nullptr;
    state.fbo = state.color = 0;
}

CubismRenderer* OpenGLReadbackRenderPath::createModelRenderer(
    CubismUserModel& model, int width, int height) {
    CurrentContext scope;
    if (!scope.activate(m_state->window, m_state->context)) return nullptr;
    model.CreateRenderer(width, height);
    auto* renderer = model.GetRenderer<CubismRenderer_OpenGLES2>();
    if (!renderer) model.DeleteRenderer();
    return renderer;
}

bool OpenGLReadbackRenderPath::bindModelTexture(CubismRenderer* renderer, std::uint32_t index,
                                               int width, int height, const unsigned char* pixels) {
    CurrentContext scope;
    if (!scope.activate(m_state->window, m_state->context)) return false;
    GLint maxTexture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    if (width > maxTexture || height > maxTexture) return false;
    GLuint texture = 0;
    glGetError();
    glGenTextures(1, &texture);
    if (!texture) return false;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    // Cubism samples model textures with GL_LINEAR_MIPMAP_LINEAR.
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return false;
    }
    m_state->textures[renderer].push_back(texture);
    static_cast<CubismRenderer_OpenGLES2*>(renderer)->BindTexture(index, texture);
    return true;
}

void OpenGLReadbackRenderPath::destroyModelRenderer(std::unique_ptr<CubismUserModel>& model,
                                                   CubismRenderer* renderer) {
    auto& state = *m_state;
    CurrentContext scope;
    if (!scope.activate(state.window, state.context)) {
        state.pendingDestruction.push_back(std::move(model));
        return;
    }
    model->DeleteRenderer();
    const auto textures = state.textures.find(renderer);
    if (textures == state.textures.end()) return;
    glDeleteTextures(static_cast<GLsizei>(textures->second.size()), textures->second.data());
    state.textures.erase(textures);
}

bool OpenGLReadbackRenderPath::disposeFramework() {
    auto& state = *m_state;
    CurrentContext scope;
    if (!scope.activate(state.window, state.context)) {
        // The SDK destructors issue GL calls. On unrecoverable context loss
        // let the process reclaim the stranded CPU models rather than run
        // their GPU cleanup against a different context.
        for (auto& model : state.pendingDestruction) model.release();
        return false;
    }
    state.pendingDestruction.clear();
    CubismFramework::Dispose();
    return true;
}

void OpenGLReadbackRenderPath::beginFrame(CubismRenderer* renderer) {
    auto& state = *m_state;
    state.completedRenderer = nullptr;
    CurrentContext scope;
    if (!renderer || !scope.activate(state.window, state.context) || !state.ensureTarget()) return;

    glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER, state.fbo);
    glViewport(0, 0, state.width, state.height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    renderer->DrawModel();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, state.fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, state.width, state.height, GL_RGBA, GL_UNSIGNED_BYTE, state.pixels.data());
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) {
        state.completedRenderer = renderer;
        state.reportedDrawError = false;
    } else if (!state.reportedDrawError) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/GL] Cubism frame failed (GL error 0x%04X)", error);
        state.reportedDrawError = true;
    }
}

void OpenGLReadbackRenderPath::endFrame(CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) {
    auto& state = *m_state;
    if (!renderer || state.completedRenderer != renderer || !bgfx::isValid(bgfxTex)) return;
    state.completedRenderer = nullptr;
    const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(state.pixels.size()));
    const std::size_t rowBytes = static_cast<std::size_t>(state.width) * 4;
    for (int y = 0; y < state.height; ++y) {
        std::memcpy(memory->data + y * rowBytes,
                    state.pixels.data() + (state.height - 1 - y) * rowBytes, rowBytes);
    }
    bgfx::updateTexture2D(bgfxTex, 0, 0, 0, 0, static_cast<std::uint16_t>(state.width),
                          static_cast<std::uint16_t>(state.height), memory);
}

void OpenGLReadbackRenderPath::resize(int width, int height) {
    constexpr int maxTextureDimension = 16384;
    if (width <= 0 || height <= 0 || width > maxTextureDimension || height > maxTextureDimension) return;
    m_state->width = width;
    m_state->height = height;
}

} // namespace Caesura

#endif
