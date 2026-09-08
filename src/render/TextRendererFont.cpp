#include "TextRenderer.h"
#include "BgfxRenderDevice.h"
#include "NullRenderDevice.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <utility>

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
std::unique_ptr<IPreparedFontState> TextRenderer::prepareFontState(
    const FontRestoreState& state, const uint8_t* bytes, size_t size) {
    if (!validFont(state,size)) return {};
    try {
        if (state.active && state.font!=FontId::TTF) return prepareBitmapFont(state);
        auto prepared=std::make_unique<PreparedFont>();
        prepared->state=state;
        if (!state.active) return prepared;
        if (!bytes || size>static_cast<size_t>((std::numeric_limits<FT_Long>::max)())) return {};
        prepared->ttf=std::make_unique<TTFState>();
        auto& font=*prepared->ttf;
        font.sourceBytes.assign(bytes,bytes+size);
        if (FT_Init_FreeType(&font.ftLib) || FT_New_Memory_Face(font.ftLib,
            font.sourceBytes.data(),static_cast<FT_Long>(font.sourceBytes.size()),0,&font.ftFace)) return {};
        if (FT_Set_Pixel_Sizes(font.ftFace,0,static_cast<FT_UInt>(state.pixelSize))) return {};
        font.ascent=font.ftFace->size->metrics.ascender/64.0f;
        font.descent=font.ftFace->size->metrics.descender/64.0f;
        font.lineGap=0;
        font.atlasPixels.assign(size_t(font.atlasW)*font.atlasH*4,0);
        const uint32_t ranges[][2]={{32,126},{0x2000,0x206f},{0x3000,0x303f},
            {0x3040,0x309f},{0x30a0,0x30ff},{0xff00,0xffef},{0x4e00,0x9fff}};
        for (const auto& range:ranges) {
            for (uint32_t cp=range[0];cp<=range[1];++cp) {
                if (!rasterizeTTFGlyph(font,cp,font.atlasPixels)
                    && font.penY+static_cast<int>(state.pixelSize)+1>=font.atlasH) break;
            }
        }
        if (font.glyphs.empty()) return {};
        prepared->atlasW=font.atlasW; prepared->atlasH=font.atlasH;
        prepared->glyphW=prepared->glyphH=static_cast<int>(state.pixelSize);
        prepared->atlasCols=font.atlasW;
        prepared->lineHeight=font.ftFace->size->metrics.height/64.0f;
        prepared->rememberedTtfPath=state.assetPath;
        prepared->rememberedTtfSize=state.pixelSize;
        std::printf("[TextRenderer] TTF prepared: %zu glyphs rasterized (%dx%d atlas).\n",
            font.glyphs.size(), font.atlasW, font.atlasH);
        return prepared;
    } catch (...) { return {}; }
}

bool TextRenderer::activateFont(PreparedFont& prepared) {
    if (!prepared.state.active) { clearFontState(); return true; }
    auto& pixels=prepared.ttf ? prepared.ttf->atlasPixels : prepared.bitmapPixels;
    if (prepared.atlasW<=0 || prepared.atlasH<=0 || prepared.atlasW>2048 || prepared.atlasH>2048
        || pixels.size()!=size_t(prepared.atlasW)*prepared.atlasH*4) return false;
    const auto* memory=bgfx::copy(pixels.data(),static_cast<uint32_t>(pixels.size()));
    if (!memory) return false;
    const auto texture=bgfx::createTexture2D(static_cast<uint16_t>(prepared.atlasW),
        static_cast<uint16_t>(prepared.atlasH),false,1,bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,memory);
    if (!bgfx::isValid(texture)) return false;
    const auto previous=m_fontTexture;
    m_fontTexture=texture;
    m_currentFont=prepared.state.font;
    m_fontDescription=std::move(prepared.state);
    m_ttf=std::move(prepared.ttf);
    m_bitmapPixels=std::move(prepared.bitmapPixels);
    m_fontGlyphW=prepared.glyphW; m_fontGlyphH=prepared.glyphH;
    m_atlasCols=prepared.atlasCols; m_cursor.lineHeight=prepared.lineHeight;
    m_ttfPath=std::move(prepared.rememberedTtfPath);
    m_ttfFontSize=prepared.rememberedTtfSize;
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
bool TextRenderer::rasterizeTTFGlyph(TTFState& font, uint32_t cp, std::vector<uint8_t>& atlas) {
    if (!font.ftFace) return false;
    if (font.glyphs.count(cp)) return true;

    FT_UInt glyphIndex = FT_Get_Char_Index(font.ftFace, cp);
    if (glyphIndex == 0 && cp != 32) return false;

    FT_Error ftErr = FT_Load_Glyph(font.ftFace, glyphIndex, FT_LOAD_DEFAULT);
    if (ftErr) return false;

    ftErr = FT_Render_Glyph(font.ftFace->glyph, FT_RENDER_MODE_NORMAL);
    if (ftErr) return false;

    FT_Bitmap* bitmap = &font.ftFace->glyph->bitmap;
    int w = (int)bitmap->width;
    int h = (int)bitmap->rows;
    if (w <= 0 || h <= 0) {
        if (cp == 32 || glyphIndex != 0) {
            GlyphMetrics gm{};
            gm.advance = (int)(font.ftFace->glyph->advance.x >> 6);
            font.glyphs[cp] = gm;
            return true;
        }
        return false;
    }

    int advance = (int)(font.ftFace->glyph->advance.x >> 6);
    int xoff = font.ftFace->glyph->bitmap_left;
    int yoff = font.ftFace->glyph->bitmap_top;
    if (!bitmap->buffer || bitmap->pixel_mode!=FT_PIXEL_MODE_GRAY || bitmap->pitch<w) return false;

    // Pack into atlas (simple row packing)
    if (font.penX + w + 1 >= font.atlasW) {
        font.penX = 1;
        font.penY += font.maxRowH + 1;
        font.maxRowH = 0;
    }
    if (font.penY + h + 1 >= font.atlasH) {
        return false;
    }

    // Copy glyph to atlas (FreeType grayscale -> RGBA8 atlas: RGB=255, A=coverage)
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int ax = font.penX + col;
            int ay = font.penY + row;
            uint8_t cov = bitmap->buffer[row * bitmap->pitch + col];
            if (cov > 0) {
                size_t idx = (static_cast<size_t>(ay) * font.atlasW + ax) * 4;
                atlas[idx + 0] = 255;
                atlas[idx + 1] = 255;
                atlas[idx + 2] = 255;
                atlas[idx + 3] = cov;
            }
        }
    }

    GlyphMetrics gm;
    gm.x = font.penX; gm.y = font.penY;
    gm.w = w; gm.h = h;
    gm.advance = advance;
    gm.offsetX = xoff; gm.offsetY = yoff;
    font.glyphs[cp] = gm;

    font.penX += w + 1;
    if (h > font.maxRowH) font.maxRowH = h;
    return true;
}
}
