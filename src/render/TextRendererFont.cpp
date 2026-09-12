#include "TextRenderer.h"
#include "BgfxRenderDevice.h"
#include "NullRenderDevice.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>
#include <algorithm>

namespace Caesura {
namespace {
constexpr size_t kMaxFontBytes=32*1024*1024;
bool validFont(const FontRestoreState& state, size_t size) {
    if (!state.active) return state.assetPath.empty() && state.pixelSize==0 && size==0;
    if (state.font==FontId::Small || state.font==FontId::Large)
        return state.assetPath.empty() && state.pixelSize==(state.font==FontId::Small ? 16 : 32) && size==0;
    return state.font==FontId::TTF && !state.assetPath.empty() && state.assetPath.size()<=4096
        && std::isfinite(state.pixelSize) && std::floor(state.pixelSize)==state.pixelSize
        && state.pixelSize>=1 && state.pixelSize<=256 && size>0 && size<=kMaxFontBytes;
}
}

FontRestoreState TextRenderer::captureFontState() const { return m_fontDescription; }

std::unique_ptr<TextRenderer::GlyphAtlas> TextRenderer::GlyphAtlas::create(
    const uint8_t* bytes, size_t size, float pixelSize) {
    if (!bytes || size == 0 || size > kMaxFontBytes
        || size > static_cast<size_t>((std::numeric_limits<FT_Long>::max)())
        || !std::isfinite(pixelSize) || std::floor(pixelSize) != pixelSize || pixelSize < 1 || pixelSize > 256) return {};
    try {
        auto font = std::unique_ptr<GlyphAtlas>(new GlyphAtlas);
        font->sourceBytes.assign(bytes, bytes + size);
        if (FT_Init_FreeType(&font->ftLib) || FT_New_Memory_Face(font->ftLib,
            font->sourceBytes.data(), static_cast<FT_Long>(font->sourceBytes.size()), 0, &font->ftFace)) return {};
        if (FT_Set_Pixel_Sizes(font->ftFace, 0, static_cast<FT_UInt>(pixelSize))) return {};
        font->ascent = font->ftFace->size->metrics.ascender / 64.0f;
        font->descent = font->ftFace->size->metrics.descender / 64.0f;
        // Straight alpha: even padding/zero-coverage texels must have white
        // RGB so linear sampling does not multiply the coverage a second time.
        font->atlasPixels.assign(size_t(font->atlasW) * font->atlasH * 4, 255);
        for (size_t i = 3; i < font->atlasPixels.size(); i += 4) font->atlasPixels[i] = 0;
        for (const uint32_t replacement : {uint32_t(0xfffd), uint32_t('?')}) {
            if (font->prepareGlyph(replacement) == PrepareStatus::Added) {
                font->replacementCodepoint = replacement;
                break;
            }
        }
        // A font without a drawable replacement cannot provide an honest,
        // bounded missing-glyph fallback. Reject preparation, preserving the
        // active font rather than sampling bitmap UVs from this TTF atlas.
        if (!font->replacementCodepoint || font->replacement().w <= 0 || font->replacement().h <= 0) return {};
        (void)font->prepareGlyph(32);
        return font;
    } catch (...) { return {}; }
}

TextRenderer::GlyphAtlas::PrepareStatus TextRenderer::GlyphAtlas::prepareGlyph(uint32_t codepoint) noexcept {
    if (codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return PrepareStatus::Missing;
    try { return TextRenderer::rasterizeTTFGlyph(*this, codepoint); }
    catch (...) { return PrepareStatus::Error; }
}

const GlyphMetrics* TextRenderer::GlyphAtlas::find(uint32_t codepoint) const {
    const auto found = glyphs.find(codepoint);
    return found == glyphs.end() ? nullptr : &found->second;
}

const GlyphMetrics& TextRenderer::GlyphAtlas::replacement() const {
    static const GlyphMetrics empty{};
    const auto* glyph = find(replacementCodepoint);
    return glyph ? *glyph : empty;
}

const GlyphMetrics& TextRenderer::GlyphAtlas::resolve(uint32_t codepoint) const {
    const auto* glyph = find(codepoint);
    return glyph ? *glyph : replacement();
}

std::unique_ptr<IPreparedFontState> TextRenderer::prepareFontState(
    const FontRestoreState& state, const uint8_t* bytes, size_t size) {
    if (!validFont(state,size)) return {};
    try {
        if (state.active && state.font!=FontId::TTF) return prepareBitmapFont(state);
        auto prepared=std::make_unique<PreparedFont>();
        prepared->state=state;
        if (!state.active) return prepared;
        prepared->ttf=GlyphAtlas::create(bytes,size,state.pixelSize);
        if (!prepared->ttf) return {};
        auto& font=*prepared->ttf;
        prepared->atlasW=font.atlasW; prepared->atlasH=font.atlasH;
        prepared->glyphW=prepared->glyphH=static_cast<int>(state.pixelSize);
        prepared->atlasCols=font.atlasW;
        prepared->lineHeight=font.ftFace->size->metrics.height/64.0f;
        prepared->rememberedTtfPath=state.assetPath;
        prepared->rememberedTtfSize=state.pixelSize;
        std::printf("[TextRenderer] TTF prepared: %zu fallback glyphs (%dx%d append-only atlas; remaining glyphs on demand).\n",
            font.glyphs.size(), font.atlasW, font.atlasH);
        return prepared;
    } catch (...) { return {}; }
}

bool TextRenderer::activateFont(PreparedFont& prepared) {
    if (!prepared.state.active) { clearFontState(); return true; }
    auto& pixels=prepared.ttf ? prepared.ttf->atlasPixels : prepared.bitmapPixels;
    if (prepared.atlasW<=0 || prepared.atlasH<=0 || prepared.atlasW>2048 || prepared.atlasH>2048
        || pixels.size()!=size_t(prepared.atlasW)*prepared.atlasH*4) return false;
    // D3D creates a texture with initial data as IMMUTABLE. A TTF atlas must
    // accept demand uploads, so allocate it empty and queue the owned full
    // upload only after successful creation. Bitmap fonts remain immutable.
    const auto* initial = prepared.ttf ? nullptr
        : bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size()));
    if (!prepared.ttf && !initial) return false;
    const auto texture=bgfx::createTexture2D(static_cast<uint16_t>(prepared.atlasW),
        static_cast<uint16_t>(prepared.atlasH),false,1,bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,initial);
    if (!bgfx::isValid(texture)) return false;
    if (prepared.ttf) {
        const auto* memory = bgfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size()));
        if (!memory) { bgfx::destroy(texture); return false; }
        bgfx::updateTexture2D(texture, 0, 0, 0, 0,
            static_cast<uint16_t>(prepared.atlasW), static_cast<uint16_t>(prepared.atlasH), memory);
    }
    const auto previous=m_fontTexture;
    m_fontTexture=texture;
    m_currentFont=prepared.state.font;
    m_fontDescription=std::move(prepared.state);
    m_ttf=std::move(prepared.ttf);
    if (m_ttf) m_ttf->acknowledgeUpload(); // Full owned atlas upload was queued above.
    m_bitmapPixels=std::move(prepared.bitmapPixels);
    m_fontGlyphW=prepared.glyphW; m_fontGlyphH=prepared.glyphH;
    m_atlasCols=prepared.atlasCols; m_cursor.lineHeight=prepared.lineHeight;
    m_ttfPath=std::move(prepared.rememberedTtfPath);
    m_ttfFontSize=prepared.rememberedTtfSize;
    m_reportedMissingGlyph=m_reportedAtlasFull=m_reportedAtlasUploadFailure=false;
    invalidateCache();
    if (bgfx::isValid(previous)) bgfx::destroy(previous);
    return true;
}

bool TextRenderer::applyFontState(std::unique_ptr<IPreparedFontState> prepared) {
    auto* value=dynamic_cast<PreparedFont*>(prepared.get());
    if (!value || (value->state.active && !m_initialized)) return false;
    return activateFont(*value);
}

void TextRenderer::clearFontState() {
    if (bgfx::isValid(m_fontTexture)) bgfx::destroy(m_fontTexture);
    m_fontTexture=BGFX_INVALID_HANDLE;
    m_ttf.reset(); m_bitmapPixels.clear(); m_ttfPath.clear();
    m_fontDescription={}; m_currentFont=FontId::Small;
    m_cursor.lineHeight=16;
    invalidateCache();
}

std::unique_ptr<IPreparedFontState> TextRenderer::takeFontForDeviceRecovery() {
    auto prepared=std::make_unique<PreparedFont>();
    prepared->state=m_fontDescription;
    prepared->rememberedTtfPath=m_ttfPath;
    prepared->rememberedTtfSize=m_ttfFontSize;
    prepared->atlasW=m_ttf ? m_ttf->atlasW : m_fontGlyphW*32;
    prepared->atlasH=m_ttf ? m_ttf->atlasH : m_fontGlyphH*3;
    prepared->glyphW=m_fontGlyphW; prepared->glyphH=m_fontGlyphH;
    prepared->atlasCols=m_atlasCols; prepared->lineHeight=m_cursor.lineHeight;
    prepared->ttf=std::move(m_ttf);
    prepared->bitmapPixels=std::move(m_bitmapPixels);
    m_fontDescription={};
    return prepared;
}

bool TextRenderer::loadTTF(const char* path, float fontSize) {
    if (!m_initialized || !path || !path[0] || !std::isfinite(fontSize) || fontSize<1 || fontSize>256) return false;
    try {
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if (!input) return false;
        const auto length=input.tellg();
        if (length<=0 || length>static_cast<std::streamoff>(kMaxFontBytes)) return false;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) return false;
        auto prepared=prepareFontState({true,FontId::TTF,path,std::floor(fontSize)},bytes.data(),bytes.size());
        return prepared && applyFontState(std::move(prepared));
    } catch (...) { return false; }
}

FontRestoreState BgfxRenderDevice::captureFontState() const {
    return m_textRenderer ? m_textRenderer->captureFontState() : FontRestoreState{};
}
FontRestoreState BgfxRenderDevice::defaultFontState() const { return {true,FontId::Small,"",16}; }
std::unique_ptr<IPreparedFontState> BgfxRenderDevice::prepareFontState(
    const FontRestoreState& state, const uint8_t* bytes, size_t size) {
    return TextRenderer::prepareFontState(state,bytes,size);
}
bool BgfxRenderDevice::applyFontState(std::unique_ptr<IPreparedFontState> prepared) {
    return canRender() && m_textRenderer && m_textRenderer->applyFontState(std::move(prepared));
}
void BgfxRenderDevice::clearFontState() { if (m_textRenderer) m_textRenderer->clearFontState(); }

FontRestoreState NullRenderDevice::captureFontState() const { return {}; }
FontRestoreState NullRenderDevice::defaultFontState() const { return {}; }
std::unique_ptr<IPreparedFontState> NullRenderDevice::prepareFontState(
    const FontRestoreState& state, const uint8_t* bytes, size_t size) {
    return !state.active ? TextRenderer::prepareFontState(state,bytes,size) : nullptr;
}
bool NullRenderDevice::applyFontState(std::unique_ptr<IPreparedFontState> prepared) {
    return prepared && !prepared->description().active;
}
void NullRenderDevice::clearFontState() {}
TextRenderer::GlyphAtlas::PrepareStatus TextRenderer::rasterizeTTFGlyph(TTFState& font, uint32_t cp) {
    using Status = GlyphAtlas::PrepareStatus;
    if (!font.ftFace) return Status::Error;
    if (font.glyphs.count(cp)) return Status::Existing;
    if (font.glyphs.size() >= GlyphAtlas::MaxGlyphs) return Status::Full;

    FT_UInt glyphIndex = FT_Get_Char_Index(font.ftFace, cp);
    if (glyphIndex == 0) return Status::Missing;

    FT_Error ftErr = FT_Load_Glyph(font.ftFace, glyphIndex, FT_LOAD_DEFAULT);
    if (ftErr) return Status::Error;

    ftErr = FT_Render_Glyph(font.ftFace->glyph, FT_RENDER_MODE_NORMAL);
    if (ftErr) return Status::Error;

    FT_Bitmap* bitmap = &font.ftFace->glyph->bitmap;
    int w = (int)bitmap->width;
    int h = (int)bitmap->rows;
    if (w <= 0 || h <= 0) {
        GlyphMetrics gm{};
        gm.advance = (int)(font.ftFace->glyph->advance.x >> 6);
        font.glyphs.emplace(cp, gm);
        return Status::Added;
    }

    int advance = (int)(font.ftFace->glyph->advance.x >> 6);
    int xoff = font.ftFace->glyph->bitmap_left;
    int yoff = font.ftFace->glyph->bitmap_top;
    if (!bitmap->buffer || bitmap->pixel_mode!=FT_PIXEL_MODE_GRAY || bitmap->num_grays!=256 || bitmap->pitch<w) return Status::Error;

    // Compute the append transaction without changing the published packer.
    // Failure never relocates/overwrites an earlier glyph, including draws
    // already submitted in this frame and UVs resident in LRU cache buffers.
    if (w + 2 > font.atlasW || h + 2 > font.atlasH) return Status::Full;
    int penX=font.penX, penY=font.penY, rowHeight=font.maxRowH;
    if (penX + w + 1 > font.atlasW) {
        penX = 1;
        penY += rowHeight + 1;
        rowHeight = 0;
    }
    if (penY + h + 1 > font.atlasH) return Status::Full;
    GlyphMetrics gm{penX,penY,w,h,advance,xoff,yoff};
    // Allocate the map entry before changing pixels/packer state. Remaining
    // steps are bounded writes without allocation, so a failed emplace leaves
    // the previous atlas and its dirty region unchanged.
    font.glyphs.emplace(cp, gm);

    // Copy glyph to atlas (FreeType grayscale -> RGBA8 atlas: RGB=255, A=coverage)
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int ax = penX + col;
            int ay = penY + row;
            uint8_t cov = bitmap->buffer[row * bitmap->pitch + col];
            size_t idx = (static_cast<size_t>(ay) * font.atlasW + ax) * 4;
            font.atlasPixels[idx + 3] = cov; // RGB stays 255, even when cov is 0.
        }
    }
    const int left=penX-1, top=penY-1, right=penX+w+1, bottom=penY+h+1;
    if (font.dirty.w == 0) font.dirty={left,top,right-left,bottom-top};
    else {
        const int x=std::min(font.dirty.x,left), y=std::min(font.dirty.y,top);
        const int r=std::max(font.dirty.x+font.dirty.w,right), b=std::max(font.dirty.y+font.dirty.h,bottom);
        font.dirty={x,y,r-x,b-y};
    }
    font.penX=penX+w+1; font.penY=penY; font.maxRowH=std::max(rowHeight,h);
    return Status::Added;
}
}
