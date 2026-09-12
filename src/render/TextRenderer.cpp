#include "TextRenderer.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include "api/IRenderDevice.h"
#include "di/BackendRegistry.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

namespace Caesura {

namespace {

bgfx::ProgramHandle toBgfx(RenderProgramHandle handle) {
    if (!handle.isValid()) return BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle result;
    result.idx = handle.idx;
    return result;
}

} // namespace

// ===========================================================================
// UTF-8 Decode Helper — consume multi-byte sequences as single codepoints
// ===========================================================================

static int utf8_char_len(uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xC0) return 1;  // continuation byte — treat as '?'
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

static uint32_t utf8_codepoint(const uint8_t* data, int len) {
    if (len <= 0 || len > 4) return 0xfffd;
    if (len == 1) return data[0] < 0x80 ? data[0] : 0xfffd;
    if (utf8_char_len(data[0]) != len || data[0] < 0xc2 || data[0] > 0xf4) return 0xfffd;
    for (int i = 1; i < len; ++i) if ((data[i] & 0xc0) != 0x80) return 0xfffd;
    uint32_t cp = 0;
    if (len == 2) cp = ((data[0] & 0x1F) << 6) | (data[1] & 0x3F);
    else if (len == 3) cp = ((data[0] & 0x0F) << 12) | ((data[1] & 0x3F) << 6) | (data[2] & 0x3F);
    else cp = ((data[0] & 0x07) << 18) | ((data[1] & 0x3F) << 12) | ((data[2] & 0x3F) << 6) | (data[3] & 0x3F);
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)
        || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
    return cp;
}


// ===========================================================================
// Embedded 8x16 bitmap font —ASCII 32 (space) through 126 (~)
// Each glyph: 16 bytes, MSB = leftmost pixel, 1 = lit pixel
// Sourced from public-domain VGA ROM font (Linux kbd console font).
// ===========================================================================

static const uint8_t kFont8x16[95][16] = {
    // 32 ' '
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 33 '!'
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 34 '"'
    {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 35 '#'
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    // 36 '$'
    {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    // 37 '%'
    {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    // 38 '&'
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 39 '''
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 40 '('
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    // 41 ')'
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    // 42 '*'
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    // 44 ','
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 46 '.'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 47 '/'
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    // 48 '0'
    {0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00},
    // 49 '1'
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    // 50 '2'
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 51 '3'
    {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 52 '4'
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    // 53 '5'
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 54 '6'
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 55 '7'
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    // 56 '8'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 57 '9'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    // 58 ':'
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    // 59 ';'
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    // 60 '<'
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    // 61 '='
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 62 '>'
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    // 63 '?'
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    // 64 '@'
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    // 65 'A'
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 66 'B'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    // 67 'C'
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    // 68 'D'
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    // 69 'E'
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 70 'F'
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 71 'G'
    {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    // 72 'H'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 73 'I'
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 74 'J'
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    // 75 'K'
    {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 76 'L'
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    // 77 'M'
    {0x00,0x00,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00},
    // 78 'N'
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    // 79 'O'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 80 'P'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 81 'Q'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    // 82 'R'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 83 'S'
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 84 'T'
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 85 'U'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 86 'V'
    {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    // 87 'W'
    {0x00,0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0x00,0x00,0x00,0x00},
    // 88 'X'
    {0x00,0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00,0x00},
    // 89 'Y'
    {0x00,0x00,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 90 'Z'
    {0x00,0x00,0xFF,0xC3,0x86,0x0C,0x18,0x30,0x60,0xC1,0xC3,0xFF,0x00,0x00,0x00,0x00},
    // 91 '['
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    // 92 '\'
    {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    // 93 ']'
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    // 94 '^'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 95 '_'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    // 96 '`'
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 97 'a'
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 98 'b'
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    // 99 'c'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 100 'd'
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 101 'e'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 102 'f'
    {0x00,0x00,0x38,0x6C,0x64,0x60,0xF0,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 103 'g'
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    // 104 'h'
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 105 'i'
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 106 'j'
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    // 107 'k'
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    // 108 'l'
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    // 109 'm'
    {0x00,0x00,0x00,0x00,0x00,0xE6,0xFF,0xDB,0xDB,0xDB,0xDB,0xDB,0x00,0x00,0x00,0x00},
    // 110 'n'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    // 111 'o'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 112 'p'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    // 113 'q'
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    // 114 'r'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    // 115 's'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    // 116 't'
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    // 117 'u'
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    // 118 'v'
    {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    // 119 'w'
    {0x00,0x00,0x00,0x00,0x00,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x00,0x00,0x00,0x00},
    // 120 'x'
    {0x00,0x00,0x00,0x00,0x00,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00},
    // 121 'y'
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    // 122 'z'
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    // 123 '{'
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    // 124 '|'
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    // 125 '}'
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    // 126 '~'
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// ===========================================================================
// Lifecycle
// ===========================================================================

TextRenderer::~TextRenderer() {
    shutdown();
}

TextRenderer::GlyphAtlas::~GlyphAtlas() {
    if (ftFace) {
        FT_Done_Face(ftFace);
        ftFace = nullptr;
    }
    if (ftLib) {
        FT_Done_FreeType(ftLib);
        ftLib = nullptr;
    }
}

bool TextRenderer::init(IRenderDevice* device, bool activateDefault) {
    if (m_initialized) return true;
    if (!device) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextRenderer] Null device pointer.");
        return false;
    }

    // Borrow shared resources from IRenderDevice
    m_textProgram = toBgfx(device->getModulatedTextureProgram());
    if (!bgfx::isValid(m_textProgram)) {
        m_textProgram = toBgfx(device->getFallbackProgram());
        DEBUG_INFO(SubSys::Render, ErrCode::Ok,
            "[TextRenderer] Modulated texture program unavailable; preserving unmodulated fallback text (color/alpha unsupported on this backend).");
    }
    if (!bgfx::isValid(m_textProgram)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextRenderer] Texture program not ready. "
                  "Ensure device::init() runs first.");
        return false;
    }

    // Use the same PosTex layout: Position(2F) + TexCoord0(2F)
    m_posTexLayout
        .begin()
        .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();

    m_texSampler = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
    if (!bgfx::isValid(m_texSampler)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextRenderer] Uniform creation failed.");
        return false;
    }

    m_u_color = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(m_u_color)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[TextRenderer] Color uniform creation failed.");
        return false;
    }

    if (activateDefault && !loadFontAtlas(FontId::Small)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] Font atlas creation failed.");
        return false;
    }

    m_cursor.lineHeight = (float)m_fontGlyphH;
    m_screenWidth  = device->getBackbufferWidth();
    m_screenHeight = device->getBackbufferHeight();
    m_savedDevice = device;
    BackendRegistry::instance().registerDeviceLostListener(this);
    m_initialized = true;
    printf("[TextRenderer] Initialized. Font: %dx%d, atlas: %d cols.\n",
           m_fontGlyphW, m_fontGlyphH, m_atlasCols);
    return true;
}

void TextRenderer::setScreenSize(int width, int height) {
    if (width <= 0 || height <= 0 || (width == m_screenWidth && height == m_screenHeight)) return;
    m_screenWidth = width;
    m_screenHeight = height;
    // Resident vertices contain NDC, so the same string/position is not a hit
    // after a surface change. Keep buffers, invalidate every LRU geometry key.
    invalidateCache();
}

bool TextRenderer::uploadPendingGlyphs() {
    if (!m_ttf || m_ttf->pendingUpload().w == 0) return true;
    if (!bgfx::isValid(m_fontTexture)) return false;
    const auto dirty = m_ttf->pendingUpload();
    try {
        // Tight, owned rows: passing an atlas-offset pointer with a shorter
        // copy would omit the source row pitch. This patch includes the
        // white/transparent guard texels and only stable or newly added cells.
        const size_t rowBytes = size_t(dirty.w) * 4;
        std::vector<uint8_t> patch(rowBytes * dirty.h);
        for (int row = 0; row < dirty.h; ++row) {
            const auto offset = (size_t(dirty.y + row) * m_ttf->width() + dirty.x) * 4;
            std::memcpy(patch.data() + size_t(row) * rowBytes,
                        m_ttf->pixels().data() + offset, rowBytes);
        }
        const auto* owned = bgfx::copy(patch.data(), static_cast<uint32_t>(patch.size()));
        if (!owned) return false;
        bgfx::updateTexture2D(m_fontTexture, 0, 0,
            static_cast<uint16_t>(dirty.x), static_cast<uint16_t>(dirty.y),
            static_cast<uint16_t>(dirty.w), static_cast<uint16_t>(dirty.h), owned,
            static_cast<uint16_t>(rowBytes));
        m_ttf->acknowledgeUpload();
        return true;
    } catch (...) { return false; }
}

bool TextRenderer::prepareTextGlyphs(const std::string& text, size_t byteBegin) {
    if (!m_ttf) return true;
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    for (size_t i = std::min(byteBegin, text.size()); i < text.size();) {
        const int length = static_cast<int>(std::min(size_t(utf8_char_len(data[i])), text.size() - i));
        const uint32_t cp = utf8_codepoint(data + i, length);
        i += static_cast<size_t>(length);
        if (cp == '\n' || cp == '\r') continue;
        const auto status = m_ttf->prepareGlyph(cp);
        if (status == GlyphAtlas::PrepareStatus::Missing && !m_reportedMissingGlyph) {
            DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                "[TextRenderer] Font lacks U+%04X; using this font atlas's replacement glyph.", cp);
            m_reportedMissingGlyph = true;
        } else if (status == GlyphAtlas::PrepareStatus::Full && !m_reportedAtlasFull) {
            DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                "[TextRenderer] Bounded glyph atlas exhausted at U+%04X; earlier glyphs remain stable, using replacement.", cp);
            m_reportedAtlasFull = true;
        } else if (status == GlyphAtlas::PrepareStatus::Error) {
            if (!m_reportedAtlasUploadFailure) DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                "[TextRenderer] Glyph preparation failed; text submission skipped.");
            m_reportedAtlasUploadFailure = true;
            return false;
        }
    }
    if (uploadPendingGlyphs()) return true;
    if (!m_reportedAtlasUploadFailure) DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
        "[TextRenderer] Glyph atlas upload failed; dirty pixels retained, text submission skipped.");
    m_reportedAtlasUploadFailure = true;
    return false;
}

float TextRenderer::measureAdvance(const std::string& text, AdvanceFn advance, void* userData) {
    if (!advance) return 0;
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    float width = 0;
    for (size_t i = 0; i < text.size();) {
        const int length = static_cast<int>(std::min(size_t(utf8_char_len(data[i])), text.size() - i));
        const uint32_t cp = utf8_codepoint(data + i, length);
        i += static_cast<size_t>(length);
        if (cp != '\n' && cp != '\r') width += advance(cp, userData);
    }
    return width;
}

void TextRenderer::shutdown() {
    BackendRegistry::instance().unregisterDeviceLostListener(this);
    clearFontState();
    if (bgfx::isValid(m_strikeTexture)) {
        bgfx::destroy(m_strikeTexture);
        m_strikeTexture = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_texSampler)) {
        bgfx::destroy(m_texSampler);
        m_texSampler = BGFX_INVALID_HANDLE;
    }
    m_ttf.reset();

    // Track 2: batch cache and CJK atlas cleanup (every LRU slot owns buffers)
    for (size_t i = 0; i < kCacheSlots; ++i) {
        MessageLayerCache& s = m_cacheSlots[i];
        if (bgfx::isValid(s.vb)) { bgfx::destroy(s.vb); s.vb = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(s.ib)) { bgfx::destroy(s.ib); s.ib = BGFX_INVALID_HANDLE; }
        s.invalidateGeometry();
    }
    if (bgfx::isValid(m_u_color))    { bgfx::destroy(m_u_color);   m_u_color   = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(m_cjkAtlas))   { bgfx::destroy(m_cjkAtlas);  m_cjkAtlas  = BGFX_INVALID_HANDLE; }
    m_cjkGlyphs.clear();

    // m_textProgram and m_posTexLayout are borrowed — do NOT destroy
    m_textProgram = BGFX_INVALID_HANDLE;
    m_initialized = false;
}

void TextRenderer::onDeviceLost() {
    // Release GPU resources WITHOUT unregistering: the registry iterates a
    // copy during notify (no erase-UB), and staying registered lets
    // notifyDeviceRestored reach us. shutdown() keeps the unregister for
    // explicit destruction; re-registering here would double-register across
    // repeated loss/restore cycles.
    if (bgfx::isValid(m_fontTexture)) {
        bgfx::destroy(m_fontTexture);
        m_fontTexture = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_strikeTexture)) {
        bgfx::destroy(m_strikeTexture);
        m_strikeTexture = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(m_texSampler)) {
        bgfx::destroy(m_texSampler);
        m_texSampler = BGFX_INVALID_HANDLE;
    }
    // Keep the selected font's CPU bytes/atlas. Device recovery must not
    // reopen a changed path or replace Large/cleared state with another font.
    // Device loss destroys every slot's buffers; the geometry they held is gone
    // too, so no slot may report a hit after restore.
    for (size_t i = 0; i < kCacheSlots; ++i) {
        MessageLayerCache& s = m_cacheSlots[i];
        if (bgfx::isValid(s.vb)) { bgfx::destroy(s.vb); s.vb = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(s.ib)) { bgfx::destroy(s.ib); s.ib = BGFX_INVALID_HANDLE; }
        s.invalidateGeometry();
    }
    if (bgfx::isValid(m_u_color))    { bgfx::destroy(m_u_color);   m_u_color   = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(m_cjkAtlas))   { bgfx::destroy(m_cjkAtlas);  m_cjkAtlas  = BGFX_INVALID_HANDLE; }
    m_cjkGlyphs.clear();
    m_textProgram = BGFX_INVALID_HANDLE;
    m_initialized = false;
}

void TextRenderer::onDeviceRestored() {
    if (m_initialized || !m_savedDevice) return;
    auto font=takeFontForDeviceRecovery();
    if (!init(m_savedDevice,false) || !applyFontState(std::move(font))) {
        clearFontState();
        DEBUG_ERR(SubSys::Render,ErrCode::Render_FontAtlasFailed,"[TextRenderer] Font device recovery failed");
    }
}

// ===========================================================================
// Font atlas
// ===========================================================================

std::unique_ptr<IPreparedFontState> TextRenderer::prepareBitmapFont(const FontRestoreState& state) {
    const FontId id = state.font;
    int glyphW = (id == FontId::Small) ? 8 : 16;
    int glyphH = (id == FontId::Small) ? 16 : 32;

    // Both atlases come from the same immutable 8x16 glyph data. Large is
    // a true 2x nearest-neighbour expansion; no mutable global atlas is used.
    const int scale = id == FontId::Small ? 1 : 2;
    const int atlasW = glyphW * 32;
    const int atlasH = glyphH * 3;
    std::vector<uint8_t> pixels(size_t(atlasW) * atlasH * 4, 255);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0;
    for (int glyph = 0; glyph < 95; ++glyph) {
        const int column = glyph % 32, row = glyph / 32;
        for (int y = 0; y < glyphH; ++y) {
            const uint8_t bits = kFont8x16[glyph][y / scale];
            for (int x = 0; x < glyphW; ++x) {
                if (!(bits & (0x80 >> (x / scale)))) continue;
                const size_t offset = (size_t(row * glyphH + y) * atlasW + column * glyphW + x) * 4;
                pixels[offset] = pixels[offset+1] = pixels[offset+2] = pixels[offset+3] = 255;
            }
        }
    }

    auto prepared = std::make_unique<PreparedFont>();
    prepared->state = state;
    prepared->bitmapPixels = std::move(pixels);
    prepared->atlasW = atlasW; prepared->atlasH = atlasH;
    prepared->glyphW = glyphW; prepared->glyphH = glyphH;
    prepared->atlasCols = 32; prepared->lineHeight = static_cast<float>(glyphH);
    return prepared;
}

bool TextRenderer::loadFontAtlas(FontId id) {
    auto prepared = prepareBitmapFont({true,id,"",id == FontId::Small ? 16.0f : 32.0f});
    return activateFont(*static_cast<PreparedFont*>(prepared.get()));
}

void TextRenderer::setFont(FontId id) {
    if (!m_initialized || (id != FontId::Small && id != FontId::Large && id != FontId::TTF)) return;
    if (id == m_currentFont && m_fontDescription.active) return;
    if (id == FontId::TTF) {
        if (!m_ttfPath.empty()) loadTTF(m_ttfPath.c_str(),m_ttfFontSize);
        return;
    }
    auto prepared = prepareFontState({true,id,"",id == FontId::Small ? 16.0f : 32.0f},nullptr,0);
    if (prepared) {
        auto* value=static_cast<PreparedFont*>(prepared.get());
        value->rememberedTtfPath=m_ttfPath;
        value->rememberedTtfSize=m_ttfFontSize;
        applyFontState(std::move(prepared));
    }
}

// ===========================================================================
// Glyph building
// ===========================================================================

TextRenderer::GlyphQuad TextRenderer::buildGlyph(
    char ch, float penX, float penY, float scaleW, float scaleH)
{
    return buildGlyph((uint32_t)(unsigned char)ch, penX, penY, scaleW, scaleH);
}

TextRenderer::GlyphQuad TextRenderer::buildGlyph(
    uint32_t cp, float penX, float penY, float scaleW, float scaleH)
{
    // Every TTF glyph, including a missing/full-atlas replacement, uses this
    // atlas's metrics and UV space. Never fall through to bitmap coordinates.
    if (m_ttf) {
        const auto& gm = m_ttf->resolve(cp);
        float gw = (float)gm.w * scaleW;
        float gh = (float)gm.h * scaleH;
        float atlasW = (float)m_ttf->atlasW;
        float atlasH = (float)m_ttf->atlasH;
        GlyphQuad q;
        q.x = penX + gm.offsetX * scaleW;
        q.y = penY - gm.offsetY * scaleH + m_ttf->ascent * scaleH;
        q.w = gw;
        q.h = gh;
        q.u0 = (float)gm.x / atlasW;
        q.v0 = (float)gm.y / atlasH;
        q.u1 = (float)(gm.x + gm.w) / atlasW;
        q.v1 = (float)(gm.y + gm.h) / atlasH;
        q.advance = (float)gm.advance * scaleW;
        if (gm.w > 0 && gm.h > 0) {
            q.paddingX = 0.5f * scaleW;
            q.paddingY = 0.5f * scaleH;
        }
        return q;
    }

    // Bitmap fallback
    int idx;
    if (cp >= 32 && cp <= 126)
        idx = (int)cp - 32;
    else
        idx = 31; // '?' from the actually bound bitmap atlas.

    int col = idx % m_atlasCols;
    int row = idx / m_atlasCols;

    float gw = (float)m_fontGlyphW * scaleW;
    float gh = (float)m_fontGlyphH * scaleH;

    float atlasW = (float)(m_fontGlyphW * m_atlasCols);
    float atlasH = (float)(m_fontGlyphH * 3);

    GlyphQuad q;
    q.x  = penX;
    q.y  = penY;
    q.w  = gw;
    q.h  = gh;
    q.u0 = ((float)(col * m_fontGlyphW))     / atlasW;
    q.v0 = ((float)(row * m_fontGlyphH))     / atlasH;
    q.u1 = ((float)((col+1) * m_fontGlyphW)) / atlasW;
    q.v1 = ((float)((row+1) * m_fontGlyphH)) / atlasH;
    q.advance = gw;
    return q;
}

// ===========================================================================
// Quad submission
// ===========================================================================

TextRenderer::NDCQuad TextRenderer::glyphQuadToNDC(
    float x, float y, float w, float h, float shear,
    float screenW, float screenH, float paddingX, float paddingY)
{
    // Extend the same affine shear through the transparent border. This keeps
    // every original ink point fixed, including synthetic italic/strike bounds.
    const float shearExtension = h > 0 ? shear * paddingY / h : 0.0f;
    x -= paddingX + shearExtension;
    y -= paddingY;
    w += 2 * paddingX;
    h += 2 * paddingY;
    shear += 2 * shearExtension;
    // Pixel coords -> NDC (passthrough shader bypasses u_viewProj).
    // Italic shear: the top edge moves right by `shear` px; the bottom
    // edge stays fixed (a true slant, not a translation).
    const float topX = x + shear;
    const float bottomX = x;
    const float invW = 2.0f / screenW;
    const float invH = 2.0f / screenH;
    NDCQuad v;
    v.x0 = topX * invW - 1.0f;       v.y0 = 1.0f - y * invH;              // top-left
    v.x1 = (topX + w) * invW - 1.0f; v.y1 = 1.0f - y * invH;              // top-right
    v.x2 = (bottomX + w) * invW - 1.0f; v.y2 = 1.0f - (y + h) * invH;     // bottom-right
    v.x3 = bottomX * invW - 1.0f;    v.y3 = 1.0f - (y + h) * invH;        // bottom-left
    return v;
}

void TextRenderer::submitGlyphQuads(uint16_t viewId, const GlyphQuad* quads,
                                     int count, TextColor color,
                                     float scaleW, float scaleH)
{
    if (count <= 0 || !bgfx::isValid(m_textProgram)) return;

    // Ortho projection
    float ortho[16];
    const bgfx::Caps* caps = bgfx::getCaps();
    bx::mtxOrtho(ortho, 0.0f, 1280.0f, 720.0f, 0.0f,
                 -1.0f, 1.0f, 0.0f, caps ? caps->homogeneousDepth : false,
                 bx::Handedness::Left);
    bgfx::setViewTransform(viewId, nullptr, ortho);

    struct PosTexVertex { float x, y, u, v; };

    int quadCount = count;
    int vertCount = quadCount * 4;
    int idxCount  = quadCount * 6;

    bgfx::TransientVertexBuffer tvb;
    if (bgfx::getAvailTransientVertexBuffer((uint32_t)vertCount,
            m_posTexLayout) < (uint32_t)vertCount) return;
    bgfx::allocTransientVertexBuffer(&tvb, (uint32_t)vertCount, m_posTexLayout);
    auto* vtx = (PosTexVertex*)tvb.data;

    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer((uint32_t)idxCount) < (uint32_t)idxCount) return;
    bgfx::allocTransientIndexBuffer(&tib, (uint32_t)idxCount);
    auto* idx = (uint16_t*)tib.data;

    float sw = (float)m_screenWidth;
    float sh = (float)m_screenHeight;
    for (int i = 0; i < quadCount; ++i) {
        const GlyphQuad& q = quads[i];
        const NDCQuad v = glyphQuadToNDC(q.x, q.y, q.w, q.h, q.shear, sw, sh, q.paddingX, q.paddingY);
        const float du = q.w > 0 ? (q.u1 - q.u0) * q.paddingX / q.w : 0;
        const float dv = q.h > 0 ? (q.v1 - q.v0) * q.paddingY / q.h : 0;
        int vi = i * 4;
        vtx[vi+0] = { v.x0, v.y0, q.u0-du, q.v0-dv };
        vtx[vi+1] = { v.x1, v.y1, q.u1+du, q.v0-dv };
        vtx[vi+2] = { v.x2, v.y2, q.u1+du, q.v1+dv };
        vtx[vi+3] = { v.x3, v.y3, q.u0-du, q.v1+dv };

        int ii = i * 6;
        uint16_t base = (uint16_t)vi;
        idx[ii+0] = base + 0; idx[ii+1] = base + 1; idx[ii+2] = base + 2;
        idx[ii+3] = base + 0; idx[ii+4] = base + 2; idx[ii+5] = base + 3;
    }

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, m_texSampler, m_fontTexture);
    const float modulation[4] = {color.r/255.0f,color.g/255.0f,color.b/255.0f,color.a/255.0f};
    bgfx::setUniform(m_u_color, modulation);
    bgfx::setState(state);
    bgfx::submit(viewId, m_textProgram);
}

void TextRenderer::ensureStrikeTexture()
{
    if (bgfx::isValid(m_strikeTexture)) return;
    const uint8_t white[4] = { 255, 255, 255, 255 };
    m_strikeTexture = bgfx::createTexture2D(
        1, 1, false, 1, bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bgfx::copy(white, 4));
}

void TextRenderer::submitStrikeBars(uint16_t viewId, const GlyphQuad* bars,
                                     int count, TextColor color)
{
    if (count <= 0 || !bgfx::isValid(m_textProgram)) return;
    ensureStrikeTexture();
    if (!bgfx::isValid(m_strikeTexture)) return;

    float ortho[16];
    const bgfx::Caps* caps = bgfx::getCaps();
    bx::mtxOrtho(ortho, 0.0f, 1280.0f, 720.0f, 0.0f,
                 -1.0f, 1.0f, 0.0f, caps ? caps->homogeneousDepth : false,
                 bx::Handedness::Left);
    bgfx::setViewTransform(viewId, nullptr, ortho);

    struct PosTexVertex { float x, y, u, v; };

    int vertCount = count * 4;
    int idxCount  = count * 6;

    bgfx::TransientVertexBuffer tvb;
    if (bgfx::getAvailTransientVertexBuffer((uint32_t)vertCount,
            m_posTexLayout) < (uint32_t)vertCount) return;
    bgfx::allocTransientVertexBuffer(&tvb, (uint32_t)vertCount, m_posTexLayout);
    auto* vtx = (PosTexVertex*)tvb.data;

    bgfx::TransientIndexBuffer tib;
    if (bgfx::getAvailTransientIndexBuffer((uint32_t)idxCount) < (uint32_t)idxCount) return;
    bgfx::allocTransientIndexBuffer(&tib, (uint32_t)idxCount);
    auto* idx = (uint16_t*)tib.data;

    float sw = (float)m_screenWidth;
    float sh = (float)m_screenHeight;
    for (int i = 0; i < count; ++i) {
        // Bars are axis-aligned (no shear); reuse the pure NDC math.
        const NDCQuad v = glyphQuadToNDC(
            bars[i].x, bars[i].y, bars[i].w, bars[i].h, 0.0f, sw, sh);
        int vi = i * 4;
        vtx[vi+0] = { v.x0, v.y0, 0, 0 };
        vtx[vi+1] = { v.x1, v.y1, 1, 0 };
        vtx[vi+2] = { v.x2, v.y2, 1, 1 };
        vtx[vi+3] = { v.x3, v.y3, 0, 1 };

        int ii = i * 6;
        uint16_t base = (uint16_t)vi;
        idx[ii+0] = base + 0; idx[ii+1] = base + 1; idx[ii+2] = base + 2;
        idx[ii+3] = base + 0; idx[ii+4] = base + 2; idx[ii+5] = base + 3;
    }

    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setTexture(0, m_texSampler, m_strikeTexture);
    const float modulation[4] = {color.r/255.0f,color.g/255.0f,color.b/255.0f,color.a/255.0f};
    bgfx::setUniform(m_u_color, modulation);
    bgfx::setState(state);
    bgfx::submit(viewId, m_textProgram);
}

// ===========================================================================
// Public API
// ===========================================================================

void TextRenderer::renderText(uint16_t viewId, const std::string& text,
                               float x, float y, TextColor color,
                               float scale, bool bold, bool italic,
                               bool strike)
{
    if (text.empty() || !m_initialized || !m_fontDescription.active) return;
    if (!prepareTextGlyphs(text)) return;

    // Build glyph quads (scale != 1 => {size=N} markup; bold => synthetic
    // bold via a second quad pass offset by ~8% of the glyph width;
    // italic => top-edge shear of ~18% of the glyph height; strike =>
    // a solid bar across the glyph's vertical middle).
    std::vector<GlyphQuad> quads;
    std::vector<GlyphQuad> strikeBars;
    quads.reserve(text.size() * (bold ? 2 : 1));
    if (strike) strikeBars.reserve(text.size());

    float penX = x;
    const float boldOffset = std::max(1.0f, 0.08f * m_fontGlyphW * scale);
    const float shear = italic
        ? std::max(1.0f, 0.18f * m_fontGlyphH * scale) : 0.0f;

    const auto* data = (const uint8_t*)text.data();
    int len = (int)text.size();
    for (int i = 0; i < len; ) {
        int clen = utf8_char_len(data[i]);
        if (i + clen > len) clen = len - i;
        uint32_t cp = utf8_codepoint(&data[i], clen);
        i += clen;
        if (cp == '\n') {
            penX = m_cursor.leftMargin;
            m_cursor.y += m_cursor.lineHeight;
            continue;
        }
        GlyphQuad q = buildGlyph(cp, penX, y, scale, scale);
        q.shear = shear;
        quads.push_back(q);
        if (bold) {
            GlyphQuad qb = q;
            qb.x += boldOffset;
            quads.push_back(qb);
        }
        if (strike) {
            // Solid bar across the glyph's vertical middle: same advance
            // width as the glyph, ~10% of its height.
            GlyphQuad bar;
            bar.x = q.x;
            bar.y = q.y + q.h * 0.5f;
            bar.w = q.w;
            bar.h = std::max(1.0f, 0.1f * q.h);
            bar.u0 = 0; bar.v0 = 0; bar.u1 = 1; bar.v1 = 1;
            strikeBars.push_back(bar);
        }
        penX += q.advance;
    }

    submitGlyphQuads(viewId, quads.data(), (int)quads.size(), color, scale, scale);
    if (!strikeBars.empty()) {
        submitStrikeBars(viewId, strikeBars.data(), (int)strikeBars.size(), color);
    }
    m_cursor.x = penX;
}

void TextRenderer::renderRuby(uint16_t viewId, const std::string& text,
                               const std::string& ruby,
                               float x, float y, TextColor color)
{
    if (text.empty() || !m_initialized || !m_fontDescription.active) return;
    if (!prepareTextGlyphs(text) || !prepareTextGlyphs(ruby)) return;

    std::vector<GlyphQuad> quads;
    quads.reserve(text.size() + ruby.size());

    float penX = x;

    // Ruby text (0.5x scale, anchored above)
    float rubyScale = 0.5f;
    const auto advance = [](uint32_t cp, void* owner) {
        return static_cast<TextRenderer*>(owner)->buildGlyph(cp,0,0,1,1).advance;
    };
    float rubyPenX = x + (measureAdvance(text,advance,this)
                          - measureAdvance(ruby,advance,this)*rubyScale) / 2.0f;
    {
        const auto* rdata = (const uint8_t*)ruby.data();
        int rlen = (int)ruby.size();
        for (int i = 0; i < rlen; ) {
            int clen = utf8_char_len(rdata[i]);
            if (i + clen > rlen) clen = rlen - i;
            uint32_t cp = utf8_codepoint(&rdata[i], clen);
            i += clen;
            GlyphQuad q = buildGlyph(cp, rubyPenX, y - m_fontGlyphH * rubyScale - 2.0f,
                                      rubyScale, rubyScale);
            quads.push_back(q);
            rubyPenX += q.advance;
        }
    }

    // Base text (1.0x scale)
    {
        const auto* tdata = (const uint8_t*)text.data();
        int tlen = (int)text.size();
        for (int i = 0; i < tlen; ) {
            int clen = utf8_char_len(tdata[i]);
            if (i + clen > tlen) clen = tlen - i;
            uint32_t cp = utf8_codepoint(&tdata[i], clen);
            i += clen;
            GlyphQuad q = buildGlyph(cp, penX, y, 1.0f, 1.0f);
            quads.push_back(q);
            penX += q.advance;
        }
    }

    submitGlyphQuads(viewId, quads.data(), (int)quads.size(), color, 1.0f, 1.0f);
    m_cursor.x = penX;
}


// ===========================================================================
// TTF loading via FreeType 2
// ===========================================================================

void TextRenderer::newline() {
    m_cursor.x = m_cursor.leftMargin;
    m_cursor.y += m_cursor.lineHeight;
}

void TextRenderer::clearText(uint16_t /*viewId*/) {
    m_cursor.x = m_cursor.leftMargin;
    // No GPU clear —just reset cursor. Layer system handles visibility.
}

// ===========================================================================
//  Track 2: Batch-cached text rendering + CJK static atlas (merged from FontRenderer)
// ===========================================================================

// ---------------------------------------------------------------------------
// Glyph lookup with fallback: TTF atlas > CJK static atlas > built-in bitmap > U+FFFD
// ---------------------------------------------------------------------------

static GlyphMetrics s_emptyGlyph2{0,0,0,0,8,0,0};

GlyphMetrics TextRenderer::getTTFGlyph(uint32_t codepoint) {
    // 1. TTF atlas
    if (m_ttf) {
        // Demand preparation/upload occurs before layout. Missing/full glyphs
        // resolve to a marker in THIS atlas, never foreign bitmap/CJK UVs.
        return m_ttf->resolve(codepoint);
    }

    // 2. CJK static atlas
    if (bgfx::isValid(m_cjkAtlas)) {
        auto cjkIt = m_cjkGlyphs.find(codepoint);
        if (cjkIt != m_cjkGlyphs.end()) {
            GlyphMetrics gm;
            gm.x = cjkIt->second.x; gm.y = cjkIt->second.y;
            gm.w = cjkIt->second.w; gm.h = cjkIt->second.h;
            gm.advance = cjkIt->second.advance;
            gm.offsetX = cjkIt->second.offsetX;
            gm.offsetY = cjkIt->second.offsetY;
            return gm;
        }
    }

    // 3. Built-in bitmap fallback (ASCII 32-126)
    if (codepoint >= 32 && codepoint <= 126) {
        GlyphMetrics gm;
        int idx = (int)(codepoint - 32);
        gm.x = (idx % m_atlasCols) * m_fontGlyphW;
        gm.y = (idx / m_atlasCols) * m_fontGlyphH;
        gm.w = m_fontGlyphW; gm.h = m_fontGlyphH;
        gm.advance = m_fontGlyphW;
        gm.offsetX = 0; gm.offsetY = 0;
        return gm;
    }

    // 4. U+FFFD replacement
    if (codepoint != 0xFFFD) return getTTFGlyph(0xFFFD);
    return s_emptyGlyph2;
}

// ---------------------------------------------------------------------------
// CJK static atlas (pre-generated G8-U5 bitmap)
// ---------------------------------------------------------------------------

bool TextRenderer::loadCjkAtlas(const std::string& atlasPath, const std::string& metaPath) {
    FILE* f = fopen(atlasPath.c_str(), "rb");
    if (!f) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK atlas not found: %s (skipping)", atlasPath.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    if (fread(data.data(), 1, size, f) != (size_t)size) {
        fclose(f);
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK atlas read incomplete: %s", atlasPath.c_str());
        return false;
    }
    fclose(f);

    const uint16_t cjkW = 4096, cjkH = 4096;
    const uint32_t expected = static_cast<uint32_t>(cjkW) * cjkH * 4;  // 64MB
    if (static_cast<size_t>(size) != expected) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK atlas size mismatch: %ld bytes, expected %u (skipping)",
                  size, expected);
        return false;
    }
    m_cjkAtlas = bgfx::createTexture2D(cjkW, cjkH, false, 1,
        bgfx::TextureFormat::RGBA8,
        BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP,
        bgfx::copy(data.data(), expected));
    if (!bgfx::isValid(m_cjkAtlas)) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK atlas texture creation failed");
        return false;
    }

    FILE* mf = fopen(metaPath.c_str(), "rb");
    if (!mf) {
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK metadata not found: %s", metaPath.c_str());
        bgfx::destroy(m_cjkAtlas); m_cjkAtlas = BGFX_INVALID_HANDLE;
        return false;
    }
    uint32_t count = 0;
    if (fread(&count, sizeof(count), 1, mf) != 1) {
        fclose(mf);
        DEBUG_ERR(SubSys::Render, ErrCode::Render_FontAtlasFailed,
                  "[TextRenderer] CJK metadata read failed");
        bgfx::destroy(m_cjkAtlas); m_cjkAtlas = BGFX_INVALID_HANDLE;
        return false;
    }
    m_cjkGlyphs.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t cp; CjkGlyph g;
        if (fread(&cp, sizeof(cp), 1, mf) != 1) break;
        fread(&g.x, sizeof(g.x), 1, mf);
        fread(&g.y, sizeof(g.y), 1, mf);
        fread(&g.w, sizeof(g.w), 1, mf);
        fread(&g.h, sizeof(g.h), 1, mf);
        fread(&g.advance, sizeof(g.advance), 1, mf);
        fread(&g.offsetX, sizeof(g.offsetX), 1, mf);
        fread(&g.offsetY, sizeof(g.offsetY), 1, mf);
        m_cjkGlyphs[cp] = g;
    }
    fclose(mf);

    printf("[TextRenderer] CJK static atlas loaded: %u glyphs (%dx%d)\n", count, cjkW, cjkH);
    return true;
}

// ---------------------------------------------------------------------------
// Batch cache management
// ---------------------------------------------------------------------------

void TextRenderer::invalidateCache() {
    // Every slot's geometry is stale (the atlas moved, or the caller asked for
    // a hard reset): drop the reusable geometry but keep the GPU buffers, which
    // are pooled resources and are rewritten by the next rebuild.
    for (size_t i = 0; i < kCacheSlots; ++i) m_cacheSlots[i].invalidateGeometry();
}

// ---------------------------------------------------------------------------
// Pure dirty-range math (GPU-free) -- extracted so unit tests can pin the
// batch-cache update logic without a GPU.
// ---------------------------------------------------------------------------

uint32_t TextRenderer::countUtf8Glyphs(const uint8_t* data, size_t len) {
    uint32_t count = 0;
    for (size_t pos = 0; pos < len; ) {
        int clen = utf8_char_len(data[pos]);
        if (pos + (size_t)clen > len) clen = (int)(len - pos);
        pos += (size_t)clen;
        ++count;
    }
    return count;
}

TextRenderer::DirtyRangeResult TextRenderer::computeDirtyRange(
    const std::string& oldText, const std::string& newText, uint32_t maxGlyphs) {
    DirtyRangeResult out;
    const uint8_t* oldData = reinterpret_cast<const uint8_t*>(oldText.data());
    const uint8_t* newData = reinterpret_cast<const uint8_t*>(newText.data());
    const size_t oldLen = oldText.size();
    const size_t newLen = newText.size();

    // Walk forward while codepoints match exactly (byte-wise compare of
    // whole sequences -- never splits a multi-byte char).
    uint32_t diffStart = 0;
    size_t oldPos = 0, newPos = 0;
    while (oldPos < oldLen && newPos < newLen) {
        int oclen = utf8_char_len(oldData[oldPos]);
        int nclen = utf8_char_len(newData[newPos]);
        if (oclen != nclen || memcmp(oldData + oldPos, newData + newPos, (size_t)oclen) != 0)
            break;
        oldPos += (size_t)oclen; newPos += (size_t)nclen; ++diffStart;
    }

    const uint32_t oldRemain = countUtf8Glyphs(oldData + oldPos, oldLen - oldPos);
    const uint32_t newRemain = countUtf8Glyphs(newData + newPos, newLen - newPos);
    if (oldRemain == 0 && newRemain == 0) return out;  // identical text

    out.changed = true;
    out.start = diffStart;
    out.end = diffStart + (oldRemain > newRemain ? oldRemain : newRemain);
    if (out.end > maxGlyphs) out.end = maxGlyphs;
    return out;
}

void TextRenderer::updateDirtyRange(MessageLayerCache& slot, const std::string& newText) {
    // Delegate the codepoint diff to the pure helper; keep the member writes
    // here so the cache state stays in one place.
    const DirtyRangeResult r =
        computeDirtyRange(slot.cachedText, newText, slot.maxGlyphs);
    if (!r.changed) { slot.clearDirty(); return; }
    slot.dirtyStart = r.start;
    slot.dirtyEnd   = r.end;
    slot.cachedText = newText;
}

// ---------------------------------------------------------------------------
// Pure append detection (GPU-free)
// ---------------------------------------------------------------------------

TextRenderer::AppendResult TextRenderer::detectAppend(const std::string& oldText,
                                                      const std::string& newText) {
    AppendResult out;
    if (oldText.empty()) return out;                 // nothing cached to extend
    if (newText.size() <= oldText.size()) return out; // identical or shortened
    if (newText.compare(0, oldText.size(), oldText) != 0) return out; // rewritten

    // Defensive: the shared prefix must end on a codepoint boundary. A cached
    // text truncated mid-sequence (only reachable through maxGlyphs clamping
    // or a corrupt caller) must fall back to a full rebuild rather than resume
    // layout from a byte that is not a lead byte.
    const uint8_t* nd = reinterpret_cast<const uint8_t*>(newText.data());
    if ((nd[oldText.size()] & 0xC0) == 0x80) return out;  // continuation byte

    // The prefix itself must decode into whole codepoints ending exactly at
    // oldText.size(); countUtf8Glyphs is lenient, so walk it strictly here.
    size_t pos = 0;
    uint32_t prefixGlyphs = 0;
    while (pos < oldText.size()) {
        const int clen = utf8_char_len(nd[pos]);
        if (pos + (size_t)clen > oldText.size()) return out;  // truncated prefix
        pos += (size_t)clen;
        ++prefixGlyphs;
    }

    out.isAppend = true;
    out.tailByteOffset = oldText.size();
    out.prefixGlyphs = prefixGlyphs;
    out.tailGlyphs = countUtf8Glyphs(nd + oldText.size(),
                                     newText.size() - oldText.size());
    return out;
}

bool TextRenderer::ensureCacheBuffers(MessageLayerCache& slot) {
    // Buffers are created lazily PER SLOT: a session that only ever draws one
    // text line pays for exactly one VB/IB pair, same as the old single slot.
    uint32_t maxVerts = slot.maxGlyphs * 6;
    uint32_t maxInds  = slot.maxGlyphs * 6;

    if (!bgfx::isValid(slot.vb)) {
        slot.vb = bgfx::createDynamicVertexBuffer(
            maxVerts, m_posTexLayout, BGFX_BUFFER_ALLOW_RESIZE);
        if (!bgfx::isValid(slot.vb)) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[TextRenderer] Failed to create dynamic vertex buffer.");
            return false;
        }
    }
    if (!bgfx::isValid(slot.ib)) {
        slot.ib = bgfx::createDynamicIndexBuffer(
            maxInds, BGFX_BUFFER_ALLOW_RESIZE | BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(slot.ib)) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[TextRenderer] Failed to create dynamic index buffer.");
            return false;
        }
    }
    return true;
}

// ===========================================================================
// Pure glyph layout (GPU-free) -- extracted so unit tests can pin the
// batch-cache geometry math without a GPU (P2-10). The production path
// injects glyphLookupForCache(); tests inject a fixed table.
// ===========================================================================

TextRenderer::GlyphLookupResult TextRenderer::glyphLookupForCache(uint32_t codepoint, void* userData) {
    auto* self = static_cast<TextRenderer*>(userData);
    GlyphLookupResult out;
    out.gm = self->getTTFGlyph(codepoint);
    const bool hasCjk = bgfx::isValid(self->m_cjkAtlas);
    bool fromCjk = false;
    if (hasCjk && !self->m_ttf) {
        fromCjk = true;
    }
    out.fromCjk = fromCjk;
    return out;
}

TextRenderer::GlyphLayoutResult TextRenderer::layoutGlyphs(
    const std::string& text, float penX, float penY,
    GlyphLookupFn lookup, void* userData,
    bool hasCjk, float invW, float invH,
    float cjkInvW, float cjkInvH,
    bool useTtf, float ttfAscent, size_t maxGlyphs,
    size_t byteBegin) {

    GlyphLayoutResult result;
    const uint8_t* tdata = reinterpret_cast<const uint8_t*>(text.data());
    const int tlen = (int)text.size();
    bool anyNonCjk = false;
    bool anyGlyph = false;

    // byteBegin > 0 == incremental append: skip the already-laid prefix. The
    // caller resumes penX from the cached absolute pen, so the emitted tail is
    // bit-identical to the same slice of a full layout (each glyph's position
    // depends only on the running pen and the glyph table).
    for (int i = (int)((byteBegin < text.size()) ? byteBegin : text.size()); i < tlen; ) {
        int clen = utf8_char_len(tdata[i]);
        if (i + clen > tlen) clen = tlen - i;
        const uint32_t cp = utf8_codepoint(&tdata[i], clen);
        i += clen;

        const GlyphLookupResult lr = lookup(cp, userData);
        const GlyphMetrics& gm = lr.gm;
        const bool fromCjk = lr.fromCjk;
        if (gm.w > 0 && gm.h > 0) {
            anyGlyph = true;
            if (!fromCjk) anyNonCjk = true;
        } else if (!fromCjk) {
            // Empty slot still participates in the allCJK decision, mirroring
            // the production glyphFromCjk bookkeeping.
            anyNonCjk = true;
        }

        LaidGlyph d;
        if (gm.w > 0 && gm.h > 0) {
            const float iw = fromCjk ? cjkInvW : invW;
            const float ih = fromCjk ? cjkInvH : invH;
            d.gx = penX + (float)gm.offsetX;
            d.gy = penY - (float)gm.offsetY
                   + (useTtf && !fromCjk ? ttfAscent : 8.0f);
            d.w = (float)gm.w;
            d.h = (float)gm.h;
            d.u0 = gm.x * iw;  d.v0 = gm.y * ih;
            d.u1 = (gm.x + gm.w) * iw;  d.v1 = (gm.y + gm.h) * ih;
            d.fromCjk = fromCjk;
            d.padding = useTtf && !fromCjk ? 0.5f : 0.0f;
        }
        // no-glyph case keeps the default zeroed LaidGlyph (empty slot)

        if (result.glyphs.size() < maxGlyphs) {
            result.glyphs.push_back(d);
        }
        penX += gm.advance;
    }

    result.penAdvance = penX;
    result.anyGlyph = anyGlyph;
    result.anyNonCjk = anyNonCjk;
    result.allCjk = hasCjk && anyGlyph && !anyNonCjk;
    return result;
}

void TextRenderer::buildQuadVertices(const std::vector<LaidGlyph>& glyphs,
                                     float screenW, float screenH,
                                     std::vector<float>& verts,
                                     std::vector<uint32_t>& indices,
                                     uint32_t glyphIndexBase) {
    verts.clear();
    indices.clear();
    verts.reserve(glyphs.size() * 6 * 4);
    indices.reserve(glyphs.size() * 6);
    const bool ndcOk = screenW > 0.0f && screenH > 0.0f;

    for (size_t gi = 0; gi < glyphs.size(); ++gi) {
        const LaidGlyph& d = glyphs[gi];
        const float p = d.padding;
        const float du = d.w > 0 ? (d.u1 - d.u0) * p / d.w : 0;
        const float dv = d.h > 0 ? (d.v1 - d.v0) * p / d.h : 0;
        const float u0 = d.u0-du, v0 = d.v0-dv, u1 = d.u1+du, v1 = d.v1+dv;
        // Absolute vertex slot: a tail written at glyph slot `glyphIndexBase`
        // must reference vertices (base+gi)*6, not (gi)*6.
        const uint32_t vbase = static_cast<uint32_t>((glyphIndexBase + gi) * 6);
        float nx0, ny0, nx1, ny1;
        if (ndcOk) {
            nx0 = ((d.gx-p) / screenW) * 2.0f - 1.0f;
            ny0 = 1.0f - ((d.gy-p) / screenH) * 2.0f;
            nx1 = ((d.gx+d.w+p) / screenW) * 2.0f - 1.0f;
            ny1 = 1.0f - ((d.gy+d.h+p) / screenH) * 2.0f;
        } else {
            nx0 = d.gx-p; ny0 = d.gy-p; nx1 = d.gx+d.w+p; ny1 = d.gy+d.h+p;
        }
        const float v[24] = {
            nx0, ny0, u0, v0,
            nx1, ny0, u1, v0,
            nx1, ny1, u1, v1,
            nx0, ny0, u0, v0,
            nx1, ny1, u1, v1,
            nx0, ny1, u0, v1
        };
        verts.insert(verts.end(), v, v + 24);
        indices.push_back(vbase);     indices.push_back(vbase + 1); indices.push_back(vbase + 2);
        indices.push_back(vbase + 3); indices.push_back(vbase + 4); indices.push_back(vbase + 5);
    }
}

// Shared bind+submit for a slot's resident geometry. Every path (full rebuild,
// incremental append, pure cache hit) ends here, so they cannot drift apart in
// texture selection or buffer ranges.
void TextRenderer::submitCachedSlot(MessageLayerCache& slot, uint16_t viewId,
                                    TextColor color, bgfx::ProgramHandle program) {
    float fc[4] = { color.r/255.0f, color.g/255.0f, color.b/255.0f, color.a/255.0f };
    bgfx::setUniform(m_u_color, fc);
    // TD-13: a cached CJK-only line samples the CJK atlas -- binding the TTF
    // texture for it would sample wrong glyphs.
    const bool needCjk = slot.cacheIsCjk && bgfx::isValid(m_cjkAtlas);
    bgfx::setTexture(0, m_texSampler, needCjk ? m_cjkAtlas : m_fontTexture);
    bgfx::setVertexBuffer(0, slot.vb, 0, slot.glyphCount * 6);
    bgfx::setIndexBuffer(slot.ib, 0, slot.glyphCount * 6);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(viewId, program);
}

float TextRenderer::rebuildCache(MessageLayerCache& slot, uint16_t viewId,
                                  const std::string& text,
                                  float x, float y, TextColor color,
                                  bgfx::ProgramHandle program) {
    if (!ensureCacheBuffers(slot) || text.empty()) return x;

    uint16_t texW = m_ttf ? (uint16_t)m_ttf->atlasW : (uint16_t)(m_atlasCols * m_fontGlyphW);
    uint16_t texH = m_ttf ? (uint16_t)m_ttf->atlasH : (uint16_t)(m_fontGlyphH * 3);

    const float invW = 1.0f / float(texW);
    const float invH = 1.0f / float(texH);
    const bool hasCjk = bgfx::isValid(m_cjkAtlas);
    const float cjkInvW = 1.0f / float(m_atlasW);
    const float cjkInvH = 1.0f / float(m_atlasH);

    // Pure layout phase (GPU-free; extracted for unit testing).
    GlyphLayoutResult laid = layoutGlyphs(text, x, y,
                                          &TextRenderer::glyphLookupForCache, this,
                                          hasCjk, invW, invH, cjkInvW, cjkInvH,
                                          m_ttf != nullptr,
                                          m_ttf ? (float)m_ttf->ascent : 8.0f,
                                          slot.maxGlyphs);

    // Pure vertex phase.
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    buildQuadVertices(laid.glyphs, (float)m_screenWidth, (float)m_screenHeight,
                      verts, indices);

    slot.glyphCount = static_cast<uint32_t>(laid.glyphs.size());
    slot.cacheIsCjk = laid.allCjk && bgfx::isValid(m_cjkAtlas);
    // Incremental-append bookkeeping for the NEXT frame of a typewriter reveal.
    slot.penEnd    = laid.penAdvance;
    slot.anyGlyph  = laid.anyGlyph;
    slot.anyNonCjk = laid.anyNonCjk;
    slot.geometryValid = true;

    struct PosTexVertex { float x, y, u, v; };
    static_assert(sizeof(PosTexVertex) == 4 * sizeof(float),
                  "PosTexVertex layout must match the float vertex stream");
    const uint32_t vBytes = (uint32_t)(verts.size() * sizeof(float));
    const uint32_t iBytes = (uint32_t)(indices.size() * sizeof(uint32_t));
    const bgfx::Memory* vm = bgfx::copy(verts.data(), vBytes);
    const bgfx::Memory* im = bgfx::copy(indices.data(), iBytes);
    bgfx::update(slot.vb, 0, vm);
    bgfx::update(slot.ib, 0, im);

    ++m_cacheStats.rebuildFull;
    m_cacheStats.glyphsLaidOut += laid.glyphs.size();
    m_cacheStats.vertexBytesUploaded += vBytes;
    m_cacheStats.indexBytesUploaded  += iBytes;

    submitCachedSlot(slot, viewId, color, program);
    slot.clearDirty();
    return laid.penAdvance;
}

float TextRenderer::appendToCache(MessageLayerCache& slot, uint16_t viewId,
                                  const std::string& text, const CachePlan& plan,
                                  float x, float y, TextColor color,
                                  bgfx::ProgramHandle program) {
    if (!ensureCacheBuffers(slot)) return x;

    uint16_t texW = m_ttf ? (uint16_t)m_ttf->atlasW : (uint16_t)(m_atlasCols * m_fontGlyphW);
    uint16_t texH = m_ttf ? (uint16_t)m_ttf->atlasH : (uint16_t)(m_fontGlyphH * 3);

    const float invW = 1.0f / float(texW);
    const float invH = 1.0f / float(texH);
    const bool hasCjk = bgfx::isValid(m_cjkAtlas);
    const float cjkInvW = 1.0f / float(m_atlasW);
    const float cjkInvH = 1.0f / float(m_atlasH);

    // Lay out ONLY the tail, resuming from the cached ABSOLUTE pen. Using the
    // stored float (not x + cachedPenAdvance) is what keeps the tail vertices
    // bit-identical to a full rebuild's.
    GlyphLayoutResult tail = layoutGlyphs(text, slot.penEnd, y,
                                          &TextRenderer::glyphLookupForCache, this,
                                          hasCjk, invW, invH, cjkInvW, cjkInvH,
                                          m_ttf != nullptr,
                                          m_ttf ? (float)m_ttf->ascent : 8.0f,
                                          (size_t)plan.glyphsToLayout,
                                          plan.byteBegin);

    std::vector<float> verts;
    std::vector<uint32_t> indices;
    buildQuadVertices(tail.glyphs, (float)m_screenWidth, (float)m_screenHeight,
                      verts, indices, slot.glyphCount);

    const uint32_t firstGlyph = slot.glyphCount;
    slot.glyphCount = firstGlyph + (uint32_t)tail.glyphs.size();
    // Fold the tail's CJK flags into the prefix's: allCjk is a property of the
    // WHOLE text, so the verdict must match what a full layout would compute.
    slot.anyGlyph  = slot.anyGlyph  || tail.anyGlyph;
    slot.anyNonCjk = slot.anyNonCjk || tail.anyNonCjk;
    slot.cacheIsCjk = hasCjk && slot.anyGlyph && !slot.anyNonCjk
                      && bgfx::isValid(m_cjkAtlas);
    slot.penEnd = tail.penAdvance;
    slot.cachedText = text;
    slot.geometryValid = true;

    // Partial upload: only the appended range. Both bgfx::update overloads take
    // an ELEMENT offset, not a byte offset (_startVertex / _startIndex, see
    // bgfx.h) -- 6 vertices and 6 indices per glyph.
    const uint32_t vBytes = (uint32_t)(verts.size() * sizeof(float));
    const uint32_t iBytes = (uint32_t)(indices.size() * sizeof(uint32_t));
    if (vBytes > 0) {
        const bgfx::Memory* vm = bgfx::copy(verts.data(), vBytes);
        bgfx::update(slot.vb, firstGlyph * 6u, vm);
    }
    if (iBytes > 0) {
        const bgfx::Memory* im = bgfx::copy(indices.data(), iBytes);
        bgfx::update(slot.ib, firstGlyph * 6u, im);
    }

    ++m_cacheStats.rebuildIncremental;
    m_cacheStats.glyphsLaidOut += tail.glyphs.size();
    m_cacheStats.vertexBytesUploaded += vBytes;
    m_cacheStats.indexBytesUploaded  += iBytes;

    submitCachedSlot(slot, viewId, color, program);
    slot.clearDirty();
    return tail.penAdvance;
}

// ---------------------------------------------------------------------------
// Cache update planning (pure): the single source of truth for "reuse / extend
// / rebuild" and for the amount of work that choice implies. renderTextCached()
// dispatches on this, and the tests measure it, so the measured policy IS the
// shipped policy.
// ---------------------------------------------------------------------------

TextRenderer::CachePlan TextRenderer::planCacheUpdate(const MessageLayerCache& slot,
                                                      uint16_t viewId,
                                                      const std::string& text,
                                                      float x, float y) {
    CachePlan plan;
    const uint32_t vBytesPerGlyph = 6u * 4u * (uint32_t)sizeof(float);
    const uint32_t iBytesPerGlyph = 6u * (uint32_t)sizeof(uint32_t);

    // Full rebuild is the default: it is always correct.
    const auto fullRebuild = [&]() {
        plan.action = CacheAction::FullRebuild;
        plan.byteBegin = 0;
        plan.firstGlyph = 0;
        const uint32_t all = countUtf8Glyphs(
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
        plan.glyphsToLayout = (all < slot.maxGlyphs) ? all : slot.maxGlyphs;
        plan.vertexBytes = plan.glyphsToLayout * vBytesPerGlyph;
        plan.indexBytes  = plan.glyphsToLayout * iBytesPerGlyph;
        return plan;
    };

    if (!slot.geometryValid) return fullRebuild();
    // Position/view are part of the geometry: moved text must be re-laid.
    if (slot.cachedViewId != viewId || slot.cachedX != x || slot.cachedY != y)
        return fullRebuild();

    if (slot.cachedText == text) {
        if (slot.isDirty()) return fullRebuild();  // explicitly invalidated
        plan.action = CacheAction::Hit;
        return plan;   // zero layout, zero upload
    }

    const AppendResult app = detectAppend(slot.cachedText, text);
    // The slot's resident glyph count must line up with the prefix, or the tail
    // would be written at the wrong offset. (A prefix clamped by maxGlyphs is
    // exactly this case, so it correctly falls back to a full rebuild.)
    if (!app.isAppend || slot.glyphCount != app.prefixGlyphs
        || slot.glyphCount >= slot.maxGlyphs) {
        return fullRebuild();
    }

    const uint32_t budget = slot.maxGlyphs - slot.glyphCount;
    plan.action = CacheAction::Append;
    plan.byteBegin = app.tailByteOffset;
    plan.firstGlyph = slot.glyphCount;
    plan.glyphsToLayout = (app.tailGlyphs < budget) ? app.tailGlyphs : budget;
    plan.vertexBytes = plan.glyphsToLayout * vBytesPerGlyph;
    plan.indexBytes  = plan.glyphsToLayout * iBytesPerGlyph;
    return plan;
}

size_t TextRenderer::selectCacheSlot(const MessageLayerCache* slots, size_t count,
                                     uint16_t viewId, const std::string& text,
                                     float x, float y) {
    if (!slots || count == 0) return 0;
    // 1. Exact key hit -- nothing to lay out or upload.
    for (size_t i = 0; i < count; ++i) {
        const MessageLayerCache& s = slots[i];
        if (s.geometryValid && s.matches(viewId, text, x, y)) return i;
    }
    // 2. Append-compatible slot: same view + position, cachedText is a strict
    //    prefix of text. This is the typewriter reveal's steady state.
    for (size_t i = 0; i < count; ++i) {
        const MessageLayerCache& s = slots[i];
        if (!s.geometryValid) continue;
        if (s.cachedViewId != viewId || s.cachedX != x || s.cachedY != y) continue;
        if (detectAppend(s.cachedText, text).isAppend) return i;
    }
    // 3. Least-recently-used slot (an unused slot has lastUse == 0 and wins).
    size_t victim = 0;
    for (size_t i = 1; i < count; ++i) {
        if (slots[i].lastUse < slots[victim].lastUse) victim = i;
    }
    return victim;
}

MessageLayerCache& TextRenderer::acquireSlot(uint16_t viewId, const std::string& text,
                                             float x, float y) {
    const size_t idx = selectCacheSlot(m_cacheSlots, kCacheSlots, viewId, text, x, y);
    MessageLayerCache& s = m_cacheSlots[idx];
    // An eviction is a LIVE slot being handed to a different key: that is the
    // event the old single-slot cache suffered on every alternating draw.
    if (s.geometryValid && !s.matches(viewId, text, x, y)
        && !detectAppend(s.cachedText, text).isAppend) {
        ++m_cacheStats.evictions;
    }
    m_lastSlot = idx;
    s.lastUse = ++m_cacheClock;
    return s;
}

float TextRenderer::renderTextCached(uint16_t viewId, const std::string& text,
                                      float x, float y, TextColor color,
                                      bgfx::ProgramHandle program) {
    if (!m_initialized || !m_fontDescription.active || text.empty()) return x;
    bgfx::ProgramHandle prog = bgfx::isValid(program) ? program : m_textProgram;
    if (!bgfx::isValid(prog)) return x;

    // Slot selection (LRU): an exact key hit needs no work; an append-
    // compatible slot lets the typewriter reveal extend existing geometry;
    // otherwise the least-recently-used slot is re-keyed.
    MessageLayerCache& slot = acquireSlot(viewId, text, x, y);
    const bool exactHit = slot.geometryValid && slot.matches(viewId, text, x, y);

    // planCacheUpdate() is the ONE policy function; the tests measure it, so the
    // measured behavior is the shipped behavior.
    const CachePlan plan = planCacheUpdate(slot, viewId, text, x, y);

    if (plan.action == CacheAction::Hit) {
        // A valid slot was built only after its glyphs were uploaded. The
        // append-only atlas keeps those cells/UVs stable even if another text
        // has an outstanding dirty patch, so no repeated glyph walk is needed.
        ++m_cacheStats.cacheHits;
        if (!ensureCacheBuffers(slot)) return x;
        submitCachedSlot(slot, viewId, color, prog);
        // The pen advance was computed once at rebuild time; skip the per-frame
        // O(n) glyph-advance walk.
        return x + slot.cachedPenAdvance;
    }

    // Before changing a slot's key/geometry, prepare only the required text
    // (the appended tail for typewriter growth). Failed uploads leave old
    // valid geometry intact and cannot turn the new text into a cache hit.
    if (!prepareTextGlyphs(text, plan.action == CacheAction::Append ? plan.byteBegin : 0)) return x;

    if (plan.action == CacheAction::Append) {
        // Typewriter growth: lay out and upload ONLY the new tail.
        const float pen = appendToCache(slot, viewId, text, plan, x, y, color, prog);
        slot.cachedPenAdvance = pen - x;
        return pen;
    }

    // Full rebuild: new text, rewritten/shortened text, moved text, or a
    // re-keyed slot. Keeps the dirty-range bookkeeping for observability.
    if (!exactHit) {
        if (text != slot.cachedText) updateDirtyRange(slot, text);
        else slot.markAllDirty();
        slot.cachedViewId = viewId;
        slot.cachedX = x;
        slot.cachedY = y;
    }
    slot.cachedText = text;
    // A re-keyed slot's old glyph bookkeeping must not leak into the new text.
    slot.glyphCount = 0;
    slot.anyGlyph = slot.anyNonCjk = false;
    const float pen = rebuildCache(slot, viewId, text, x, y, color, prog);
    slot.cachedPenAdvance = pen - x;
    return pen;
}

// ===========================================================================
// CJK Kinsoku Shori (避头尾法则) line-breaking rules
// ===========================================================================

bool TextRenderer::isKinsokuLineStartForbidden(uint32_t cp) {
    // 1. ASCII closing punctuation and quotes
    switch (cp) {
        case '!': case '"': case '\'': case ')': case ',':
        case '.': case ':': case ';': case '?': case ']':
        case '}':
            return true;
        default: break;
    }

    // 2. Unicode quotes and brackets (closing)
    switch (cp) {
        case 0x00BB: // » Right-pointing double angle quotation mark
        case 0x2019: // ’ Right single quotation mark
        case 0x201D: // ” Right double quotation mark
        case 0x203A: // › Single right-pointing angle quotation mark
        case 0x3009: // 〉 Right angle bracket
        case 0x300B: // 》 Right double angle bracket
        case 0x300D: // 」 Right corner bracket
        case 0x300F: // 』 Right white corner bracket
        case 0x3011: // 】 Right black lenticular bracket
        case 0x3015: // 〕 Right tortoise shell bracket
        case 0x3017: // 〗 Right white lenticular bracket
        case 0x3019: // 㙹 / 㙙 Right white tortoise shell bracket
        case 0x301B: // 㛼 / 㛛 Right white square bracket
        case 0xFE5A: // ﹚ Small right parenthesis
        case 0xFE5C: // ﹜ Small right curly bracket
        case 0xFE5E: // ﹞ Small right tortoise shell bracket
        case 0xFF09: // ） Fullwidth right parenthesis
        case 0xFF3D: // ］ Fullwidth right square bracket
        case 0xFF5D: // ｝ Fullwidth right curly bracket
        case 0xFF60: // ｠ Fullwidth right white parenthesis
        case 0xFF63: // ｣ Halfwidth right corner bracket
            return true;
        default: break;
    }

    // 3. Commas, periods, question/exclamation, fullwidth & halfwidth
    switch (cp) {
        case 0x3001: // 、 Ideographic comma
        case 0x3002: // 。 Ideographic full stop
        case 0xFE50: // ﹐ Small comma
        case 0xFE51: // ﹑ Small ideographic comma
        case 0xFE52: // ﹒ Small full stop
        case 0xFE54: // ﹔ Small semicolon
        case 0xFE55: // ﹕ Small colon
        case 0xFE56: // ﹖ Small question mark
        case 0xFE57: // ﹗ Small exclamation mark
        case 0xFF01: // ！ Fullwidth exclamation mark
        case 0xFF0C: // ， Fullwidth comma
        case 0xFF0E: // ． Fullwidth full stop
        case 0xFF1A: // ： Fullwidth colon
        case 0xFF1B: // ； Fullwidth semicolon
        case 0xFF1F: // ？ Fullwidth question mark
        case 0xFF61: // ｡ Halfwidth ideographic full stop
        case 0xFF64: // ､ Halfwidth ideographic comma
            return true;
        default: break;
    }

    // 4. Connecting / middle dots / ellipsis / dashes / prolonging / iteration marks
    switch (cp) {
        case 0x00B7: // · Middle dot
        case 0x2014: // — Em dash
        case 0x2015: // ― Horizontal bar
        case 0x2025: // ‥ Two dot leader
        case 0x2026: // … Horizontal ellipsis
        case 0x3005: // 々 Ideographic iteration mark
        case 0x301C: // 〜 Wave dash
        case 0x303B: // 〻 Vertical ideographic iteration mark
        case 0x303C: // 〼 Masu mark
        case 0x309D: // ゝ Hiragana iteration mark
        case 0x309E: // ゞ Hiragana voiced iteration mark
        case 0x30FB: // ・ Katakana middle dot
        case 0x30FC: // ー Katakana-Hiragana prolonged sound mark
        case 0x30FD: // ヽ Katakana iteration mark
        case 0x30FE: // ヾ Katakana voiced iteration mark
        case 0xFF5E: // ～ Fullwidth tilde
        case 0xFF65: // ･ Halfwidth katakana middle dot
        case 0xFF70: // ｰ Halfwidth katakana-hiragana prolonged sound mark
            return true;
        default: break;
    }

    // 5. Small Kana (Hiragana & Katakana)
    // Small Hiragana
    switch (cp) {
        case 0x3041: // ぁ
        case 0x3043: // ぃ
        case 0x3045: // ぅ
        case 0x3047: // ぇ
        case 0x3049: // ぉ
        case 0x3063: // っ
        case 0x3083: // ゃ
        case 0x3085: // ゅ
        case 0x3087: // ょ
        case 0x308E: // ゎ
        case 0x3095: // ゕ
        case 0x3096: // ゖ
            return true;
        default: break;
    }
    // Small Katakana
    switch (cp) {
        case 0x30A1: // ァ
        case 0x30A3: // ィ
        case 0x30A5: // ゥ
        case 0x30A7: // ェ
        case 0x30A9: // ォ
        case 0x30C3: // ッ
        case 0x30E3: // ャ
        case 0x30E5: // ュ
        case 0x30E7: // ョ
        case 0x30EE: // ヮ
        case 0x30F5: // ヵ
        case 0x30F6: // ヶ
            return true;
        default: break;
    }
    // Halfwidth Small Katakana (0xFF67..0xFF6F: ｧ ｨ ｩ ｪ ｫ ｬ ｭ ｮ ｯ)
    if (cp >= 0xFF67 && cp <= 0xFF6F) return true;
    // Katakana Phonetic Extensions (0x31F0..0x31FF: ㇰ..ㇿ)
    if (cp >= 0x31F0 && cp <= 0x31FF) return true;

    // 6. Units and symbols that shouldn't start a line
    switch (cp) {
        case 0x0025: // % Percent
        case 0x00B0: // ° Degree
        case 0x2032: // ′ Prime
        case 0x2033: // ″ Double prime
        case 0x2103: // ℃ Degree Celsius
        case 0xFF05: // ％ Fullwidth percent
            return true;
        default: break;
    }

    return false;
}

bool TextRenderer::isKinsokuLineEndForbidden(uint32_t cp) {
    // 1. ASCII opening brackets
    switch (cp) {
        case '(': case '[': case '{':
            return true;
        default: break;
    }

    // 2. Unicode quotes and brackets (opening)
    switch (cp) {
        case 0x00AB: // « Left-pointing double angle quotation mark
        case 0x2018: // ‘ Left single quotation mark
        case 0x201C: // “ Left double quotation mark
        case 0x2039: // ‹ Single left-pointing angle quotation mark
        case 0x3008: // 〈 Left angle bracket
        case 0x300A: // 《 Left double angle bracket
        case 0x300C: // 「 Left corner bracket
        case 0x300E: // 『 Left white corner bracket
        case 0x3010: // 【 Left black lenticular bracket
        case 0x3014: // 〔 Left tortoise shell bracket
        case 0x3016: // 〖 Left white lenticular bracket
        case 0x3018: // 〘 Left white tortoise shell bracket
        case 0x301A: // 〚 Left white square bracket
        case 0xFE59: // ﹙ Small left parenthesis
        case 0xFE5B: // ﹛ Small left curly bracket
        case 0xFE5D: // ﹝ Small left tortoise shell bracket
        case 0xFF08: // （ Fullwidth left parenthesis
        case 0xFF3B: // ［ Fullwidth left square bracket
        case 0xFF5B: // ｛ Fullwidth left curly bracket
        case 0xFF5F: // ｟ Fullwidth left white parenthesis
        case 0xFF62: // ｢ Halfwidth left corner bracket
            return true;
        default: break;
    }

    // 3. Currency and prefix symbols
    switch (cp) {
        case 0x0023: // # Number sign
        case 0x0024: // $ Dollar
        case 0x00A3: // £ Pound
        case 0x00A5: // ¥ Yen
        case 0x00A7: // § Section sign
        case 0x20AC: // € Euro
        case 0x20A9: // ₩ Won
        case 0x2116: // № Numero sign
        case 0xFF03: // ＃ Fullwidth number sign
        case 0xFF04: // ＄ Fullwidth dollar
        case 0xFFE1: // ￡ Fullwidth pound
        case 0xFFE5: // ￥ Fullwidth yen
        case 0xFFE6: // ￦ Fullwidth won
            return true;
        default: break;
    }

    return false;
}

// ===========================================================================
// Kinsoku-aware greedy line breaking
//
// WIRING STATUS (t10): the three predicates above used to be an API with unit
// tests and NO caller -- nothing in the engine asked them where a line may
// break. wrapTextKinsoku() is the missing decision point: it is the one place
// that turns those predicates into an actual line-break result, and it is
// exercised by behavior tests (given a text + width, assert the forbidden
// positions are not broken).
//
// It is NOT yet the engine's production wrap path, and that is deliberate:
//   * Production message wrapping happens in Lua, in scripts/kag/text_layout.lua
//     (wrap_paragraph -> can_break, with its own OPENING/CLOSING_PUNCTUATION
//     tables). The C++ renderText() only honors explicit '\n'; it has never
//     measured a max width, so there is no C++ caller to hand this to.
//   * Routing production text through here would mean either adding a Lua
//     binding (src/script/bindings/) or widening IRenderDevice -- both outside
//     this task's file set, and both would put TWO kinsoku tables in the same
//     decision path until one side is deleted.
// Who should wire it, and where: whoever owns the C++/Lua duplication decision.
// Either (1) delete the Lua table and have text_layout.lua call a new
// kag.text_measure binding that forwards to wrapTextKinsoku(), or (2) declare
// Lua authoritative for wrapping and keep this as the C++-side (ErrorUI /
// future native UI) implementation. Until then this is a tested, self-contained
// algorithm rather than an untested unused predicate set.
// ===========================================================================

std::vector<size_t> TextRenderer::wrapTextKinsoku(const std::string& text,
                                                  float maxWidth,
                                                  AdvanceFn advance, void* userData) {
    std::vector<size_t> breaks;
    breaks.push_back(0);
    if (text.empty() || !advance) { breaks.push_back(text.size()); return breaks; }

    // Decode once: byte offset + codepoint + advance per character.
    struct Ch { size_t off; size_t len; uint32_t cp; float adv; };
    std::vector<Ch> chars;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n; ) {
        int clen = utf8_char_len(d[i]);
        if (i + (size_t)clen > n) clen = (int)(n - i);
        const uint32_t cp = utf8_codepoint(&d[i], clen);
        chars.push_back(Ch{ i, (size_t)clen, cp, advance(cp, userData) });
        i += (size_t)clen;
    }

    size_t lineStart = 0;               // index into chars
    float  lineWidth = 0.0f;
    for (size_t i = 0; i < chars.size(); ++i) {
        // Explicit newline always breaks and is consumed by the break.
        if (chars[i].cp == '\n') {
            breaks.push_back(chars[i].off + chars[i].len);
            lineStart = i + 1;
            lineWidth = 0.0f;
            continue;
        }

        const float next = lineWidth + chars[i].adv;
        if (next <= maxWidth || i == lineStart) {
            // Fits (or is the sole character on the line: never produce an
            // empty line, even for a glyph wider than maxWidth).
            lineWidth = next;
            continue;
        }

        // Overflow at i: the greedy break is "before i". Walk left to the last
        // position the kinsoku rules allow. Position k means "break between
        // chars[k-1] and chars[k]".
        size_t breakAt = i;
        while (breakAt > lineStart + 1
               && !canBreakBetween(chars[breakAt - 1].cp, chars[breakAt].cp)) {
            --breakAt;
        }
        // No legal position inside the line: fall back to the overflow point.
        // (Standard behavior -- a forbidden pair that spans the whole line must
        // still be laid out somewhere, and an infinite loop is never acceptable.)
        if (breakAt <= lineStart) breakAt = i;

        breaks.push_back(chars[breakAt].off);
        lineStart = breakAt;
        lineWidth = 0.0f;
        for (size_t k = breakAt; k <= i; ++k) lineWidth += chars[k].adv;
    }

    breaks.push_back(text.size());
    return breaks;
}

bool TextRenderer::canBreakBetween(uint32_t leftCodepoint, uint32_t rightCodepoint) {
    if (leftCodepoint == 0 || rightCodepoint == 0) return true;

    // Cannot break after line-end forbidden characters
    if (isKinsokuLineEndForbidden(leftCodepoint)) return false;

    // Cannot break before line-start forbidden characters
    if (isKinsokuLineStartForbidden(rightCodepoint)) return false;

    // Unicode combining marks (cannot break before combining mark)
    if ((rightCodepoint >= 0x0300 && rightCodepoint <= 0x036F) ||
        (rightCodepoint >= 0x1AB0 && rightCodepoint <= 0x1AFF) ||
        (rightCodepoint >= 0x1DC0 && rightCodepoint <= 0x1DFF) ||
        (rightCodepoint >= 0x20D0 && rightCodepoint <= 0x20FF) ||
        (rightCodepoint >= 0xFE20 && rightCodepoint <= 0xFE2F)) {
        return false;
    }

    // Inseparable consecutive punctuation pairs:
    // Two ellipses (……), two em-dashes (—— or ――), two two-dot leaders (‥‥)
    if ((leftCodepoint == 0x2026 && rightCodepoint == 0x2026) ||
        (leftCodepoint == 0x2025 && rightCodepoint == 0x2025) ||
        (leftCodepoint == 0x2014 && rightCodepoint == 0x2014) ||
        (leftCodepoint == 0x2015 && rightCodepoint == 0x2015)) {
        return false;
    }

    return true;
}

} // namespace Caesura
