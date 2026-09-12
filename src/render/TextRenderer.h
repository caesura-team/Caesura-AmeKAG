#pragma once
#include <bgfx/bgfx.h>
#include <string>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <ft2build.h>
#include "api/IRenderDevice.h"
#include "../di/api/IDeviceLostListener.h"
#include FT_FREETYPE_H

namespace Caesura {

// CJK static atlas entry
struct CjkGlyph {
    uint16_t x = 0, y = 0, w = 0, h = 0;
    int16_t advance = 0, offsetX = 0, offsetY = 0;
};

// Resident vertex/index buffer for text layer batching.
//
// One instance == one LRU slot of TextRenderer's text-geometry cache. The slot
// owns its dynamic VB/IB; eviction re-keys the slot and rebuilds into the SAME
// buffers (the buffers are the pooled resource, so eviction costs no GPU
// allocation).
struct MessageLayerCache {
    bgfx::DynamicVertexBufferHandle vb = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle  ib = BGFX_INVALID_HANDLE;
    uint32_t maxGlyphs = 2048;
    uint32_t dirtyStart = 0, dirtyEnd = 0, glyphCount = 0;
    std::string cachedText;
    // Full cache key: geometry is only reused when EVERY parameter matches
    // (a single-slot cache keyed on text alone would mis-render the second
    // draw when multiple draws share a text but differ in view/position).
    uint16_t cachedViewId = 0xFFFF;
    float    cachedX = 0.0f, cachedY = 0.0f;
    float    cachedPenAdvance = 0.0f;  // sum of glyph advances for cachedText
    bool matches(uint16_t viewId, const std::string& text, float x, float y) const {
        return cachedViewId == viewId && cachedText == text && cachedX == x && cachedY == y;
    }
    bool        cacheIsCjk = false;     // TD-13: whether cached text uses CJK atlas

    // -- Incremental-append bookkeeping ----------------------------------
    // penEnd is the ABSOLUTE pen X after cachedText. An append-only update
    // resumes layout from exactly this float (reconstructing it as
    // x + cachedPenAdvance would round differently and break byte equivalence
    // with the full-rebuild path).
    float penEnd = 0.0f;
    // Cumulative CJK bookkeeping: layoutGlyphs() derives allCjk from the whole
    // text, so an incremental update has to fold the prefix's flags back in.
    bool anyGlyph = false;
    bool anyNonCjk = false;
    // false == the buffers hold nothing reusable (fresh slot, invalidated
    // cache, or post-device-loss).
    bool geometryValid = false;
    // LRU stamp (TextRenderer::m_cacheClock at the last use). 0 == never used.
    uint64_t lastUse = 0;

    bool isDirty() const { return dirtyStart < dirtyEnd; }
    void markAllDirty() { dirtyStart = 0; dirtyEnd = maxGlyphs; }
    void clearDirty()   { dirtyStart = dirtyEnd = 0; }
    // Drop reusable geometry without touching the GPU buffers.
    void invalidateGeometry() {
        cachedText.clear();
        glyphCount = 0;
        penEnd = 0.0f;
        anyGlyph = anyNonCjk = false;
        geometryValid = false;
        cachedViewId = 0xFFFF;
        markAllDirty();
    }
};


struct TextColor {
    uint8_t r, g, b, a;
    static TextColor White()  { return {255,255,255,255}; }
    static TextColor Black()  { return {  0,  0,  0,255}; }
    static TextColor Gray()   { return {128,128,128,255}; }
};

struct TextCursor {
    float x = 32.0f; float y = 48.0f;
    float leftMargin = 32.0f; float lineHeight = 16.0f;
};

struct GlyphMetrics { int x=0, y=0, w=0, h=0, advance=0, offsetX=0, offsetY=0; };

class TextRenderer : public IDeviceLostListener {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    bool init(IRenderDevice* device, bool activateDefault = true);
    void shutdown();

    // CPU font owner used by preparation, live demand rasterization and device
    // recovery. Append-only placement keeps every existing glyph/UV stable.
    // These operations never access a GPU and expose no mutable atlas storage.
    class GlyphAtlas {
    public:
        enum class PrepareStatus { Existing, Added, Missing, Full, Error };
        struct DirtyRect { int x=0, y=0, w=0, h=0; };
        static constexpr size_t MaxGlyphs = 8192;
        static std::unique_ptr<GlyphAtlas> create(const uint8_t* bytes, size_t size, float pixelSize);
        ~GlyphAtlas();
        GlyphAtlas(const GlyphAtlas&) = delete;
        GlyphAtlas& operator=(const GlyphAtlas&) = delete;
        PrepareStatus prepareGlyph(uint32_t codepoint) noexcept;
        const GlyphMetrics* find(uint32_t codepoint) const;
        const GlyphMetrics& resolve(uint32_t codepoint) const;
        const GlyphMetrics& replacement() const;
        int width() const { return atlasW; }
        int height() const { return atlasH; }
        size_t glyphCount() const { return glyphs.size(); }
        const std::vector<uint8_t>& pixels() const { return atlasPixels; }
        DirtyRect pendingUpload() const { return dirty; }
        // Called only after the consumer has queued an owned GPU upload.
        void acknowledgeUpload() { dirty = {}; }
    private:
        friend class TextRenderer;
        GlyphAtlas() = default;
        FT_Library ftLib = nullptr;
        FT_Face ftFace = nullptr;
        float ascent=0, descent=0, lineGap=0;
        int atlasW=2048, atlasH=2048;
        int penX=1, penY=1, maxRowH=0;
        uint32_t replacementCodepoint=0;
        std::unordered_map<uint32_t, GlyphMetrics> glyphs;
        std::vector<uint8_t> sourceBytes;
        std::vector<uint8_t> atlasPixels;
        DirtyRect dirty;
    };

    // TTF loading owns source bytes and prepares replacement/space; actual
    // text codepoints are rasterized on demand before their first submission.
    bool loadTTF(const char* path, float fontSize = 24.0f);
    FontRestoreState captureFontState() const;
    static std::unique_ptr<IPreparedFontState> prepareFontState(
        const FontRestoreState& state, const uint8_t* bytes, size_t size);
    bool applyFontState(std::unique_ptr<IPreparedFontState> prepared);
    void clearFontState();
    std::unique_ptr<IPreparedFontState> takeFontForDeviceRecovery();

    void setScreenSize(int w, int h);
    void setFont(FontId id);
    FontId currentFont() const { return m_currentFont; }
    float lineHeight() const { return m_cursor.lineHeight; }

    void renderText(uint16_t viewId, const std::string& text,
                    float x, float y, TextColor color,
                    float scale = 1.0f, bool bold = false,
                    bool italic = false, bool strike = false);
    void renderRuby(uint16_t viewId, const std::string& text,
                     const std::string& ruby, float x, float y, TextColor color);

    // -- Pure quad geometry (headless-testable) ---------------------------
    // NDC vertex math for a glyph quad: the top edge is sheared right by
    // `shear` px (italic), the bottom edge stays fixed, so advance metrics
    // are unchanged. Extracted as a pure static so unit tests can pin the
    // geometry without a GPU.
    struct NDCQuad { float x0, y0, x1, y1, x2, y2, x3, y3; };
    static NDCQuad glyphQuadToNDC(float x, float y, float w, float h,
                                  float shear, float screenW, float screenH,
                                  float paddingX = 0, float paddingY = 0);

    // -- Pure glyph layout (headless-testable) ----------------------------
    // The batch-cache layout math (UTF-8 decode, glyph lookup, quad + UV
    // building, pen advance, NDC conversion) is extracted as pure statics so
    // unit tests can pin it without a GPU. The glyph source is injected via
    // a callback; the production path wraps getTTFGlyph(), tests provide a
    // table. `hasCjk` selects the CJK UV space, `useTtf`/`ttfAscent` drive
    // the baseline offset (mirrors rebuildCache()).
    struct LaidGlyph {
        float gx = 0, gy = 0, w = 0, h = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        bool fromCjk = false;
        // Ink/advance metrics above stay unchanged. TTF geometry includes
        // half a transparent source texel for the complete bilinear footprint.
        float padding = 0;
    };
    struct GlyphLookupResult {
        GlyphMetrics gm;   // w/h <= 0 means "no glyph" (empty slot)
        bool fromCjk;      // glyph sourced from the CJK atlas space
    };
    using GlyphLookupFn = GlyphLookupResult (*)(uint32_t codepoint, void* userData);
    struct GlyphLayoutResult {
        std::vector<LaidGlyph> glyphs;
        float penAdvance = 0.0f;   // final pen X after all advances
        bool allCjk = false;       // every emitted glyph came from CJK
        // Raw bookkeeping behind allCjk. An incremental (append-only) update
        // only lays out the tail, so it must OR these into the prefix's flags
        // to reach the same allCjk verdict the full path computes.
        bool anyGlyph = false;
        bool anyNonCjk = false;
    };
    // Pure: decode UTF-8 `text`, look up each codepoint, emit quads + UVs.
    // No bgfx/device state is touched. maxGlyphs truncates the output list.
    // `byteBegin` starts the decode at a byte offset (must be a codepoint
    // boundary): the incremental append path lays out ONLY the new tail, with
    // penX resumed from the cached absolute pen. Per-codepoint layout depends
    // on nothing but the pen and the glyph table, which is what makes the tail
    // result bit-identical to the corresponding slice of a full layout.
    static GlyphLayoutResult layoutGlyphs(const std::string& text,
                                          float penX, float penY,
                                          GlyphLookupFn lookup, void* userData,
                                          bool hasCjk, float invW, float invH,
                                          float cjkInvW, float cjkInvH,
                                          bool useTtf, float ttfAscent,
                                          size_t maxGlyphs,
                                          size_t byteBegin = 0);
    // Pure: turn laid-out glyphs into a triangle-list vertex stream
    // (6 verts x {x,y,u,v} per glyph) + indices. screenW/H <= 0 falls back
    // to raw pixel coordinates (matches rebuildCache()).
    // `glyphIndexBase` is the glyph slot the first emitted glyph occupies in
    // the resident buffer: indices are absolute, so a tail update written at
    // vertex offset base*6 must emit indices starting from base*6 too.
    static void buildQuadVertices(const std::vector<LaidGlyph>& glyphs,
                                  float screenW, float screenH,
                                  std::vector<float>& verts,
                                  std::vector<uint32_t>& indices,
                                  uint32_t glyphIndexBase = 0);

    // -- Pure dirty-range math (headless-testable) -------------------------
    // Count UTF-8 codepoints in a byte range (lenient: a truncated trailing
    // sequence counts as one glyph). Used by the batch-cache dirty tracking.
    static uint32_t countUtf8Glyphs(const uint8_t* data, size_t len);

    struct DirtyRangeResult {
        bool changed = false;   // false == texts identical, no rebuild needed
        uint32_t start = 0;     // first changed glyph (codepoint-aligned)
        uint32_t end = 0;       // one past the last changed glyph, <= maxGlyphs
    };
    // Pure: codepoint-aligned dirty range when the cached text changes from
    // oldText to newText. Mirrors updateDirtyRange() exactly, minus the
    // MessageLayerCache member writes.
    static DirtyRangeResult computeDirtyRange(const std::string& oldText,
                                              const std::string& newText,
                                              uint32_t maxGlyphs);

    // -- Pure append detection (headless-testable) -------------------------
    // The typewriter reveal grows a line one codepoint at a time, so the new
    // text is the old text plus a tail. That is the ONLY shape the cache can
    // update incrementally: every already-laid glyph keeps its position, so
    // only the tail needs layout and only the tail's vertices need uploading.
    struct AppendResult {
        bool isAppend = false;    // newText == oldText + non-empty tail
        size_t tailByteOffset = 0;  // byte offset of the tail in newText
        uint32_t prefixGlyphs = 0;  // codepoints in the shared prefix
        uint32_t tailGlyphs = 0;    // codepoints in the tail
    };
    // isAppend is false for identical text, shortened text, any rewrite, and
    // for a prefix that is not codepoint-aligned (defensive: a truncated
    // multi-byte lead byte must never be treated as a complete prefix).
    static AppendResult detectAppend(const std::string& oldText,
                                     const std::string& newText);

    // -- Cache update planning (headless-testable) -------------------------
    // The decision "reuse as-is / extend the tail / rebuild everything", plus
    // the exact work that decision implies. This is the SAME function the
    // production renderTextCached() uses to choose its path, so a test that
    // calls it is measuring the real policy, not a re-implementation of it.
    enum class CacheAction : uint8_t {
        Hit = 0,        // geometry reusable as-is: no layout, no upload
        Append,         // lay out + upload ONLY the appended tail
        FullRebuild     // lay out + upload everything
    };
    struct CachePlan {
        CacheAction action = CacheAction::FullRebuild;
        uint32_t glyphsToLayout = 0;  // codepoints handed to layoutGlyphs()
        uint32_t firstGlyph = 0;      // glyph slot the upload starts at
        uint32_t vertexBytes = 0;     // bytes handed to bgfx::update (VB)
        uint32_t indexBytes = 0;      // bytes handed to bgfx::update (IB)
        size_t   byteBegin = 0;       // layoutGlyphs() start offset
    };
    // Pure: no bgfx, no member writes. `slot` is inspected, never modified.
    static CachePlan planCacheUpdate(const MessageLayerCache& slot,
                                     uint16_t viewId, const std::string& text,
                                     float x, float y);

    // -- Pure LRU slot selection (headless-testable) -----------------------
    // Picks the slot index this draw should use, in priority order:
    //   1. exact-key hit (reuse as-is),
    //   2. append-compatible slot (same view/pos, cachedText is a prefix),
    //   3. least-recently-used slot (an unused slot has lastUse == 0 and wins).
    // Pure: reads the slots, writes nothing. The caller stamps lastUse.
    static size_t selectCacheSlot(const MessageLayerCache* slots, size_t count,
                                  uint16_t viewId, const std::string& text,
                                  float x, float y);

    // -- Instrumentation (test-only observability) -------------------------
    // Counters for the batch cache. They are plain uint64 increments on the
    // paths that already do far heavier work (a bgfx::update or a full glyph
    // layout), so they are compiled into every configuration rather than
    // guarded by CAESURA_DEBUG: a release-only code shape could not be
    // measured by the tests that must prove the optimization.
    struct CacheStats {
        uint64_t rebuildFull = 0;       // full layout + full upload
        uint64_t rebuildIncremental = 0; // tail-only layout + tail-only upload
        uint64_t cacheHits = 0;         // no layout, no upload
        uint64_t glyphsLaidOut = 0;     // codepoints passed through layoutGlyphs
        uint64_t vertexBytesUploaded = 0; // bytes handed to bgfx::update (VB)
        uint64_t indexBytesUploaded = 0;  // bytes handed to bgfx::update (IB)
        uint64_t evictions = 0;         // LRU slots re-keyed to a new text
    };
    const CacheStats& cacheStats() const { return m_cacheStats; }
    void resetCacheStats() { m_cacheStats = CacheStats{}; }

    // -- Kinsoku-aware line breaking (headless-testable) -------------------
    // Greedy wrap of `text` into lines no wider than maxWidth, honoring the
    // kinsoku rules via canBreakBetween(). Width comes from an injected
    // advance callback so this is GPU-free and unit-testable.
    // Returns byte offsets: line i spans [breaks[i], breaks[i+1]).
    using AdvanceFn = float (*)(uint32_t codepoint, void* userData);
    static float measureAdvance(const std::string& text, AdvanceFn advance, void* userData);
    static std::vector<size_t> wrapTextKinsoku(const std::string& text,
                                               float maxWidth,
                                               AdvanceFn advance, void* userData);

    // -- CJK Kinsoku Shori (避头尾法则) line-breaking rules ----------------
    static bool isKinsokuLineStartForbidden(uint32_t codepoint);
    static bool isKinsokuLineEndForbidden(uint32_t codepoint);
    static bool canBreakBetween(uint32_t leftCodepoint, uint32_t rightCodepoint);

    void newline();
    void clearText(uint16_t viewId);
    void setCursor(float x, float y) { m_cursor.x = x; m_cursor.y = y; }
    void resetCursor() { m_cursor.x = m_cursor.leftMargin; }

    const TextCursor& cursor() const { return m_cursor; }
    bgfx::TextureHandle fontTexture() const { return m_fontTexture; }
    bool isInitialized() const { return m_initialized; }

    // -- Batch-cached rendering (Track 2) --
    float renderTextCached(uint16_t viewId, const std::string& text,
                           float x, float y, TextColor color,
                           bgfx::ProgramHandle program = BGFX_INVALID_HANDLE);
    void invalidateCache();
    // The most recently used slot. Kept as `cache()` so existing callers and
    // tests that inspected the single-slot cache keep compiling.
    const MessageLayerCache& cache() const { return m_cacheSlots[m_lastSlot]; }
    // Slot count and per-slot inspection: one text layer (name / body / ruby)
    // occupies one slot, so a frame drawing N distinct texts needs N slots to
    // avoid mutual eviction.
    static constexpr size_t kCacheSlots = 8;
    size_t cacheSlotCount() const { return kCacheSlots; }
    const MessageLayerCache& cacheSlot(size_t i) const { return m_cacheSlots[i]; }

    // -- CJK static atlas (Track 2) --
    bool loadCjkAtlas(const std::string& atlasPath, const std::string& metaPath);
    bool isExpanding() const { return m_expanding; }

    // -- IDeviceLostListener --
    // Releases GPU resources only -- does NOT unregister: the listener list
    // is iterated during notifyDeviceLost (erase during iteration is UB) and
    // restore must still reach us to reinitialize. Defined in the .cpp.
    void onDeviceLost() override;
    void onDeviceRestored() override;

private:
    // GlyphQuad: axis-aligned quad + per-glyph italic shear (top-edge
    // horizontal offset in px; 0 = upright). The bottom edge stays fixed,
    // so advance metrics are unchanged by italics.
    struct GlyphQuad {
        float x, y, w, h, shear = 0.0f, u0, v0, u1, v1;
        float advance = 0.0f, paddingX = 0.0f, paddingY = 0.0f;
    };
    GlyphQuad buildGlyph(char ch, float penX, float penY, float scaleW, float scaleH);
    GlyphQuad buildGlyph(uint32_t cp, float penX, float penY, float scaleW, float scaleH);

    void submitGlyphQuads(uint16_t viewId, const GlyphQuad* quads,
                          int count, TextColor color, float scaleW, float scaleH);
    // Strikethrough bars: solid-color quads across the glyph (1x1 white
    // texture; lazily created, destroyed on shutdown/device loss).
    void ensureStrikeTexture();
    void submitStrikeBars(uint16_t viewId, const GlyphQuad* bars,
                          int count, TextColor color);
    bool loadFontAtlas(FontId id);

    // TTF atlas
    using TTFState = GlyphAtlas;
    struct PreparedFont final : public IPreparedFontState {
        FontRestoreState state;
        std::unique_ptr<TTFState> ttf;
        std::vector<uint8_t> bitmapPixels;
        int atlasW=0, atlasH=0, glyphW=0, glyphH=0, atlasCols=0;
        float lineHeight=16;
        std::string rememberedTtfPath;
        float rememberedTtfSize=24;
        const FontRestoreState& description() const override { return state; }
    };
    static std::unique_ptr<IPreparedFontState> prepareBitmapFont(const FontRestoreState& state);
    bool activateFont(PreparedFont& prepared);
    std::unique_ptr<TTFState> m_ttf;
    static GlyphAtlas::PrepareStatus rasterizeTTFGlyph(TTFState& font, uint32_t cp);
    bool prepareTextGlyphs(const std::string& text, size_t byteBegin = 0);
    bool uploadPendingGlyphs();
    bool m_reportedMissingGlyph=false, m_reportedAtlasFull=false, m_reportedAtlasUploadFailure=false;
    std::vector<uint8_t> m_bitmapPixels;
    FontRestoreState m_fontDescription;

    // -- Glyph lookup (Track 2) --
    GlyphMetrics getTTFGlyph(uint32_t codepoint);
    // GPU-free glyph source used by rebuildCache: resolves via getTTFGlyph()
    // and computes the CJK-sourcing flag. userData = the TextRenderer*.
    static GlyphLookupResult glyphLookupForCache(uint32_t codepoint, void* userData);

    // -- Batch cache internals (Track 2) --
    struct GlyphDraw { float gx, gy, w, h, u0, v0, u1, v1; };
    void updateDirtyRange(MessageLayerCache& slot, const std::string& newText);
    // Full rebuild: lay out every codepoint, upload the whole VB/IB.
    float rebuildCache(MessageLayerCache& slot, uint16_t viewId,
                       const std::string& text,
                       float x, float y, TextColor color,
                       bgfx::ProgramHandle program);
    // Append-only update: lay out ONLY the new tail and upload ONLY its
    // vertex/index range. Returns the new absolute pen X.
    float appendToCache(MessageLayerCache& slot, uint16_t viewId,
                        const std::string& text, const CachePlan& plan,
                        float x, float y, TextColor color,
                        bgfx::ProgramHandle program);
    // Bind + submit a slot's resident geometry (shared by every path).
    void submitCachedSlot(MessageLayerCache& slot, uint16_t viewId,
                          TextColor color, bgfx::ProgramHandle program);
    bool ensureCacheBuffers(MessageLayerCache& slot);
    // Pick the slot for this key via selectCacheSlot(), then stamp its LRU
    // clock and count an eviction when a live slot is re-keyed.
    MessageLayerCache& acquireSlot(uint16_t viewId, const std::string& text,
                                   float x, float y);

    // -- Track 2 state --
    uint16_t m_atlasW = 2048, m_atlasH = 2048;
    bgfx::TextureHandle m_cjkAtlas = BGFX_INVALID_HANDLE;
    std::unordered_map<uint32_t, CjkGlyph> m_cjkGlyphs;
    bool m_expanding = false;
    bgfx::UniformHandle m_u_color = BGFX_INVALID_HANDLE;
    // LRU set of text-geometry slots. Capacity 8: a visual-novel frame draws
    // speaker name + message body + a handful of ruby runs / choice labels, so
    // 8 covers the observed worst case while costing at most 8 dynamic VB/IB
    // pairs (created lazily, only when a slot is first used).
    MessageLayerCache m_cacheSlots[kCacheSlots];
    size_t   m_lastSlot = 0;     // most recently used slot (cache() target)
    uint64_t m_cacheClock = 0;   // monotonic LRU stamp source
    CacheStats m_cacheStats;

    IRenderDevice* m_savedDevice = nullptr;
    bool m_initialized = false;
    FontId m_currentFont = FontId::Small;

    bgfx::TextureHandle m_fontTexture    = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_strikeTexture  = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout  m_posTexLayout;
    bgfx::UniformHandle m_texSampler      = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_textProgram = BGFX_INVALID_HANDLE;

    int m_screenWidth  = 1280;
    int m_screenHeight = 720;
    int m_fontGlyphW = 8, m_fontGlyphH = 16;
    int m_atlasCols = 32;
    float m_ttfFontSize = 24.0f;
    std::string m_ttfPath;

    TextCursor m_cursor;
};

} // namespace Caesura
