#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <cstddef>
#include <vector>
#include "RenderTypes.h"

namespace Caesura {

struct ScreenshotTicket {
    uint64_t requestId = 0;
    uint64_t generation = 0;
    explicit operator bool() const { return requestId != 0 && generation != 0; }
};
struct ScreenshotOptions {
    // Both zero select native backbuffer dimensions at admission; otherwise both
    // must be positive. Captures are resized to these dimensions if the surface
    // changes before submission. Excessive dimensions/retention are rejected.
    uint32_t width = 0;
    uint32_t height = 0;
};
enum class ScreenshotStatus { Unknown, Pending, Completed, Failed, Cancelled };
struct ScreenshotResult {
    ScreenshotTicket ticket;
    ScreenshotStatus status = ScreenshotStatus::Unknown;
    // Renderer-local submission frame, assigned by advanceFrame (zero before submission).
    uint64_t frameId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> png;
    std::string error;
};

enum class FontId : uint8_t { Small = 0, Large = 1, TTF = 2 };
struct FontRestoreState {
    bool active = false;
    FontId font = FontId::Small;
    std::string assetPath;
    float pixelSize = 0;
};
class IPreparedFontState {
public:
    virtual ~IPreparedFontState() = default;
    virtual const FontRestoreState& description() const = 0;
};

// -- View ID constants -----------------------------------------------------
// Render order enforced by IRenderDevice::init()
// VIEW_RTT (0) renders first   VIEW_MAIN (1) composites second   VIEW_DEBUG (2) last
constexpr uint16_t VIEW_RTT   = 0;  // Offscreen render-to-texture canvas
constexpr uint16_t VIEW_MAIN  = 1;  // Primary compositing pipeline (KAG UI)
constexpr uint16_t VIEW_DEBUG = 2;  // Debug overlay / IMGUI
constexpr uint16_t VIEW_TRANSITION = 99;  // Transition compositing view

// -- Handle types ----------------------------------------------------------

struct ViewportHandle {
    uint32_t id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const ViewportHandle& o) const { return id == o.id; }
    bool operator!=(const ViewportHandle& o) const { return id != o.id; }
};

struct RenderRuntimeInfo {
    std::string backendName;
    int width = 0;
    int height = 0;
    int viewCount = 0;
    bool shaderReady = false;
};

// -- Abstract Render Device ------------------------------------------------

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    // Lifecycle
    virtual bool init(void* nativeWindowHandle, int width, int height) = 0;

    // Present surface size in pixels. On desktop it equals the window size;
    // on Android the OS-imposed surface (e.g. 2320x956) can differ from the
    // configured engine resolution (1920x1080). The renderer keeps LOGICAL
    // coordinates for the scene while the presentation scales to this size,
    // so the game content fills the display (no clipping/letterbox).
    virtual void setPresentSize(uint32_t width, uint32_t height) = 0;
    virtual bool isInitialized() const = 0;
    virtual void beginShutdown() = 0;

    // -- IMPORTANT: shutdown() internally calls flushAllRTT() then renderer shutdown --
    // The teardown contract is:
    //   1. flushAllRTT()   release all GPU-side framebuffers and textures
    //      while the GPU context is still alive
    //   2. renderer shutdown   destroy GPU context
    // Callers must ensure Lua VM is already dead before calling this.
    virtual void shutdown() = 0;

    // Explicitly release all RTT framebuffers/textures while GPU context is
    // still alive. Safe to call multiple times. Called automatically by shutdown().
    virtual void flushAllRTT() = 0;

    // Frame management: commit_frame finalizes scene/postfx submissions without
    // advancing bgfx. advanceFrame submits pending captures and advances once.
    // endFrame is the convenience spelling of commit_frame + advanceFrame.
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void commit_frame() = 0;
    virtual void advanceFrame() = 0;

    // View management
    virtual void setScreenOffset(int dx, int dy) = 0;
    virtual void setViewRect(uint16_t viewId, uint16_t x, uint16_t y,
                             uint16_t width, uint16_t height) = 0;
    virtual void setViewClear(uint16_t viewId, uint16_t flags,
                              uint32_t rgba, float depth, uint8_t stencil) = 0;
    virtual void touch(uint16_t viewId) = 0;

    // Offscreen render target
    virtual ViewportHandle createRenderTarget(int width, int height) = 0;
    virtual void destroyRenderTarget(ViewportHandle handle) = 0;

    // Draw a viewport`s texture as a full-view quad in another view
    virtual void blitViewport(ViewportHandle handle, uint16_t targetView,
                              float x, float y, float w, float h) = 0;

    // Get an opaque texture handle from a viewport handle.
    virtual RenderTextureHandle getViewportTexture(ViewportHandle handle) = 0;

    // Resolution query
    virtual int getBackbufferWidth() const = 0;
    virtual int getBackbufferHeight() const = 0;

    // Window resize -- must be called when the OS window changes size.
    // Recreates the backbuffer and re-applies all view transforms.
    virtual void resize(int width, int height) = 0;

    // Direct texture blit (submits a textured quad to the target view)
    // Used by RenderBinding::submit_batch to render layer quads.
    // The texture handle is an opaque integer (bgfx texture id).
    virtual void blitTexture(uint16_t targetView, uint32_t textureId,
                              float x, float y, float w, float h, uint8_t opacity) = 0;

    // -- Stretch Blit / Affine Blit (transform.lua GPU path) --------------
    // Stretch: blits src_rect -> dst_rect with sampler filter.
    // filterType: 0=Nearest, 1=Linear, 2=Anisotropic, 3+=Custom shader
    virtual void stretchBlt(uint16_t targetView, uint32_t dstTexId,
                             float dx, float dy, float dw, float dh,
                             uint32_t srcTexId,
                             float sx, float sy, float sw, float sh,
                             int filterType) = 0;

    // Affine: blits src_rect through 2D affine matrix onto dst rect.
    // matrix: {a, b, c, d, tx, ty} (2x3 row-major, column-vector convention:
    //   x` = a*x + c*y + tx
    //   y` = b*x + d*y + ty)
    virtual void affineBlt(uint16_t targetView, uint32_t dstTexId,
                            float dx, float dy, float dw, float dh,
                            uint32_t srcTexId,
                            float sx, float sy, float sw, float sh,
                            const float matrix[6]) = 0;

    // Debug marker
    
    // -- Batch protocol (performance optimization per spec [0.3]) ----------
    // beginBatch() defers GPU submission; flushBatch() submits all queued
    // draw calls at once, reducing draw-call overhead for multi-layer scenes.
    virtual void beginBatch() = 0;
    virtual void flushBatch() = 0;

    // Debug marker
    virtual void setDebugName(uint16_t viewId, const std::string& name) = 0;
    virtual void drawDebugOverlay(const std::string& title) = 0;
    virtual bool requestScreenshot(const std::string& path) = 0;
    // Rejected admission returns Failed with an invalid ticket. Accepted requests
    // start Pending. takeScreenshot preserves Pending and consumes a terminal
    // result exactly once (subsequent takes return Unknown). Cancellation only
    // changes Pending to Cancelled; a terminal result still must be taken.
    virtual ScreenshotResult requestScreenshot(const ScreenshotOptions& options) = 0;
    virtual ScreenshotResult takeScreenshot(const ScreenshotTicket& ticket) = 0;
    virtual bool cancelScreenshot(const ScreenshotTicket& ticket) = 0;
    virtual bool recoverDevice(void* nativeWindowHandle, int width, int height) = 0;
    virtual void flagDeviceLost() = 0;
    virtual bool consumeDeviceLost() = 0;

    // -- Text rendering (bitmap font via embedded atlas) --------------
    // scale:  glyph scale factor (1.0 = atlas size; {size=N} markup).
    // bold:   synthetic bold (double-pass x-offset; {b} markup).
    // italic: italic shear (top-edge horizontal offset; {i} markup).
    // strike: strikethrough bar across the glyph (solid 1px-ish bar;
    //         {s} markup).
    virtual void renderText(uint16_t viewId, const std::string& text,
                             float x, float y,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                             float scale = 1.0f, bool bold = false,
                             bool italic = false, bool strike = false) = 0;
    virtual void renderRuby(uint16_t viewId, const std::string& text,
                             const std::string& ruby,
                             float x, float y,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;
    virtual void setFont(int fontId) = 0;
    virtual bool loadTTF(const char* path, float fontSize) = 0;
    virtual FontRestoreState captureFontState() const = 0;
    virtual FontRestoreState defaultFontState() const = 0;
    virtual std::unique_ptr<IPreparedFontState> prepareFontState(
        const FontRestoreState& state, const uint8_t* bytes, size_t size) = 0;
    virtual bool applyFontState(std::unique_ptr<IPreparedFontState> prepared) = 0;
    virtual void clearFontState() = 0;
    virtual float textLineHeight() const = 0;

    // -- Blend / Transition / VFX submission (P1: abstract interface methods) --
    virtual void submitBlend(uint16_t viewId, RenderTextureHandle baseTex,
                             RenderTextureHandle blendTex, int mode,
                             float baseAlpha, float blendAlpha, float globalAlpha) = 0;
    virtual void submitTransition(uint16_t viewId, RenderTextureHandle fromTex,
                                  RenderTextureHandle toTex, RenderTextureHandle ruleTex,
                                  int method, float progress) = 0;
    virtual void submitVFX(uint16_t viewId, RenderTextureHandle srcTex,
                           int effect, float fadeAlpha, float fadeR, float fadeG, float fadeB,
                           float blurRadius, float quakeX, float quakeY) = 0;
    virtual void fillViewport(ViewportHandle handle, uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;

    // -- Accessibility color filter (Neo-Genesis) ---------------------------
    // Presets: deuteranopia/protanopia/tritanopia simulation matrices
    // (Machado et al.), grayscale, high-contrast. The active preset is
    // applied by submitVFX(effect=4) full-screen passes; returns false on
    // devices without post-processing support (Null).
    enum class ColorFilterPreset {
        None = 0,
        Deuteranopia,
        Protanopia,
        Tritanopia,
        Grayscale,
        HighContrast,
    };
    virtual bool setColorFilter(ColorFilterPreset preset) = 0;

    // -- Post-processing chain (round 102: Neo-Genesis postfx) ----------
    // Full-screen effect chain applied to the scene before backbuffer
    // composite. While at least one PostFx is active, the scene renders to
    // an internal scene RTT and the chain runs sceneRTT -> stage... ->
    // backbuffer each frame. When the chain is empty the scene draws
    // directly to the backbuffer (zero overhead, current behavior).
    // Null/software renderers return 0 / false (graceful degradation --
    // the Lua layer treats unsupported as no-op, matching submitVFX).
    enum class PostFxKind : uint8_t {
        Vignette = 0,      // radial darkening; params: strength, radius, rgb tint
        LutColorGrade = 1, // 3x1 color matrix grade; params: strength, rgb, lutMix
        SoftBlur = 2,      // gaussian soften; params: radius(px), amount
        Bloom = 3,         // bright-pass + downsampled additive glow; params: strength, amount(threshold)
        Lut3D = 4,         // 3D LUT (t214/t209): 2D-packed LUT texture + intensity blend; params: lutTexture, lutSize
    };
    struct PostFxParams {
        float strength = 1.0f; // master intensity 0..1
        float radius  = 0.0f;  // vignette inner radius / blur radius px / bloom spread
        float amount  = 0.0f;  // bloom threshold / blur mix
        float r = 1.0f, g = 1.0f, b = 1.0f; // tint color (vignette/LUT)
        float lutMix = 0.0f;   // LUT grade mix 0..1 (1 = full grade)
        RenderTextureHandle lutTexture{}; // Lut3D (t214): 2D-packed LUT texture (borrowed; TextureManager owns it)
        uint8_t lutSize = 0;   // Lut3D cube side N (16/64); 0 = derive from texture height (width must be N*N)
    };
    using PostFxHandle = uint32_t; // 0 = invalid/unsupported
    virtual bool isPostFxSupported(PostFxKind kind) const = 0;
    virtual PostFxHandle createPostFx(PostFxKind kind, const PostFxParams& params) = 0;
    virtual void setPostFxParams(PostFxHandle handle, const PostFxParams& params) = 0;
    virtual void destroyPostFx(PostFxHandle handle) = 0; // 0/unknown safe no-op
    virtual void clearPostFx() = 0;
    virtual bool isPostFxActive() const = 0;

    // -- Shader / Sampler access (for ParticleSystem and other GPU systems) --
    virtual RenderUniformHandle getDefaultSampler() const = 0;
    virtual RenderProgramHandle getFallbackProgram() const = 0;

    // Backend identification --------------------------------------------
    virtual const char* getBackendName() const = 0;
    virtual RenderRuntimeInfo getRuntimeInfo() const = 0;
    virtual bool setPreferredBackend(const char*) = 0;
};

} // namespace Caesura
