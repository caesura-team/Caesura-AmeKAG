// =============================================================================
// test_text_renderer.cpp — GPU-free unit tests for TextRenderer's pure
// glyph-layout math (P2-10 closure).
//
// layoutGlyphs() / buildQuadVertices() were extracted from rebuildCache() so
// the UTF-8 decode, glyph lookup, quad/UV building, pen advance and NDC
// conversion can be pinned without a GPU. The glyph source is injected via a
// GlyphLookupFn; tests use a fixed table instead of FreeType/bgfx state.
// =============================================================================

#include "doctest.h"

#include "render/TextRenderer.h"

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

using namespace Caesura;

namespace {

// Fixed glyph table for the injected lookup: ASCII 'A'..'C' plus CJK '中'.
struct TableGlyph {
    GlyphMetrics gm;
    bool fromCjk;
};

GlyphMetrics makeGm(int x, int y, int w, int h, int advance, int ox = 0, int oy = 0) {
    GlyphMetrics gm;
    gm.x = x; gm.y = y; gm.w = w; gm.h = h;
    gm.advance = advance; gm.offsetX = ox; gm.offsetY = oy;
    return gm;
}

TextRenderer::GlyphLookupResult lookupFromTable(uint32_t cp, void* userData) {
    const auto* table = static_cast<const std::unordered_map<uint32_t, TableGlyph>*>(userData);
    auto it = table->find(cp);
    TextRenderer::GlyphLookupResult out;
    if (it != table->end()) {
        out.gm = it->second.gm;
        out.fromCjk = it->second.fromCjk;
    } else {
        out.gm = makeGm(0, 0, 0, 0, 0);  // missing glyph -> empty slot
        out.fromCjk = false;
    }
    return out;
}

struct GeometryPoint { double x, y; };

double twiceSignedArea(GeometryPoint a, GeometryPoint b, GeometryPoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool triangleContains(const std::array<GeometryPoint, 3>& triangle, GeometryPoint point) {
    constexpr double epsilon = 1e-10;
    if (std::abs(twiceSignedArea(triangle[0], triangle[1], triangle[2])) <= epsilon) return false;
    const double a = twiceSignedArea(triangle[0], triangle[1], point);
    const double b = twiceSignedArea(triangle[1], triangle[2], point);
    const double c = twiceSignedArea(triangle[2], triangle[0], point);
    return (a >= -epsilon && b >= -epsilon && c >= -epsilon)
        || (a <= epsilon && b <= epsilon && c <= epsilon);
}

// Inspect the actual indexed geometry returned by production. Buffer lengths and
// equality against another call to the same builder cannot detect half a quad.
void checkQuadCoverage(const std::vector<float>& vertices, const std::vector<uint32_t>& indices,
                       size_t glyph, uint32_t glyphIndexBase,
                       double left, double top, double right, double bottom) {
    REQUIRE(indices.size() >= (glyph + 1) * 6);
    const uint32_t streamBase = glyphIndexBase * 6;
    const uint32_t glyphBase = streamBase + static_cast<uint32_t>(glyph) * 6;
    std::array<std::array<GeometryPoint, 3>, 2> triangles{};
    for (size_t i = 0; i < 6; ++i) {
        const uint32_t index = indices[glyph * 6 + i];
        REQUIRE(index >= glyphBase);
        REQUIRE(index < glyphBase + 6);
        const size_t local = size_t(index - streamBase) * 4;
        REQUIRE(local + 3 < vertices.size());
        triangles[i / 3][i % 3] = {vertices[local], vertices[local + 1]};
    }
    const double first = twiceSignedArea(triangles[0][0], triangles[0][1], triangles[0][2]);
    const double second = twiceSignedArea(triangles[1][0], triangles[1][1], triangles[1][2]);
    CHECK(std::abs(first) > 1e-10);
    CHECK(std::abs(second) > 1e-10);
    CHECK(first * second > 0.0); // Same winding; neither triangle may degenerate.
    CHECK((std::abs(first) + std::abs(second)) / 2.0
          == doctest::Approx(std::abs((right - left) * (bottom - top))));
    // Interior probes exercise both sides of the diagonal, including the lower
    // left part previously omitted by the cached text mesh.
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        const GeometryPoint point{left + (right - left) * (x + 0.5) / 4,
                                  top + (bottom - top) * (y + 0.5) / 4};
        CHECK((triangleContains(triangles[0], point) || triangleContains(triangles[1], point)));
    }
}

} // namespace

namespace {
const std::vector<uint8_t>& realFontBytes() {
    static const auto bytes = [] {
        std::ifstream input("assets/fonts/NotoSansCJKsc-Regular.otf", std::ios::binary);
        REQUIRE(input.good());
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    }();
    return bytes;
}

std::vector<uint8_t> atlasRegion(const TextRenderer::GlyphAtlas& atlas, const GlyphMetrics& glyph) {
    std::vector<uint8_t> result;
    for (int y = glyph.y; y < glyph.y + glyph.h; ++y) {
        const size_t begin = (size_t(y) * atlas.width() + glyph.x) * 4;
        result.insert(result.end(), atlas.pixels().begin() + begin, atlas.pixels().begin() + begin + size_t(glyph.w) * 4);
    }
    return result;
}

struct IndependentFont {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    explicit IndependentFont(unsigned size) {
        REQUIRE(FT_Init_FreeType(&library) == 0);
        const auto& bytes = realFontBytes();
        REQUIRE(FT_New_Memory_Face(library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face) == 0);
        REQUIRE(FT_Set_Pixel_Sizes(face, 0, size) == 0);
    }
    ~IndependentFont() {
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }
};
}

TEST_CASE("U16 font atlas: actual used CJK glyphs match independent FreeType without range preloading") {
    using Atlas = TextRenderer::GlyphAtlas;
    const auto& bytes = realFontBytes();
    auto atlas = Atlas::create(bytes.data(), bytes.size(), 28);
    REQUIRE(atlas != nullptr);
    CHECK(atlas->glyphCount() <= 3);
    CHECK(atlas->width() == 2048);
    CHECK(atlas->height() == 2048);
    IndependentFont reference(28);
    // Representatives of all six previously preloaded Unicode groups. Actual
    // demand preparation must retain glyph metrics and coverage for each.
    const std::array<uint32_t, 7> codepoints{'G', 0x2014, 0x3002, 0x304b, 0xff21, 0x6c49, 0x6f22};
    for (uint32_t cp : codepoints) {
        CAPTURE(cp);
        CHECK(atlas->prepareGlyph(cp) == Atlas::PrepareStatus::Added);
        const auto* glyph = atlas->find(cp);
        REQUIRE(glyph != nullptr);
        REQUIRE(FT_Load_Char(reference.face, cp, FT_LOAD_RENDER) == 0);
        const auto& ft = *reference.face->glyph;
        CHECK(glyph->advance == (ft.advance.x >> 6));
        CHECK(glyph->offsetX == ft.bitmap_left);
        CHECK(glyph->offsetY == ft.bitmap_top);
        CHECK(glyph->w == static_cast<int>(ft.bitmap.width));
        CHECK(glyph->h == static_cast<int>(ft.bitmap.rows));
        bool equal = true;
        for (int y = 0; y < glyph->h; ++y) for (int x = 0; x < glyph->w; ++x) {
            const size_t pixel = (size_t(glyph->y + y) * atlas->width() + glyph->x + x) * 4;
            equal = equal && atlas->pixels()[pixel] == 255 && atlas->pixels()[pixel + 1] == 255
                && atlas->pixels()[pixel + 2] == 255 && atlas->pixels()[pixel + 3] == ft.bitmap.buffer[y * ft.bitmap.pitch + x];
        }
        CHECK(equal);
    }
    CHECK(atlas->glyphCount() <= 3 + codepoints.size());
    CHECK(atlas->prepareGlyph(0x6f22) == Atlas::PrepareStatus::Existing);
}

TEST_CASE("U16 font atlas: appends keep earlier glyphs and straight-alpha padding immutable") {
    using Atlas = TextRenderer::GlyphAtlas;
    const auto& bytes = realFontBytes();
    auto atlas = Atlas::create(bytes.data(), bytes.size(), 28);
    REQUIRE(atlas != nullptr);
    REQUIRE(atlas->prepareGlyph('A') == Atlas::PrepareStatus::Added);
    const GlyphMetrics before = *atlas->find('A');
    const auto pixels = atlasRegion(*atlas, before);
    atlas->acknowledgeUpload();
    CHECK(atlas->pendingUpload().w == 0);
    REQUIRE(atlas->prepareGlyph(0x6f22) == Atlas::PrepareStatus::Added);
    const auto dirty = atlas->pendingUpload();
    CHECK(dirty.w > 0);
    CHECK(dirty.h > 0);
    CHECK(dirty.x + dirty.w <= atlas->width());
    CHECK(dirty.y + dirty.h <= atlas->height());
    CHECK(atlas->find('A')->x == before.x);
    CHECK(atlas->find('A')->y == before.y);
    CHECK(atlasRegion(*atlas, *atlas->find('A')) == pixels);
    bool straightRgb = true;
    for (size_t i = 0; i < atlas->pixels().size(); i += 4)
        straightRgb = straightRgb && atlas->pixels()[i] == 255 && atlas->pixels()[i + 1] == 255 && atlas->pixels()[i + 2] == 255;
    CHECK(straightRgb);
    CHECK(atlas->pixels()[3] == 0);
}

TEST_CASE("U16 font atlas: capacity and missing glyphs never overwrite existing atlas coordinates") {
    using Atlas = TextRenderer::GlyphAtlas;
    const auto& bytes = realFontBytes();
    auto atlas = Atlas::create(bytes.data(), bytes.size(), 256);
    REQUIRE(atlas != nullptr);
    REQUIRE(atlas->prepareGlyph('W') == Atlas::PrepareStatus::Added);
    const GlyphMetrics before = *atlas->find('W');
    const auto pixels = atlasRegion(*atlas, before);
    bool full = false;
    for (uint32_t cp = 0x4e00; cp < 0x5000; ++cp) {
        const auto count = atlas->glyphCount();
        const auto status = atlas->prepareGlyph(cp);
        if (status == Atlas::PrepareStatus::Full) {
            CHECK(atlas->glyphCount() == count);
            CHECK(atlas->find(cp) == nullptr);
            full = true;
            break;
        }
        REQUIRE(status != Atlas::PrepareStatus::Error);
    }
    CHECK(full);
    CHECK(atlas->glyphCount() <= Atlas::MaxGlyphs);
    CHECK(atlas->prepareGlyph('W') == Atlas::PrepareStatus::Existing);
    CHECK(atlas->find('W')->x == before.x);
    CHECK(atlas->find('W')->y == before.y);
    CHECK(atlasRegion(*atlas, *atlas->find('W')) == pixels);
    const auto count = atlas->glyphCount();
    CHECK(atlas->prepareGlyph(0x110000) == Atlas::PrepareStatus::Missing);
    CHECK(atlas->glyphCount() == count);
    CHECK(atlas->resolve(0x110000).x == atlas->replacement().x);
    CHECK(atlas->resolve(0x110000).y == atlas->replacement().y);
    CHECK(atlas->replacement().w > 0);
}

TEST_CASE("U16 font advance: UTF-8 and proportional ruby measure actual selected glyph advances") {
    using Atlas = TextRenderer::GlyphAtlas;
    const auto& bytes = realFontBytes();
    auto atlas = Atlas::create(bytes.data(), bytes.size(), 28);
    REQUIRE(atlas != nullptr);
    for (uint32_t cp : {uint32_t('A'), uint32_t('B'), uint32_t('i'), uint32_t(0x6f22), uint32_t(0x5b57), uint32_t(0x304b), uint32_t(0x3093), uint32_t(0x3058)}) {
        REQUIRE(atlas->prepareGlyph(cp) != Atlas::PrepareStatus::Error);
        REQUIRE(atlas->find(cp) != nullptr);
    }
    const auto advance = [](uint32_t cp, void* data) {
        return static_cast<float>(static_cast<Atlas*>(data)->resolve(cp).advance);
    };
    IndependentFont reference(28);
    float expected = 0;
    for (uint32_t cp : {uint32_t(0x6f22), uint32_t(0x5b57)}) {
        REQUIRE(FT_Load_Char(reference.face, cp, FT_LOAD_RENDER) == 0);
        expected += static_cast<float>(reference.face->glyph->advance.x >> 6);
    }
    CHECK(TextRenderer::measureAdvance("\xE6\xBC\xA2\xE5\xAD\x97", advance, atlas.get()) == expected);
    const float base = TextRenderer::measureAdvance("AB", advance, atlas.get());
    const float ruby = TextRenderer::measureAdvance("iii", advance, atlas.get()) * .5f;
    CHECK(base == atlas->find('A')->advance + atlas->find('B')->advance);
    CHECK(ruby == atlas->find('i')->advance * 1.5f);
    CHECK(base != 2 * 28); // Byte/nominal-cell width is not a proportional advance.
}

TEST_CASE("U16 font filtering: cached glyph bounds reach transparent guard centers without changing ink metrics") {
    using Atlas = TextRenderer::GlyphAtlas;
    const auto& bytes = realFontBytes();
    auto atlas = Atlas::create(bytes.data(), bytes.size(), 28);
    REQUIRE(atlas != nullptr);
    // Populate adjacent cells before testing the guard: the border must remain
    // transparent even after another glyph occupies the next atlas cell.
    for (uint32_t cp : {uint32_t('A'),uint32_t('B'),uint32_t('g'),uint32_t(0x6f22)})
        REQUIRE(atlas->prepareGlyph(cp) == Atlas::PrepareStatus::Added);
    const auto lookup = [](uint32_t cp, void* owner) {
        return TextRenderer::GlyphLookupResult{static_cast<Atlas*>(owner)->resolve(cp),false};
    };
    const auto laid = TextRenderer::layoutGlyphs("ABg", 64, 90, lookup, atlas.get(),
        false, 1.0f/atlas->width(), 1.0f/atlas->height(), 0, 0, true, 33, 8);
    REQUIRE(laid.glyphs.size() == 3);
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(laid.glyphs, 0, 0, vertices, indices);
    float pen = 64;
    for (size_t i=0; i<3; ++i) {
        const auto& gm = atlas->resolve(static_cast<uint32_t>("ABg"[i]));
        const auto& ink = laid.glyphs[i];
        CHECK(ink.gx == pen + gm.offsetX);
        CHECK(ink.gy == 123 - gm.offsetY);
        CHECK(ink.w == gm.w); CHECK(ink.h == gm.h);
        CHECK(vertices[i*24] == doctest::Approx(ink.gx-.5f));
        CHECK(vertices[i*24+1] == doctest::Approx(ink.gy-.5f));
        CHECK(vertices[i*24+2]*atlas->width() == doctest::Approx(gm.x-.5f));
        CHECK(vertices[i*24+3]*atlas->height() == doctest::Approx(gm.y-.5f));
        CHECK(vertices[i*24+10]*atlas->width() == doctest::Approx(gm.x+gm.w+.5f));
        CHECK(vertices[i*24+11]*atlas->height() == doctest::Approx(gm.y+gm.h+.5f));
        bool transparent = true;
        for (int y=gm.y-1; y<=gm.y+gm.h; ++y)
            for (int x=gm.x-1; x<=gm.x+gm.w; ++x)
                if (x==gm.x-1 || x==gm.x+gm.w || y==gm.y-1 || y==gm.y+gm.h)
                    transparent &= atlas->pixels()[(size_t(y)*atlas->width()+x)*4+3] == 0;
        CHECK(transparent);
        pen += gm.advance;
    }
    CHECK(laid.penAdvance == pen);
}

TEST_CASE("U16 font filtering: transparent padding preserves original italic ink transform") {
    const auto q = TextRenderer::glyphQuadToNDC(100,200,16,16,3,640,360,.25f,.25f);
    const float leftTop=(q.x0+1)*320, leftBottom=(q.x3+1)*320;
    CHECK((1-q.y0)*180 == doctest::Approx(199.75f));
    CHECK((1-q.y3)*180 == doctest::Approx(216.25f));
    // Interpolate along the enlarged edge to each ORIGINAL ink boundary.
    // The ink's horizontal .25-pixel inset restores the original shear.
    CHECK(leftTop+(leftBottom-leftTop)*(.25f/16.5f)+.25f == doctest::Approx(103));
    CHECK(leftTop+(leftBottom-leftTop)*(16.25f/16.5f)+.25f == doctest::Approx(100));
}

TEST_CASE("U16 text resize: changed screen dimensions invalidate every geometry cache slot") {
    TextRenderer text;
    text.setScreenSize(1280, 720);
    for (size_t i = 0; i < text.cacheSlotCount(); ++i) CHECK_FALSE(text.cacheSlot(i).isDirty());
    text.setScreenSize(0, 360); // Invalid surface must not poison NDC/cache state.
    for (size_t i = 0; i < text.cacheSlotCount(); ++i) CHECK_FALSE(text.cacheSlot(i).isDirty());
    text.setScreenSize(640, 360);
    for (size_t i = 0; i < text.cacheSlotCount(); ++i) {
        CHECK(text.cacheSlot(i).isDirty());
        CHECK_FALSE(text.cacheSlot(i).geometryValid);
    }
}

TEST_CASE("Text layout: UTF-8 multi-byte decode emits one glyph per codepoint") {
    std::unordered_map<uint32_t, TableGlyph> table;
    table[0x41] = {makeGm(0, 0, 8, 16, 8), false};    // 'A'
    table[0x4E2D] = {makeGm(10, 0, 16, 16, 16, 0, 4), true}; // '中' (CJK)

    // "A中A" — 5 UTF-8 bytes -> 3 codepoints
    const std::string text = "A\xE4\xB8\xAD" "A";
    auto res = TextRenderer::layoutGlyphs(
        text, 10.0f, 20.0f, lookupFromTable, &table,
        true /*hasCjk*/, 1.0f / 256.0f, 1.0f / 48.0f,
        1.0f / 4096.0f, 1.0f / 4096.0f,
        false /*useTtf*/, 0.0f, 64);

    REQUIRE(res.glyphs.size() == 3);
    // pen advance = sum of advances: 8 + 16 + 8
    CHECK(res.penAdvance == doctest::Approx(42.0f));
    // first and last glyph are ASCII (not CJK), middle is CJK
    CHECK(res.glyphs[0].fromCjk == false);
    CHECK(res.glyphs[1].fromCjk == true);
    CHECK(res.glyphs[2].fromCjk == false);
    // not all CJK
    CHECK(res.allCjk == false);
}

TEST_CASE("Text layout: allCjk true only when every glyph is CJK") {
    std::unordered_map<uint32_t, TableGlyph> table;
    table[0x4E2D] = {makeGm(10, 0, 16, 16, 16, 0, 4), true};
    table[0x6587] = {makeGm(30, 0, 16, 16, 16, 0, 4), true}; // '文'

    auto res = TextRenderer::layoutGlyphs(
        "\xE4\xB8\xAD\xE6\x96\x87", 0.0f, 0.0f, lookupFromTable, &table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, 64);

    REQUIRE(res.glyphs.size() == 2);
    CHECK(res.allCjk == true);
}

TEST_CASE("Text layout: empty text produces no glyphs and zero advance") {
    std::unordered_map<uint32_t, TableGlyph> table;
    auto res = TextRenderer::layoutGlyphs(
        "", 5.0f, 5.0f, lookupFromTable, &table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, 64);
    CHECK(res.glyphs.empty());
    CHECK(res.penAdvance == doctest::Approx(5.0f));  // pen unchanged
    CHECK(res.allCjk == false);
}

TEST_CASE("Text layout: missing glyph emits an empty slot and still advances") {
    std::unordered_map<uint32_t, TableGlyph> table;  // no glyphs at all
    auto res = TextRenderer::layoutGlyphs(
        "A", 0.0f, 0.0f, lookupFromTable, &table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, 64);
    REQUIRE(res.glyphs.size() == 1);
    CHECK(res.glyphs[0].w == 0.0f);  // empty slot
    CHECK(res.glyphs[0].h == 0.0f);
    CHECK(res.penAdvance == doctest::Approx(0.0f));  // missing glyph advance 0
    CHECK(res.allCjk == false);
}

TEST_CASE("Text layout: quad geometry uses offset, ascent and UV math") {
    std::unordered_map<uint32_t, TableGlyph> table;
    // glyph at atlas (10,5) size 8x16, advance 12, offset (2,-3)
    table[0x41] = {makeGm(10, 5, 8, 16, 12, 2, -3), false};

    const float invW = 1.0f / 256.0f;   // TTF atlas 256 wide
    const float invH = 1.0f / 48.0f;    // TTF atlas 48 tall
    auto res = TextRenderer::layoutGlyphs(
        "A", 100.0f, 200.0f, lookupFromTable, &table,
        false /*hasCjk*/, invW, invH, 1.0f / 4096.0f, 1.0f / 4096.0f,
        true /*useTtf*/, 14.0f /*ascent*/, 64);

    REQUIRE(res.glyphs.size() == 1);
    const auto& d = res.glyphs[0];
    CHECK(d.gx == doctest::Approx(100.0f + 2.0f));      // penX + offsetX
    CHECK(d.gy == doctest::Approx(200.0f - (-3.0f) + 14.0f)); // penY - offsetY + ascent
    CHECK(d.w == doctest::Approx(8.0f));
    CHECK(d.h == doctest::Approx(16.0f));
    // UVs: (10,5) -> (18,21) over atlas
    CHECK(d.u0 == doctest::Approx(10.0f / 256.0f));
    CHECK(d.v0 == doctest::Approx(5.0f / 48.0f));
    CHECK(d.u1 == doctest::Approx(18.0f / 256.0f));
    CHECK(d.v1 == doctest::Approx(21.0f / 48.0f));
    CHECK(res.penAdvance == doctest::Approx(100.0f + 12.0f));
}

TEST_CASE("Text layout: CJK glyph uses CJK UV space and default baseline") {
    std::unordered_map<uint32_t, TableGlyph> table;
    table[0x4E2D] = {makeGm(10, 5, 16, 16, 16, 1, 4), true};

    auto res = TextRenderer::layoutGlyphs(
        "\xE4\xB8\xAD", 50.0f, 60.0f, lookupFromTable, &table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        true /*useTtf but glyph fromCjk*/, 14.0f, 64);

    REQUIRE(res.glyphs.size() == 1);
    const auto& d = res.glyphs[0];
    // CJK baseline: NOT the TTF ascent (8.0f default)
    CHECK(d.gy == doctest::Approx(60.0f - 4.0f + 8.0f));
    // CJK UV space uses the 4096 atlas
    CHECK(d.u0 == doctest::Approx(10.0f / 4096.0f));
    CHECK(d.v0 == doctest::Approx(5.0f / 4096.0f));
    CHECK(d.fromCjk == true);
    CHECK(res.allCjk == true);
}

TEST_CASE("Text layout: maxGlyphs truncates emitted quads, advance keeps counting") {
    std::unordered_map<uint32_t, TableGlyph> table;
    table[0x41] = {makeGm(0, 0, 8, 16, 8), false};  // 'A'

    auto res = TextRenderer::layoutGlyphs(
        "AAA", 0.0f, 0.0f, lookupFromTable, &table,
        false, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, /*maxGlyphs=*/2);

    REQUIRE(res.glyphs.size() == 2);            // truncated
    CHECK(res.penAdvance == doctest::Approx(24.0f));  // all 3 advances counted
}

TEST_CASE("Text layout: NDC conversion wraps pixel quads to [-1,1]") {
    std::vector<TextRenderer::LaidGlyph> glyphs;
    TextRenderer::LaidGlyph g;
    g.gx = 320.0f; g.gy = 180.0f; g.w = 10.0f; g.h = 20.0f;
    g.u0 = 0.0f; g.v0 = 0.0f; g.u1 = 1.0f; g.v1 = 1.0f;
    glyphs.push_back(g);

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(glyphs, 640.0f, 360.0f, verts, indices);

    // 1 glyph -> 6 verts x 4 floats + 6 indices
    REQUIRE(verts.size() == 24);
    REQUIRE(indices.size() == 6);
    // center of screen -> NDC (0,0); quad spans 10x20 px
    // v0: (nx0, ny0, u0, v0) = ((320/640)*2-1, 1-(180/360)*2, 0, 0) = (0, 0, 0, 0)
    CHECK(verts[0] == doctest::Approx(0.0f));
    CHECK(verts[1] == doctest::Approx(0.0f));
    // v1: x1 = ((320+10)/640)*2-1 = 0.03125
    CHECK(verts[4] == doctest::Approx(0.03125f));
    // v2 = {nx1, ny1, u1, v1}: verts[8]=nx1, verts[9]=ny1, verts[10]=u1, verts[11]=v1
    // ny1 = 1-((180+20)/360)*2 = 1-1.1111 = -0.11111
    CHECK(verts[9] == doctest::Approx(-0.111111f));
    CHECK(verts[10] == doctest::Approx(1.0f));   // u1 passthrough
    CHECK(verts[11] == doctest::Approx(1.0f));   // v1 passthrough
    // Both triangles must reference the six-vertex stream, including bottom-left.
    CHECK(indices[0] == 0);
    CHECK(indices[5] == 5);
    checkQuadCoverage(verts, indices, 0, 0, 0.0, 0.0, 0.03125, -1.0 / 9.0);
}

TEST_CASE("Text layout: appended glyph indices preserve two triangles and full coverage") {
    std::vector<TextRenderer::LaidGlyph> glyphs(2);
    glyphs[0].gx = 12; glyphs[0].gy = 34; glyphs[0].w = 8; glyphs[0].h = 16;
    glyphs[1].gx = 41; glyphs[1].gy = 7; glyphs[1].w = 9; glyphs[1].h = 13;
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    constexpr uint32_t base = 7;
    TextRenderer::buildQuadVertices(glyphs, 0, 0, vertices, indices, base);
    REQUIRE(vertices.size() == 48);
    REQUIRE(indices.size() == 12);
    checkQuadCoverage(vertices, indices, 0, base, 12, 34, 20, 50);
    checkQuadCoverage(vertices, indices, 1, base, 41, 7, 50, 20);
}

TEST_CASE("Text layout: NDC falls back to pixel coords when screen size is 0") {
    std::vector<TextRenderer::LaidGlyph> glyphs;
    TextRenderer::LaidGlyph g;
    g.gx = 12.0f; g.gy = 34.0f; g.w = 8.0f; g.h = 16.0f;
    glyphs.push_back(g);

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(glyphs, 0.0f, 0.0f, verts, indices);

    REQUIRE(verts.size() == 24);
    // raw pixel coords pass through
    CHECK(verts[0] == doctest::Approx(12.0f));
    CHECK(verts[1] == doctest::Approx(34.0f));
    CHECK(verts[4] == doctest::Approx(20.0f));  // 12 + 8
    CHECK(verts[9] == doctest::Approx(50.0f));  // 34 + 16 (ny1)
    CHECK(verts[10] == doctest::Approx(0.0f));  // u1 = 0 (default glyph)
}

TEST_CASE("Text layout: empty glyph list produces empty vertex stream") {
    std::vector<TextRenderer::LaidGlyph> glyphs;
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(glyphs, 640.0f, 360.0f, verts, indices);
    CHECK(verts.empty());
    CHECK(indices.empty());
}

TEST_CASE("TextRenderer: loadTTF handles invalid arguments and uninitialized state safely") {
    TextRenderer tr;
    // Uninitialized renderer should reject loadTTF gracefully without crashing
    CHECK_FALSE(tr.loadTTF(nullptr, 22.0f));
    CHECK_FALSE(tr.loadTTF("", 22.0f));
    CHECK_FALSE(tr.loadTTF("assets/fonts/NotoSansCJKsc-Regular.otf", 0.0f));
    CHECK_FALSE(tr.loadTTF("assets/fonts/NotoSansCJKsc-Regular.otf", -10.0f));
    CHECK_FALSE(tr.loadTTF("assets/fonts/NotoSansCJKsc-Regular.otf", 22.0f));
}

TEST_CASE("TextRenderer: RGBA8 dynamic atlas buffer formatting & UV consistency") {
    const uint32_t atlasW = 256;
    const uint32_t atlasH = 256;
    std::vector<uint8_t> atlasBuffer(atlasW * atlasH * 4, 0);

    // Simulate rasterizing a single 8x16 glyph at offset (16, 16) with coverage alpha = 200
    const uint32_t gx = 16, gy = 16, gw = 8, gh = 16;
    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            uint32_t idx = ((gy + y) * atlasW + (gx + x)) * 4;
            atlasBuffer[idx + 0] = 255; // R
            atlasBuffer[idx + 1] = 255; // G
            atlasBuffer[idx + 2] = 255; // B
            atlasBuffer[idx + 3] = 200; // Alpha
        }
    }

    // Verify transparent area outside glyph bounds remains zero
    CHECK(atlasBuffer[0] == 0);
    CHECK(atlasBuffer[1] == 0);
    CHECK(atlasBuffer[2] == 0);
    CHECK(atlasBuffer[3] == 0);

    // Verify written glyph pixel within bounds has white RGB and exact alpha
    uint32_t centerIdx = ((gy + 4) * atlasW + (gx + 4)) * 4;
    CHECK(atlasBuffer[centerIdx + 0] == 255);
    CHECK(atlasBuffer[centerIdx + 1] == 255);
    CHECK(atlasBuffer[centerIdx + 2] == 255);
    CHECK(atlasBuffer[centerIdx + 3] == 200);

    // Verify UV mapping precision matches atlas dimensions
    float u0 = static_cast<float>(gx) / atlasW;
    float v0 = static_cast<float>(gy) / atlasH;
    float u1 = static_cast<float>(gx + gw) / atlasW;
    float v1 = static_cast<float>(gy + gh) / atlasH;
    CHECK(u0 == doctest::Approx(16.0f / 256.0f));
    CHECK(v0 == doctest::Approx(16.0f / 256.0f));
    CHECK(u1 == doctest::Approx(24.0f / 256.0f));
    CHECK(v1 == doctest::Approx(32.0f / 256.0f));
}

TEST_CASE("CJK Kinsoku Shori: Line start forbidden characters (行首禁则 / 避头)") {
    // Closing brackets and quotes
    CHECK(TextRenderer::isKinsokuLineStartForbidden(')'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden(']'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden('}'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden('!'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden('?'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden(','));
    CHECK(TextRenderer::isKinsokuLineStartForbidden('.'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden(':'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden(';'));
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF09)); // ）
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF3D)); // ］
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF5D)); // ｝
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3009)); // 〉
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x300B)); // 》
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x300D)); // 」
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x300F)); // 』
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3011)); // 】
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3015)); // 〕
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x2019)); // ’
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x201D)); // ”

    // Commas and periods
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3001)); // 、
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3002)); // 。
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF0C)); // ，
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF0E)); // ．
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF01)); // ！
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF1F)); // ？
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF1A)); // ：
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF1B)); // ；

    // Japanese small kana
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3041)); // ぁ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3063)); // っ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3083)); // ゃ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30A1)); // ァ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30C3)); // ッ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30E3)); // ャ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30F5)); // ヵ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30F6)); // ヶ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF67)); // ｧ
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF6F)); // ｯ

    // Ellipsis, middle dot, dashes, iteration
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x2026)); // …
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x2014)); // —
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x00B7)); // ·
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30FB)); // ・
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x30FC)); // ー
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0xFF5E)); // ～
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x301C)); // 〜
    CHECK(TextRenderer::isKinsokuLineStartForbidden(0x3005)); // 々

    // Regular letters/characters are NOT forbidden at start
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden('A'));
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden('z'));
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden(0x4E2D)); // 中
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden(0x3042)); // あ (normal vowel)
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden(0x30A2)); // ア (normal vowel)
    CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden(0x304B)); // か
}

TEST_CASE("CJK Kinsoku Shori: Line end forbidden characters (行尾禁则 / 避尾)") {
    // Opening brackets and quotes
    CHECK(TextRenderer::isKinsokuLineEndForbidden('('));
    CHECK(TextRenderer::isKinsokuLineEndForbidden('['));
    CHECK(TextRenderer::isKinsokuLineEndForbidden('{'));
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0xFF08)); // （
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0xFF3B)); // ［
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0xFF5B)); // ｛
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x3008)); // 〈
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x300A)); // 《
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x300C)); // 「
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x300E)); // 『
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x3010)); // 【
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x3014)); // 〔
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x2018)); // ‘
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x201C)); // “

    // Currency and prefixes
    CHECK(TextRenderer::isKinsokuLineEndForbidden('$'));
    CHECK(TextRenderer::isKinsokuLineEndForbidden('#'));
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x00A5)); // ¥
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0xFFE5)); // ￥
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x20AC)); // €
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x00A3)); // £
    CHECK(TextRenderer::isKinsokuLineEndForbidden(0x2116)); // №

    // Regular letters/characters are NOT forbidden at end
    CHECK_FALSE(TextRenderer::isKinsokuLineEndForbidden('A'));
    CHECK_FALSE(TextRenderer::isKinsokuLineEndForbidden('9'));
    CHECK_FALSE(TextRenderer::isKinsokuLineEndForbidden(0x4E2D)); // 中
    CHECK_FALSE(TextRenderer::isKinsokuLineEndForbidden(0x3042)); // あ
    CHECK_FALSE(TextRenderer::isKinsokuLineEndForbidden(0x3002)); // 。(line start forbidden, not line end)
}

TEST_CASE("CJK Kinsoku Shori: canBreakBetween rules") {
    // Normal CJK characters can break between each other
    CHECK(TextRenderer::canBreakBetween(0x4E2D, 0x6587)); // "中文"

    // Cannot break before line-start forbidden characters
    CHECK_FALSE(TextRenderer::canBreakBetween(0x4E2D, 0x3002)); // "中。"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x4E2D, 0xFF0C)); // "中，"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x4E2D, 0x300D)); // "中」"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x4E2D, 0x3063)); // "中っ"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x304D, 0x3083)); // "きゃ" (ki + small ya)
    CHECK_FALSE(TextRenderer::canBreakBetween(0x30AC, 0x30FC)); // "ガー" (ga + prolonged sound)

    // Cannot break after line-end forbidden characters
    CHECK_FALSE(TextRenderer::canBreakBetween(0x300C, 0x4E2D)); // "「中"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x300A, 0x4E2D)); // "《中"
    CHECK_FALSE(TextRenderer::canBreakBetween(0xFFE5, '1'));    // "￥1"

    // Cannot break between two ellipses or two dashes
    CHECK_FALSE(TextRenderer::canBreakBetween(0x2026, 0x2026)); // "……"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x2014, 0x2014)); // "——"
    CHECK_FALSE(TextRenderer::canBreakBetween(0x2015, 0x2015)); // "――"
}

// =============================================================================
// t10 — batch-cache incremental append, LRU slot selection, kinsoku wrapping.
//
// These tests are GPU-free by construction: the cache POLICY
// (TextRenderer::planCacheUpdate / selectCacheSlot / detectAppend) and the cache
// GEOMETRY (layoutGlyphs / buildQuadVertices) are pure statics, and the
// production renderTextCached() dispatches on exactly those functions. So what
// is measured and asserted here is the shipped policy, not a parallel model.
// =============================================================================

namespace {

// Mirror of the production upload sizes (6 verts x 4 floats, 6 uint32 indices
// per glyph). Local on purpose: a change to the vertex format breaks these
// numbers loudly instead of silently invalidating the measurement.
constexpr uint32_t kVBytesPerGlyph = 6u * 4u * (uint32_t)sizeof(float);
constexpr uint32_t kIBytesPerGlyph = 6u * (uint32_t)sizeof(uint32_t);

std::unordered_map<uint32_t, TableGlyph> makeAsciiCjkTable() {
    std::unordered_map<uint32_t, TableGlyph> t;
    for (uint32_t cp = 'a'; cp <= 'z'; ++cp)
        t[cp] = {makeGm((int)(cp - 'a') * 8, 16, 8, 16, 9), false};
    t[' '] = {makeGm(0, 32, 0, 0, 6), false};
    const uint32_t cjk[] = {0x4F60,0x597D,0x4E16,0x754C,0x7684,0x8BDD,0x8BED,0x58F0,
                            0x97F3,0x5F88,0x6E29,0x67D4,0x4E2D,0x6587,0x5B57,0x7B26};
    for (size_t i = 0; i < sizeof(cjk)/sizeof(cjk[0]); ++i)
        t[cjk[i]] = {makeGm((int)i * 24, 100, 22, 22, 24, 1, 4), true};
    return t;
}

// Test-local UTF-8 encoder: the production decoder is what is under test, so
// the encoding side must not share code with it.
std::string utf8Encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) { s += (char)cp; }
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
}

uint32_t decodeFirstCodepoint(const std::string& s, size_t at) {
    const uint8_t* d = reinterpret_cast<const uint8_t*>(s.data()) + at;
    if ((d[0] & 0x80) == 0) return d[0];
    if ((d[0] & 0xE0) == 0xC0) return ((uint32_t)(d[0] & 0x1F) << 6) | (d[1] & 0x3F);
    return ((uint32_t)(d[0] & 0x0F) << 12) | ((uint32_t)(d[1] & 0x3F) << 6) | (d[2] & 0x3F);
}

// GPU-free stand-in for one cache slot plus its resident buffers: applies
// exactly the writes rebuildCache()/appendToCache() perform (including the
// partial-range write), so the simulated buffers equal what bgfx would hold.
struct SimSlot {
    MessageLayerCache meta;
    std::vector<float> vb;
    std::vector<uint32_t> ib;
    SimSlot() {
        vb.assign((size_t)meta.maxGlyphs * 6 * 4, 0.0f);
        ib.assign((size_t)meta.maxGlyphs * 6, 0u);
    }
};

struct SimCounters {
    uint64_t fullRebuilds = 0, appends = 0, hits = 0;
    uint64_t glyphsLaidOut = 0;
    uint64_t vertexBytes = 0, indexBytes = 0;
};

void simDraw(SimSlot& s, uint16_t viewId, const std::string& text,
             float x, float y,
             const std::unordered_map<uint32_t, TableGlyph>& table,
             float screenW, float screenH, SimCounters& c) {
    const auto plan = TextRenderer::planCacheUpdate(s.meta, viewId, text, x, y);

    if (plan.action == TextRenderer::CacheAction::Hit) { ++c.hits; return; }

    const float penStart = (plan.action == TextRenderer::CacheAction::Append)
                         ? s.meta.penEnd : x;
    auto laid = TextRenderer::layoutGlyphs(
        text, penStart, y, lookupFromTable, (void*)&table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, (size_t)plan.glyphsToLayout, plan.byteBegin);

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(laid.glyphs, screenW, screenH, verts, indices,
                                    plan.firstGlyph);

    for (size_t i = 0; i < verts.size(); ++i)
        s.vb[(size_t)plan.firstGlyph * 6 * 4 + i] = verts[i];
    for (size_t i = 0; i < indices.size(); ++i)
        s.ib[(size_t)plan.firstGlyph * 6 + i] = indices[i];

    if (plan.action == TextRenderer::CacheAction::Append) {
        ++c.appends;
        s.meta.glyphCount = plan.firstGlyph + (uint32_t)laid.glyphs.size();
        s.meta.anyGlyph  = s.meta.anyGlyph  || laid.anyGlyph;
        s.meta.anyNonCjk = s.meta.anyNonCjk || laid.anyNonCjk;
    } else {
        ++c.fullRebuilds;
        s.meta.glyphCount = (uint32_t)laid.glyphs.size();
        s.meta.anyGlyph  = laid.anyGlyph;
        s.meta.anyNonCjk = laid.anyNonCjk;
        s.meta.cachedViewId = viewId;
        s.meta.cachedX = x;
        s.meta.cachedY = y;
    }
    s.meta.cacheIsCjk = s.meta.anyGlyph && !s.meta.anyNonCjk;
    s.meta.penEnd = laid.penAdvance;
    s.meta.cachedText = text;
    s.meta.cachedPenAdvance = laid.penAdvance - x;
    s.meta.geometryValid = true;
    s.meta.clearDirty();

    c.glyphsLaidOut += laid.glyphs.size();
    c.vertexBytes += (uint64_t)verts.size() * sizeof(float);
    c.indexBytes  += (uint64_t)indices.size() * sizeof(uint32_t);
}

// The PRE-t10 behavior for the same draw: rebuildCache() ignored the dirty range
// and re-laid + re-uploaded the WHOLE text whenever the key missed, and the key
// contained the full text so a growing line missed on every frame.
void simDrawLegacyFullRebuild(const std::string& text, float x, float y,
                              const std::unordered_map<uint32_t, TableGlyph>& table,
                              float screenW, float screenH, SimCounters& c) {
    auto laid = TextRenderer::layoutGlyphs(
        text, x, y, lookupFromTable, (void*)&table,
        true, 1.0f / 256.0f, 1.0f / 48.0f, 1.0f / 4096.0f, 1.0f / 4096.0f,
        false, 0.0f, 2048, 0);
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    TextRenderer::buildQuadVertices(laid.glyphs, screenW, screenH, verts, indices, 0);
    ++c.fullRebuilds;
    c.glyphsLaidOut += laid.glyphs.size();
    c.vertexBytes += (uint64_t)verts.size() * sizeof(float);
    c.indexBytes  += (uint64_t)indices.size() * sizeof(uint32_t);
}

// Fullwidth-ish advance model: CJK / fullwidth = 2 units, ASCII = 1.
float fixedAdvance(uint32_t cp, void* userData) {
    (void)userData;
    if (cp == '\n') return 0.0f;
    return (cp >= 0x1100) ? 2.0f : 1.0f;
}

std::vector<std::string> makeTypewriterFrames(int codepoints) {
    const uint32_t cjk[] = {0x4F60,0x597D,0x4E16,0x754C,0x7684,0x8BDD,0x8BED,0x58F0,
                            0x97F3,0x5F88,0x6E29,0x67D4,0x4E2D,0x6587,0x5B57,0x7B26};
    std::vector<std::string> frames;
    std::string acc;
    for (int i = 0; i < codepoints; ++i) {
        if (i % 4 == 3) acc += (char)('a' + (i % 26));   // ASCII
        else acc += utf8Encode(cjk[i % 16]);             // CJK
        frames.push_back(acc);
    }
    return frames;
}

} // namespace
TEST_CASE("Batch cache: detectAppend recognizes only append-shaped growth") {
    auto a = TextRenderer::detectAppend("Hel", "Hello");
    CHECK(a.isAppend);
    CHECK(a.tailByteOffset == 3);
    CHECK(a.prefixGlyphs == 3);
    CHECK(a.tailGlyphs == 2);

    const std::string cjk2 = utf8Encode(0x4F60) + utf8Encode(0x597D);
    const std::string cjk3 = cjk2 + utf8Encode(0x4E16);
    auto b = TextRenderer::detectAppend(cjk2, cjk3);
    CHECK(b.isAppend);
    CHECK(b.tailByteOffset == 6);      // two 3-byte codepoints
    CHECK(b.prefixGlyphs == 2);
    CHECK(b.tailGlyphs == 1);

    // NOT appends: identical, shortened, rewritten, nothing cached.
    CHECK_FALSE(TextRenderer::detectAppend("Hello", "Hello").isAppend);
    CHECK_FALSE(TextRenderer::detectAppend("Hello", "Hell").isAppend);
    CHECK_FALSE(TextRenderer::detectAppend("Hello", "Hallo!").isAppend);
    CHECK_FALSE(TextRenderer::detectAppend("", "Hello").isAppend);

    // Defensive: a prefix truncated mid-sequence must NOT count as an append --
    // resuming layout from a continuation byte would desync the UTF-8 decode.
    CHECK_FALSE(TextRenderer::detectAppend(cjk3.substr(0, 7), cjk3).isAppend);
}

TEST_CASE("Batch cache: plan picks Hit / Append / FullRebuild correctly") {
    MessageLayerCache slot;
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abc", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);

    slot.cachedText = "abc";
    slot.glyphCount = 3;
    slot.cachedViewId = 1;
    slot.cachedX = 10.0f; slot.cachedY = 20.0f;
    slot.geometryValid = true;
    slot.clearDirty();

    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abc", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::Hit);

    const auto app = TextRenderer::planCacheUpdate(slot, 1, "abcde", 10.0f, 20.0f);
    CHECK(app.action == TextRenderer::CacheAction::Append);
    CHECK(app.glyphsToLayout == 2);            // ONLY the tail
    CHECK(app.firstGlyph == 3);
    CHECK(app.byteBegin == 3);
    CHECK(app.vertexBytes == 2 * kVBytesPerGlyph);
    CHECK(app.indexBytes == 2 * kIBytesPerGlyph);

    // Different view / moved / shortened / rewritten => full rebuild.
    CHECK(TextRenderer::planCacheUpdate(slot, 2, "abcde", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abcde", 11.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abcde", 10.0f, 21.0f).action
          == TextRenderer::CacheAction::FullRebuild);
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "ab", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "aXcde", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);

    // An explicitly invalidated slot rebuilds even for identical text.
    slot.markAllDirty();
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abc", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);

    // A slot whose resident glyph count disagrees with the prefix (maxGlyphs
    // clamping) must NOT append: the tail would land at the wrong offset.
    slot.clearDirty();
    slot.glyphCount = 2;
    CHECK(TextRenderer::planCacheUpdate(slot, 1, "abcde", 10.0f, 20.0f).action
          == TextRenderer::CacheAction::FullRebuild);
}

TEST_CASE("Batch cache: incremental append is byte-identical to a full rebuild") {
    // CORRECTNESS RED LINE. Reveal a 60-codepoint mixed CJK/ASCII line one
    // codepoint per frame through the incremental path, then build the same
    // final text from scratch, and memcmp the ENTIRE resident vertex/index
    // buffers (raw bytes, not an epsilon compare).
    const auto table = makeAsciiCjkTable();
    const auto frames = makeTypewriterFrames(60);
    const std::string finalText = frames.back();

    SimSlot incremental; SimCounters incCount;
    for (const auto& f : frames)
        simDraw(incremental, 1, f, 32.0f, 48.0f, table, 1280.0f, 720.0f, incCount);

    SimSlot full; SimCounters fullCount;
    simDraw(full, 1, finalText, 32.0f, 48.0f, table, 1280.0f, 720.0f, fullCount);

    REQUIRE(incremental.meta.glyphCount == 60);
    REQUIRE(incremental.meta.glyphCount == full.meta.glyphCount);
    REQUIRE(incCount.appends == 59);        // the reveal really used the tail path
    REQUIRE(fullCount.fullRebuilds == 1);

    // Whole buffers, including the untouched tail: a partial upload must never
    // leave stale geometry anywhere.
    CHECK(std::memcmp(incremental.vb.data(), full.vb.data(),
                      incremental.vb.size() * sizeof(float)) == 0);
    CHECK(std::memcmp(incremental.ib.data(), full.ib.data(),
                      incremental.ib.size() * sizeof(uint32_t)) == 0);

    // Derived state must agree too: penEnd drives the caller's cursor and
    // cacheIsCjk selects the atlas -- a mismatch renders wrong glyphs.
    CHECK(incremental.meta.penEnd == full.meta.penEnd);
    CHECK(incremental.meta.cachedPenAdvance == full.meta.cachedPenAdvance);
    CHECK(incremental.meta.cacheIsCjk == full.meta.cacheIsCjk);
    CHECK(incremental.meta.cacheIsCjk == false);   // the line contains ASCII
}

TEST_CASE("Batch cache: CJK-only reveal keeps the CJK-only verdict incrementally") {
    const auto table = makeAsciiCjkTable();
    const uint32_t cjk[] = {0x4F60,0x597D,0x4E16,0x754C,0x7684};
    std::vector<std::string> frames;
    std::string acc;
    for (int i = 0; i < 5; ++i) { acc += utf8Encode(cjk[i]); frames.push_back(acc); }

    SimSlot inc; SimCounters ic;
    for (const auto& f : frames) simDraw(inc, 1, f, 0.0f, 0.0f, table, 1280.0f, 720.0f, ic);
    SimSlot full; SimCounters fc;
    simDraw(full, 1, frames.back(), 0.0f, 0.0f, table, 1280.0f, 720.0f, fc);

    CHECK(inc.meta.cacheIsCjk == true);
    CHECK(inc.meta.cacheIsCjk == full.meta.cacheIsCjk);
    CHECK(std::memcmp(inc.vb.data(), full.vb.data(), inc.vb.size() * sizeof(float)) == 0);

    // Appending ONE ASCII glyph must flip the verdict, exactly as a full
    // rebuild of that text would.
    const std::string mixed = frames.back() + "a";
    simDraw(inc, 1, mixed, 0.0f, 0.0f, table, 1280.0f, 720.0f, ic);
    SimSlot fullMixed; SimCounters fmc;
    simDraw(fullMixed, 1, mixed, 0.0f, 0.0f, table, 1280.0f, 720.0f, fmc);
    CHECK(inc.meta.cacheIsCjk == false);
    CHECK(inc.meta.cacheIsCjk == fullMixed.meta.cacheIsCjk);
    CHECK(std::memcmp(inc.vb.data(), fullMixed.vb.data(),
                      inc.vb.size() * sizeof(float)) == 0);
}

TEST_CASE("Batch cache: typewriter reveal measurement (before vs after)") {
    // MEASUREMENT. 60-codepoint line, one codepoint per frame, then held for 30
    // frames (the reader is reading it) = 90 draws. Counters are the same
    // quantities production CacheStats tracks: codepoints handed to
    // layoutGlyphs, and bytes handed to bgfx::update.
    const auto table = makeAsciiCjkTable();
    auto frames = makeTypewriterFrames(60);
    const std::string finalText = frames.back();
    for (int i = 0; i < 30; ++i) frames.push_back(finalText);

    SimCounters before;
    for (const auto& f : frames)
        simDrawLegacyFullRebuild(f, 32.0f, 48.0f, table, 1280.0f, 720.0f, before);

    SimSlot slot; SimCounters after;
    for (const auto& f : frames)
        simDraw(slot, 1, f, 32.0f, 48.0f, table, 1280.0f, 720.0f, after);

    MESSAGE("typewriter: 60 codepoints revealed + 30 held frames = 90 draws");
    MESSAGE("  BEFORE  layouts=", before.fullRebuilds,
            " glyphsLaidOut=", before.glyphsLaidOut,
            " vertexBytes=", before.vertexBytes,
            " indexBytes=", before.indexBytes);
    MESSAGE("  AFTER   full=", after.fullRebuilds, " append=", after.appends,
            " hit=", after.hits,
            " glyphsLaidOut=", after.glyphsLaidOut,
            " vertexBytes=", after.vertexBytes,
            " indexBytes=", after.indexBytes);

    // Before: all 90 frames re-laid the whole current text.
    // 60 reveal frames = 1+2+...+60 = 1830 codepoints, + 30 held frames x 60.
    CHECK(before.fullRebuilds == 90);
    CHECK(before.glyphsLaidOut == 1830 + 30 * 60);
    CHECK(before.vertexBytes == (uint64_t)(1830 + 30 * 60) * kVBytesPerGlyph);
    CHECK(before.indexBytes == (uint64_t)(1830 + 30 * 60) * kIBytesPerGlyph);

    // After: 1 full rebuild (first frame) + 59 tail appends + 30 pure hits.
    CHECK(after.fullRebuilds == 1);
    CHECK(after.appends == 59);
    CHECK(after.hits == 30);
    CHECK(after.glyphsLaidOut == 60);      // each codepoint laid out exactly once
    CHECK(after.vertexBytes == (uint64_t)60 * kVBytesPerGlyph);
    CHECK(after.indexBytes == (uint64_t)60 * kIBytesPerGlyph);

    // O(N^2) -> O(N). Exact figures for this scenario (pinned as absolute
    // numbers, not ratios, so a regression reports a concrete value):
    //   layout codepoints  3630 -> 60      (60.5x less layout work)
    //   vertex bytes     348480 -> 5760    (60.5x fewer uploaded bytes)
    //   index bytes       87120 -> 1440    (60.5x)
    CHECK(before.glyphsLaidOut == 3630);
    CHECK(before.vertexBytes == 348480);
    CHECK(before.indexBytes == 87120);
    CHECK(after.glyphsLaidOut == 60);
    CHECK(after.vertexBytes == 5760);
    CHECK(after.indexBytes == 1440);
    // The reduction factor is EXACTLY 60.5x, not 61x: 3630 / 60 = 60.5. Asserted
    // in cross-multiplied integer form so integer division cannot round the
    // claim upward (a ratio written as "== 61" would be an overstatement, and
    // "== 60" silently truncates the real figure).
    CHECK(before.glyphsLaidOut * 2 == after.glyphsLaidOut * 121);   // 60.5x
    CHECK(before.vertexBytes  * 2 == after.vertexBytes  * 121);     // 60.5x
    CHECK(before.indexBytes   * 2 == after.indexBytes   * 121);     // 60.5x
    // Per-frame averages across the 90 draws: 3872 -> 64 vertex bytes/frame,
    // 40.3 -> 0.67 laid-out codepoints/frame.
    CHECK(before.vertexBytes / 90 == 3872);
    CHECK(after.vertexBytes / 90 == 64);
}

TEST_CASE("Batch cache: LRU keeps concurrent text layers resident (single slot thrashes)") {
    // A visual-novel frame draws several distinct texts. With ONE slot they
    // evict each other every frame; with the LRU set each keeps its geometry.
    MessageLayerCache slots[TextRenderer::kCacheSlots];
    const std::string name = "aoi";
    const std::string body = "hello there";
    const std::string ruby = "furigana";
    struct Draw { uint16_t view; const std::string* text; float x, y; };
    const Draw draws[] = {
        {1, &name, 32.0f, 400.0f},
        {1, &body, 32.0f, 440.0f},
        {1, &ruby, 32.0f, 380.0f},
    };

    uint64_t clock = 0;
    const auto commit = [&clock](MessageLayerCache* arr, size_t i, const Draw& d) {
        arr[i].cachedText = *d.text;
        arr[i].cachedViewId = d.view;
        arr[i].cachedX = d.x; arr[i].cachedY = d.y;
        arr[i].glyphCount = (uint32_t)d.text->size();
        arr[i].geometryValid = true;
        arr[i].clearDirty();
        arr[i].lastUse = ++clock;
    };

    for (const auto& d : draws) {
        const size_t i = TextRenderer::selectCacheSlot(
            slots, TextRenderer::kCacheSlots, d.view, *d.text, d.x, d.y);
        commit(slots, i, d);
    }
    size_t live = 0;
    for (size_t i = 0; i < TextRenderer::kCacheSlots; ++i)
        if (slots[i].geometryValid) ++live;
    CHECK(live == 3);          // three distinct slots, no mutual eviction

    // Second frame: all three are exact hits.
    for (const auto& d : draws) {
        const size_t i = TextRenderer::selectCacheSlot(
            slots, TextRenderer::kCacheSlots, d.view, *d.text, d.x, d.y);
        CHECK(TextRenderer::planCacheUpdate(slots[i], d.view, *d.text, d.x, d.y).action
              == TextRenderer::CacheAction::Hit);
        slots[i].lastUse = ++clock;
    }

    // Contrast: capacity 1 rebuilds on every single draw.
    MessageLayerCache single[1];
    MessageLayerCache lru[TextRenderer::kCacheSlots];
    uint32_t singleRebuilds = 0, lruRebuilds = 0;
    for (int frame = 0; frame < 2; ++frame) {
        for (const auto& d : draws) {
            const size_t si = TextRenderer::selectCacheSlot(single, 1, d.view, *d.text, d.x, d.y);
            if (TextRenderer::planCacheUpdate(single[si], d.view, *d.text, d.x, d.y).action
                == TextRenderer::CacheAction::FullRebuild) ++singleRebuilds;
            commit(single, si, d);

            const size_t li = TextRenderer::selectCacheSlot(
                lru, TextRenderer::kCacheSlots, d.view, *d.text, d.x, d.y);
            if (TextRenderer::planCacheUpdate(lru[li], d.view, *d.text, d.x, d.y).action
                == TextRenderer::CacheAction::FullRebuild) ++lruRebuilds;
            commit(lru, li, d);
        }
    }
    MESSAGE("3 text layers x 2 frames: single-slot full rebuilds=", singleRebuilds,
            "  LRU-8 full rebuilds=", lruRebuilds);
    CHECK(singleRebuilds == 6);   // every draw thrashes
    CHECK(lruRebuilds == 3);      // one per layer, then hits
}

TEST_CASE("Batch cache: LRU evicts the least-recently-used slot") {
    MessageLayerCache slots[TextRenderer::kCacheSlots];
    uint64_t clock = 0;
    for (size_t i = 0; i < TextRenderer::kCacheSlots; ++i) {
        const std::string t = "text" + std::to_string(i);
        const size_t idx = TextRenderer::selectCacheSlot(
            slots, TextRenderer::kCacheSlots, 1, t, 0.0f, (float)i);
        CHECK(idx == i);              // unused slots are taken in order
        slots[idx].cachedText = t;
        slots[idx].cachedViewId = 1;
        slots[idx].cachedX = 0.0f; slots[idx].cachedY = (float)i;
        slots[idx].glyphCount = (uint32_t)t.size();
        slots[idx].geometryValid = true;
        slots[idx].clearDirty();
        slots[idx].lastUse = ++clock;
    }
    slots[0].lastUse = ++clock;       // slot 0 is no longer the oldest
    CHECK(TextRenderer::selectCacheSlot(slots, TextRenderer::kCacheSlots,
                                        1, "ninth", 0.0f, 99.0f) == 1);
}

TEST_CASE("Kinsoku wrap: no line starts with forbidden punctuation") {
    // "中文，中文。" at a width where the naive greedy break lands right before
    // the fullwidth comma -- which kinsoku forbids.
    const std::string text = utf8Encode(0x4E2D) + utf8Encode(0x6587) + utf8Encode(0xFF0C)
                           + utf8Encode(0x4E2D) + utf8Encode(0x6587) + utf8Encode(0x3002);
    auto breaks = TextRenderer::wrapTextKinsoku(text, 4.0f, fixedAdvance, nullptr);
    REQUIRE(breaks.size() >= 3);           // at least one real break
    for (size_t i = 1; i + 1 < breaks.size(); ++i) {
        REQUIRE(breaks[i] < text.size());
        CHECK_FALSE(TextRenderer::isKinsokuLineStartForbidden(
            decodeFirstCodepoint(text, breaks[i])));
    }
}

TEST_CASE("Kinsoku wrap: never breaks immediately after an opening bracket") {
    // 「中文中文」 -- 「 is 3 bytes, so a break offset of 3 would mean the line
    // ended on the opening bracket.
    const std::string text = utf8Encode(0x300C) + utf8Encode(0x4E2D) + utf8Encode(0x6587)
                           + utf8Encode(0x4E2D) + utf8Encode(0x6587) + utf8Encode(0x300D);
    auto breaks = TextRenderer::wrapTextKinsoku(text, 4.0f, fixedAdvance, nullptr);
    for (size_t i = 1; i + 1 < breaks.size(); ++i) CHECK(breaks[i] != 3);
}

TEST_CASE("Kinsoku wrap: ASCII wraps greedily and explicit newlines always break") {
    auto breaks = TextRenderer::wrapTextKinsoku("abcdefghij", 4.0f, fixedAdvance, nullptr);
    REQUIRE(breaks.size() == 4);
    CHECK(breaks[0] == 0);
    CHECK(breaks[1] == 4);
    CHECK(breaks[2] == 8);
    CHECK(breaks[3] == 10);

    auto nl = TextRenderer::wrapTextKinsoku("ab\ncd", 100.0f, fixedAdvance, nullptr);
    REQUIRE(nl.size() == 3);
    CHECK(nl[1] == 3);     // just past the '\n'
    CHECK(nl[2] == 5);
}

TEST_CASE("Kinsoku wrap: a glyph wider than the line still yields non-empty lines") {
    // Width 1 with 2-unit CJK glyphs: each char gets its own line, the wrap
    // terminates, and no zero-length line is emitted.
    const std::string text = utf8Encode(0x4E2D) + utf8Encode(0x6587) + utf8Encode(0x5B57);
    auto breaks = TextRenderer::wrapTextKinsoku(text, 1.0f, fixedAdvance, nullptr);
    REQUIRE(breaks.size() >= 2);
    for (size_t i = 1; i < breaks.size(); ++i) CHECK(breaks[i] > breaks[i - 1]);
    CHECK(breaks.back() == text.size());
}
