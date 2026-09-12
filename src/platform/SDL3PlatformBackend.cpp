#include "SDL3PlatformBackend.h"
#include <cstdio>

namespace Caesura {

SDL3PlatformBackend::~SDL3PlatformBackend() {
    shutdown();
}

bool SDL3PlatformBackend::init(const char* title, int width, int height) {
    if (m_initialized) return true;

    m_width  = width;
    m_height = height;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "[SDL3] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
#if defined(__ANDROID__)
    // Device-day: bgfx GLES uses the SDL-owned EGL context; the window must
    // be created OpenGL-capable or SDL_GL_CreateContext refuses it.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    // Lock the app to landscape: without SDL_HINT_ORIENTATIONS SDL3 lets the
    // Android display auto-rotation drive the window orientation, so the app
    // visibly spins with the phone despite the manifest screenOrientation
    // lock (the engine then re-orients its surface). The hint makes SDL
    // restrict the activity to landscape and ignore sensor rotation.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft,LandscapeRight");
#endif

    m_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

    if (!m_window) {
        fprintf(stderr, "[SDL3] CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_ShowWindow(m_window);
    m_initialized = true;
    printf("[SDL3] Platform backend initialized: %dx%d \"%s\"\n", width, height, title);
    return true;
}

void SDL3PlatformBackend::shutdown() {
    if (!m_initialized) return;
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
    m_initialized = false;
    printf("[SDL3] Platform backend shut down.\n");
}

bool SDL3PlatformBackend::pollEvent() {
    if (!m_initialized) return false;
    return SDL_PollEvent(&m_lastEvent);
}

IPlatformBackend::MouseState SDL3PlatformBackend::getMouseState() const {
    MouseState ms;
    float mx, my;
    SDL_GetMouseState(&mx, &my);
    ms.x = mx; ms.y = my;
    ms.leftDown = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
    return ms;
}

uint64_t SDL3PlatformBackend::getTicksMs() const {
    return SDL_GetTicks();
}

int SDL3PlatformBackend::getWindowWidth() const {
    int width = 0;
    return m_window && SDL_GetWindowSize(m_window, &width, nullptr) && width > 0
        ? width : m_width;
}

int SDL3PlatformBackend::getWindowHeight() const {
    int height = 0;
    return m_window && SDL_GetWindowSize(m_window, nullptr, &height) && height > 0
        ? height : m_height;
}

void SDL3PlatformBackend::resizeWindow(int width, int height) {
    if (m_window && SDL_SetWindowSize(m_window, width, height)) {
        // SDL/OS resize requests may be asynchronous. Expose the actual
        // current window units; the Engine owns polling, not this backend.
        SDL_GetWindowSize(m_window, &m_width, &m_height);
    }
}

void SDL3PlatformBackend::setFullscreen(bool fullscreen) {
    if (m_window) {
        SDL_SetWindowFullscreen(m_window, fullscreen);
        printf("[SDL3] Fullscreen: %s\n", fullscreen ? "on" : "off");
    }
}

void SDL3PlatformBackend::postFrame() {
#if defined(__ANDROID__)
    if (m_window) SDL_GL_SwapWindow(m_window);
#endif
}

bool SDL3PlatformBackend::createGLContext() {
#if defined(__ANDROID__)
    if (m_glContext) return true;
    if (!SDL_GL_CreateContext(m_window)) {
        fprintf(stderr, "[SDL3] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_GL_MakeCurrent(m_window, SDL_GL_GetCurrentContext())) {
        fprintf(stderr, "[SDL3] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        return false;
    }
    m_glContext = SDL_GL_GetCurrentContext();
    fprintf(stderr, "[SDL3] EGL context created: %p\n", m_glContext);
    return true;
#else
    return true;
#endif
}

void* SDL3PlatformBackend::getNativeWindowHandle() const {
    if (!m_window) return nullptr;

    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);

    void* nwh = SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!nwh) {
        nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    }
    if (!nwh) {
        nwh = (void*)(uintptr_t)SDL_GetNumberProperty(props,
            SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    }
    if (!nwh) {
        nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    }
#if defined(__ANDROID__)
    if (!nwh) {
        nwh = SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    }
#endif

    return nwh;
}

bool SDL3PlatformBackend::startTextInput() {
    if (!m_window) return false;
    return SDL_StartTextInput(m_window);
}

bool SDL3PlatformBackend::stopTextInput() {
    if (!m_window) return false;
    return SDL_StopTextInput(m_window);
}

bool SDL3PlatformBackend::setTextInputRect(int x, int y, int w, int h, int cursor) {
    if (!m_window) return false;
    SDL_Rect rect{ x, y, w, h };
    return SDL_SetTextInputArea(m_window, &rect, cursor);
}

bool SDL3PlatformBackend::isTextInputActive() const {
    if (!m_window) return false;
    return SDL_TextInputActive(m_window);
}

} // namespace Caesura
