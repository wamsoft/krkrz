// MemoryOverlay の OpenGL ES 直接版描画。SDLOGLDrawDevice (SDL_Renderer 不在経路)
// と OGLDrawDevice (Canvas/Texture フル機能版) の両方の Show 末尾から呼べる
// 共通実装。SDL 依存はなし (SDL_Renderer 版は sdl3/visual/MemoryOverlayRender.cpp
// で SDLDrawDevice 用に維持)。
//
// 描画レイアウト・色は SDL 版に準拠。OGL 直接 (font は 8x8 bitmap embed) で描画。
// 計測ロジック (delta 計算 + log 出力) は SDL 版と重複コードあるが、static 共有
// 不要なため敢えて独立 (両関数が同時呼出されない前提)。

#include "tjsCommHead.h"
#include "MemoryOverlayGL.h"

#ifdef KRKRZ_ENABLE_MEMORY_OVERLAY

#include "MemoryOverlay.h"
#include "ThreadIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"

#include "OpenGLHeader.h"
#include "GLShaderUtil.h"
#include "OpenGLContext.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>

namespace {

//===========================================================================
// SDL 依存排除用の型/関数
//===========================================================================

// OverlayColor と互換 (RGBA8 4 byte)。SDL 版とコードを揃えやすくするため同名の field。
struct OverlayColor {
    uint8_t r, g, b, a;
};

// 起動時を起点とした ms tick (SDL_GetTicks 互換)。
uint32_t GetTicksMs() {
    using namespace std::chrono;
    static const auto base = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - base).count();
}

//===========================================================================
// レイアウト定数 (SDL 版と同じ)
//===========================================================================
constexpr float kOverlayScale = 1.5f;
constexpr int kPanelW       = 320;
// Sound 行 + SysFree 行 ぶんで +24 (kRowH × 2)
#if defined(KRKRZ_DRAW_STATS) && defined(KRKRZ_SDLMEMORY_STAT)
// Draw stats + render stats で +7 行 + キャッシュ件数 2 行 + GlobalAlloc 2 行 (GblK + GblS) + Sound 1 行 + SysFree 1 行
constexpr int kPanelH       = 308;
constexpr int kHeaderH      = 228;
#elif defined(KRKRZ_DRAW_STATS)
// Draw stats あり、SDL stats なし → GlobalAlloc 1 行のみ (GblK) + Sound 1 行 + SysFree 1 行
constexpr int kPanelH       = 296;
constexpr int kHeaderH      = 216;
#elif defined(KRKRZ_SDLMEMORY_STAT)
// 通常時: 11 行 (FPS + File + Bitmap + Sound + RSS + Alloc/s + FileCache + ImageCache + GblK + GblS + SysFree)
constexpr int kPanelH       = 224;
constexpr int kHeaderH      = 144;
#else
// SDL stats なし: 10 行 (GblS が消える、Sound + SysFree 追加)
constexpr int kPanelH       = 212;
constexpr int kHeaderH      = 132;
#endif
constexpr int kPanelMargin  = 8;
constexpr int kRowH         = 12;
constexpr uint32_t kFpsRefreshMs = 500;
constexpr uint32_t kStatsRefreshMs = 500;

double ExtractFile  (const TVPMemoryOverlaySample &s) { return (double)s.file_used; }
double ExtractBitmap(const TVPMemoryOverlaySample &s) { return (double)s.bitmap_used; }
double ExtractSound (const TVPMemoryOverlaySample &s) { return (double)s.sound_used; }
double ExtractRSS   (const TVPMemoryOverlaySample &s) { return (double)s.process_rss; }

const char *TrimSitePath(const char *p) {
    if (!p) return "";
    const char *last = p;
    for (const char *s = p; *s; ++s) {
        if (*s == '/' || *s == '\\') last = s + 1;
    }
    return last;
}

//===========================================================================
// 8x8 bitmap font (public domain)
// font8x8_basic.h (https://github.com/dhepper/font8x8) からの抜粋。ASCII 0x20-0x7E。
// 各文字は 8 バイト = 8 行、各バイトは 1 行 (LSB が左端、MSB が右端)。
//===========================================================================
static const uint8_t kFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 0x21 '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x22 '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 0x23 '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 0x24 '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 0x25 '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 0x26 '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 0x27 '\''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 0x28 '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 0x29 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 0x2A '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 0x2B '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 0x2C ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 0x2D '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 0x2E '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 0x2F '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0x30 '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 0x31 '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 0x32 '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 0x33 '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 0x34 '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 0x35 '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 0x36 '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 0x37 '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 0x38 '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 0x39 '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 0x3A ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 0x3B ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 0x3C '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 0x3D '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 0x3E '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 0x3F '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 0x40 '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 0x41 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 0x42 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 0x43 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 0x44 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 0x45 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 0x46 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 0x47 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 0x48 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x49 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 0x4A 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 0x4B 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 0x4C 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 0x4D 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 0x4E 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 0x4F 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 0x50 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 0x51 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 0x52 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 0x53 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x54 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 0x55 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 0x56 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 0x57 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 0x58 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 0x59 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 0x5A 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 0x5B '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 0x5C '\\'
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 0x5D ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 0x5E '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 0x5F '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 0x60 '`'
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 0x61 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 0x62 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 0x63 'c'
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // 0x64 'd'
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 0x65 'e'
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 0x66 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 0x67 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 0x68 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x69 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 0x6A 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 0x6B 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 0x6C 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 0x6D 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 0x6E 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 0x6F 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 0x70 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 0x71 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 0x72 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 0x73 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 0x74 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 0x75 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 0x76 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 0x77 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 0x78 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 0x79 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 0x7A 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 0x7B '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 0x7C '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 0x7D '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x7E '~'
};
constexpr int kFontCharW = 8;
constexpr int kFontCharH = 8;
constexpr int kFontCharCount = 95;

//===========================================================================
// OGL state (lazy init、SDLOGLDrawDevice の GL context 上で初期化)
//===========================================================================
struct GLOverlayState {
    bool   initialized = false;
    GLuint program     = 0;
    GLuint vbo         = 0;
    GLuint font_tex    = 0;
    GLint  loc_screenSize = -1;  // uniform vec2: viewport (px)
    GLint  loc_useTex     = -1;  // uniform int: 0=color only, 1=sample texture
    GLint  loc_tex        = -1;  // uniform sampler2D
    GLint  attr_pos       = -1;
    GLint  attr_uv        = -1;
    GLint  attr_color     = -1;
};

GLOverlayState g_gl;

// vertex: pos.xy (px from top-left), uv.xy (0..1), rgba8
struct Vertex {
    float    x, y;
    float    u, v;
    uint8_t  r, g, b, a;
};
static_assert(sizeof(Vertex) == 5 * sizeof(float), "Vertex packing");

const char *kVS = R"(#version 100
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform vec2  u_screenSize;
varying vec2  v_uv;
varying vec4  v_color;
void main() {
    // pixel coords (top-left origin) → clip space
    vec2 p = a_pos / u_screenSize;
    p.y = 1.0 - p.y;
    p = p * 2.0 - 1.0;
    gl_Position = vec4(p, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
}
)";

const char *kFS = R"(#version 100
precision mediump float;
uniform int       u_useTex;
uniform sampler2D u_tex;
varying vec2      v_uv;
varying vec4      v_color;
void main() {
    if (u_useTex == 1) {
        // font texture: alpha-only (R channel から取り出し)
        float a = texture2D(u_tex, v_uv).r;
        gl_FragColor = vec4(v_color.rgb, v_color.a * a);
    } else {
        gl_FragColor = v_color;
    }
}
)";

bool InitGL() {
    if (g_gl.initialized) return true;

    g_gl.program = CompileProgram(kVS, kFS);
    if (!g_gl.program) {
        TVPLOG_ERROR("MemoryOverlayGL: shader compile failed");
        return false;
    }
    g_gl.loc_screenSize = glGetUniformLocation(g_gl.program, "u_screenSize");
    g_gl.loc_useTex     = glGetUniformLocation(g_gl.program, "u_useTex");
    g_gl.loc_tex        = glGetUniformLocation(g_gl.program, "u_tex");
    g_gl.attr_pos       = glGetAttribLocation (g_gl.program, "a_pos");
    g_gl.attr_uv        = glGetAttribLocation (g_gl.program, "a_uv");
    g_gl.attr_color     = glGetAttribLocation (g_gl.program, "a_color");

    glGenBuffers(1, &g_gl.vbo);

    // Font atlas: 95 char × 8x8、横 16 char × 縦 6 char に並べる (= 128x48)。
    // R8 1 byte/pixel。
    constexpr int kAtlasCols = 16;
    constexpr int kAtlasRows = 6;  // ceil(95 / 16) = 6
    constexpr int kAtlasW = kAtlasCols * kFontCharW;  // 128
    constexpr int kAtlasH = kAtlasRows * kFontCharH;  // 48
    static uint8_t atlas[kAtlasW * kAtlasH] = {};
    for (int ch = 0; ch < kFontCharCount; ++ch) {
        const int col = ch % kAtlasCols;
        const int row = ch / kAtlasCols;
        const int x0 = col * kFontCharW;
        const int y0 = row * kFontCharH;
        for (int yy = 0; yy < kFontCharH; ++yy) {
            uint8_t bits = kFont8x8[ch][yy];
            for (int xx = 0; xx < kFontCharW; ++xx) {
                atlas[(y0 + yy) * kAtlasW + (x0 + xx)] = (bits & (1 << xx)) ? 0xFF : 0x00;
            }
        }
    }
    glGenTextures(1, &g_gl.font_tex);
    glBindTexture(GL_TEXTURE_2D, g_gl.font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0,
                 GL_RED, GL_UNSIGNED_BYTE, atlas);
    glBindTexture(GL_TEXTURE_2D, 0);

    g_gl.initialized = true;
    return true;
}

//===========================================================================
// 描画 helper (Vertex を vector に積んで最後に 1 回 draw)
//===========================================================================
struct Batch {
    std::vector<Vertex> verts;
    bool use_tex = false; // 切替境界で flush

    int viewport_w, viewport_h;
    float scale; // overlay 内のスケール (= kOverlayScale)

    void Reset(int vw, int vh, float s) {
        verts.clear();
        viewport_w = vw;
        viewport_h = vh;
        scale = s;
        use_tex = false;
    }
};

// 矩形塗り (top-left x,y、size w,h、color)
void AddFilledRect(Batch &b, float x, float y, float w, float h, OverlayColor c) {
    if (b.use_tex) {
        // mode 切替: flush は最後に呼ぶので、ここで切替フラグだけ立てる方式は無理。
        // mode を 2 系統に分けるか、頂点に mode 属性追加するかだが、ここでは簡略化のため
        // 1 batch で 1 mode に統一 (= 後から呼び分けで描画する)。
        // 実装簡略化: rect/line/text を別 batch にする。Render 関数で別々に描画。
    }
    const float sx = x * b.scale;
    const float sy = y * b.scale;
    const float sw = w * b.scale;
    const float sh = h * b.scale;
    Vertex v0{sx,      sy,      0, 0, c.r, c.g, c.b, c.a};
    Vertex v1{sx + sw, sy,      0, 0, c.r, c.g, c.b, c.a};
    Vertex v2{sx,      sy + sh, 0, 0, c.r, c.g, c.b, c.a};
    Vertex v3{sx + sw, sy + sh, 0, 0, c.r, c.g, c.b, c.a};
    b.verts.push_back(v0);
    b.verts.push_back(v1);
    b.verts.push_back(v2);
    b.verts.push_back(v2);
    b.verts.push_back(v1);
    b.verts.push_back(v3);
}

// 矩形枠 (top-left x,y、size w,h、color、太さ 1 px)
void AddRectOutline(Batch &b, float x, float y, float w, float h, OverlayColor c) {
    AddFilledRect(b, x,         y,         w, 1, c);  // top
    AddFilledRect(b, x,         y + h - 1, w, 1, c);  // bottom
    AddFilledRect(b, x,         y,         1, h, c);  // left
    AddFilledRect(b, x + w - 1, y,         1, h, c);  // right
}

// 線分 (太さ 1 px、長方形ぎり一致しないが許容)
void AddLine(Batch &b, float x0, float y0, float x1, float y1, OverlayColor c) {
    // 簡易: 矩形に近似 (水平/垂直/斜め含めて 1 px 厚さ)
    // 斜線では幅 1 px の単純塗り (品質は気にしない)
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-3f) return;
    // perpendicular unit
    const float nx = -dy / len * 0.5f;
    const float ny =  dx / len * 0.5f;
    const float sx0 = (x0 + nx) * b.scale, sy0 = (y0 + ny) * b.scale;
    const float sx1 = (x0 - nx) * b.scale, sy1 = (y0 - ny) * b.scale;
    const float sx2 = (x1 + nx) * b.scale, sy2 = (y1 + ny) * b.scale;
    const float sx3 = (x1 - nx) * b.scale, sy3 = (y1 - ny) * b.scale;
    Vertex v0{sx0, sy0, 0, 0, c.r, c.g, c.b, c.a};
    Vertex v1{sx1, sy1, 0, 0, c.r, c.g, c.b, c.a};
    Vertex v2{sx2, sy2, 0, 0, c.r, c.g, c.b, c.a};
    Vertex v3{sx3, sy3, 0, 0, c.r, c.g, c.b, c.a};
    b.verts.push_back(v0);
    b.verts.push_back(v1);
    b.verts.push_back(v2);
    b.verts.push_back(v2);
    b.verts.push_back(v1);
    b.verts.push_back(v3);
}

// テキスト描画 (8x8 font、各 glyph を quad で)
void AddText(Batch &b, float x, float y, const char *s, OverlayColor c) {
    constexpr int kAtlasCols = 16;
    constexpr int kAtlasRows = 6;
    constexpr float kAtlasW = (float)(kAtlasCols * kFontCharW);  // 128
    constexpr float kAtlasH = (float)(kAtlasRows * kFontCharH);  // 48
    const float scale = b.scale;
    const float gw = kFontCharW * scale;
    const float gh = kFontCharH * scale;
    float cx = x * scale;
    const float cy = y * scale;
    for (; *s; ++s) {
        const unsigned char ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) {
            cx += gw;
            continue;
        }
        const int idx = ch - 0x20;
        const int col = idx % kAtlasCols;
        const int row = idx / kAtlasCols;
        const float u0 = (col * kFontCharW) / kAtlasW;
        const float v0 = (row * kFontCharH) / kAtlasH;
        const float u1 = u0 + (float)kFontCharW / kAtlasW;
        const float v1 = v0 + (float)kFontCharH / kAtlasH;
        Vertex va{cx,      cy,      u0, v0, c.r, c.g, c.b, c.a};
        Vertex vb{cx + gw, cy,      u1, v0, c.r, c.g, c.b, c.a};
        Vertex vc{cx,      cy + gh, u0, v1, c.r, c.g, c.b, c.a};
        Vertex vd{cx + gw, cy + gh, u1, v1, c.r, c.g, c.b, c.a};
        b.verts.push_back(va);
        b.verts.push_back(vb);
        b.verts.push_back(vc);
        b.verts.push_back(vc);
        b.verts.push_back(vb);
        b.verts.push_back(vd);
        cx += gw;
    }
}

void FlushBatch(const Batch &b, bool use_font_tex) {
    if (b.verts.empty()) return;
    glUseProgram(g_gl.program);
    glUniform2f(g_gl.loc_screenSize, (float)b.viewport_w, (float)b.viewport_h);
    glUniform1i(g_gl.loc_useTex, use_font_tex ? 1 : 0);
    if (use_font_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_gl.font_tex);
        glUniform1i(g_gl.loc_tex, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(b.verts.size() * sizeof(Vertex)),
                 b.verts.data(),
                 GL_STREAM_DRAW);

    glEnableVertexAttribArray(g_gl.attr_pos);
    glVertexAttribPointer(g_gl.attr_pos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(g_gl.attr_uv);
    glVertexAttribPointer(g_gl.attr_uv, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(g_gl.attr_color);
    glVertexAttribPointer(g_gl.attr_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          (void*)offsetof(Vertex, r));

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b.verts.size());

    glDisableVertexAttribArray(g_gl.attr_pos);
    glDisableVertexAttribArray(g_gl.attr_uv);
    glDisableVertexAttribArray(g_gl.attr_color);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (use_font_tex) glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void DrawSeriesLineGL(Batch &b, const std::vector<TVPMemoryOverlaySample> &samples,
                      double (*extract)(const TVPMemoryOverlaySample &),
                      OverlayColor color, double max_val,
                      int graph_x, int graph_y, int graph_w, int graph_h)
{
    if (samples.size() < 2 || max_val <= 0) return;
    const size_t n = samples.size();
    const double x_step = (n > 1) ? (double)graph_w / (double)(TVPMemoryOverlay::kMaxSamples - 1) : 0.0;
    const size_t base = TVPMemoryOverlay::kMaxSamples - n;
    for (size_t i = 1; i < n; ++i) {
        double v0 = extract(samples[i - 1]);
        double v1 = extract(samples[i]);
        float x0 = (float)(graph_x + (base + i - 1) * x_step);
        float x1 = (float)(graph_x + (base + i)     * x_step);
        float y0 = (float)(graph_y + graph_h - (v0 / max_val) * graph_h);
        float y1 = (float)(graph_y + graph_h - (v1 / max_val) * graph_h);
        AddLine(b, x0, y0, x1, y1, color);
    }
}

} // anonymous namespace

//===========================================================================
// 公開関数: TVPRenderMemoryOverlayGL
// SDLOGLDrawDevice::Show 末尾から呼び出す。GL context が current な前提。
// FPS 計測 + memory snapshot + 計測値 delta + log 出力 + GL 描画。
//===========================================================================
void TVPRenderMemoryOverlayGL()
{
    if (!TVPMemoryOverlay::IsEnabled()) return;
    if (!InitGL()) return;

    //-------- FPS 計測 ----------
    static uint32_t s_fps_last_refresh_ms = 0;
    static int      s_fps_frame_count     = 0;
    static double   s_fps_value           = 0.0;
    {
        const uint32_t now_ms = GetTicksMs();
        s_fps_frame_count++;
        if (s_fps_last_refresh_ms == 0) {
            s_fps_last_refresh_ms = now_ms;
        } else {
            const uint32_t elapsed = now_ms - s_fps_last_refresh_ms;
            if (elapsed >= kFpsRefreshMs) {
                s_fps_value = (double)s_fps_frame_count * 1000.0 / (double)elapsed;
                s_fps_frame_count     = 0;
                s_fps_last_refresh_ms = now_ms;
            }
        }
    }

    std::vector<TVPMemoryOverlaySample> samples;
    TVPMemoryOverlay::GetSnapshot(samples);
    if (samples.empty()) return;

    // viewport size
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    const int viewport_w = vp[2], viewport_h = vp[3];
    const int scaled_win_w = (int)((float)viewport_w / kOverlayScale);
    if (scaled_win_w < kPanelW + kPanelMargin * 2) return;
    const int px = scaled_win_w - kPanelW - kPanelMargin;
    const int py = kPanelMargin;

    // バイト最大値 (グラフ縦軸スケール)
    double max_bytes = 0.0;
    for (auto &s : samples) {
        if ((double)s.file_used   > max_bytes) max_bytes = (double)s.file_used;
        if ((double)s.bitmap_used > max_bytes) max_bytes = (double)s.bitmap_used;
        if ((double)s.sound_used  > max_bytes) max_bytes = (double)s.sound_used;
        if ((double)s.process_rss > max_bytes) max_bytes = (double)s.process_rss;
    }
    if (max_bytes <= 0) max_bytes = 1.0;

    // 共通色 (既存 SDL 版準拠)
    auto &latest = samples.back();
    char buf[160];
    const OverlayColor cFPS {255, 255, 255, 255};
    const OverlayColor cFile{255, 96,  96,  255};
    const OverlayColor cBmap{96,  255, 96,  255};
    const OverlayColor cSnd {255, 160, 255, 255};
    const OverlayColor cRSS {96,  160, 255, 255};
    const OverlayColor cRate{255, 220, 96,  255};
    const OverlayColor cBg  {0,   0,   0,   180};
    const OverlayColor cBd  {96,  96,  96,  255};

    // 描画準備
    Batch shape_batch; shape_batch.Reset(viewport_w, viewport_h, kOverlayScale);
    Batch text_batch;  text_batch.Reset (viewport_w, viewport_h, kOverlayScale);

    // 半透明背景 + 枠
    AddFilledRect (shape_batch, (float)px, (float)py, (float)kPanelW, (float)kPanelH, cBg);
    AddRectOutline(shape_batch, (float)px, (float)py, (float)kPanelW, (float)kPanelH, cBd);

    // テキスト行
    int line_y = py + 4;
    auto putLine = [&](OverlayColor c, const char *fmt, auto... args) {
        if constexpr (sizeof...(args) == 0) {
            AddText(text_batch, (float)(px + 6), (float)line_y, fmt, c);
        } else {
            std::snprintf(buf, sizeof(buf), fmt, args...);
            AddText(text_batch, (float)(px + 6), (float)line_y, buf, c);
        }
        line_y += kRowH;
    };

    constexpr double MB = 1024.0 * 1024.0;
    putLine(cFPS,  "FPS:     %7.1f",                   s_fps_value);
    putLine(cFile, "File:    %7.2f MB (peak %7.2f)",
            latest.file_used   / MB, latest.file_peak   / MB);
    putLine(cBmap, "Bitmap:  %7.2f MB (peak %7.2f)",
            latest.bitmap_used / MB, latest.bitmap_peak / MB);
    putLine(cSnd,  "Sound:   %7.2f MB (peak %7.2f)",
            latest.sound_used  / MB, latest.sound_peak  / MB);
    putLine(cRSS,  "RSS:     %7.2f MB",                latest.process_rss / MB);

    // Alloc/s (近似)
    if (samples.size() >= 2) {
        const auto &prev = samples[samples.size() - 2];
        const auto &curr = latest;
        const tjs_uint64 alloc_per_sec_file =
            (curr.file_alloc_count > prev.file_alloc_count) ?
                (curr.file_alloc_count - prev.file_alloc_count) : 0;
        const tjs_uint64 alloc_per_sec_bmp =
            (curr.bitmap_alloc_count > prev.bitmap_alloc_count) ?
                (curr.bitmap_alloc_count - prev.bitmap_alloc_count) : 0;
        const tjs_uint64 alloc_per_sec_snd =
            (curr.sound_alloc_count > prev.sound_alloc_count) ?
                (curr.sound_alloc_count - prev.sound_alloc_count) : 0;
        putLine(cRate, "Alloc/s F:%5llu B:%5llu S:%5llu",
                (unsigned long long)alloc_per_sec_file,
                (unsigned long long)alloc_per_sec_bmp,
                (unsigned long long)alloc_per_sec_snd);
    } else {
        putLine(cRate, "Alloc/s  (collecting...)");
    }

    // キャッシュエントリ件数 (file 層 / decode 層)。pinned は内訳。
    const OverlayColor cFCache{160, 192, 255, 255};
    const OverlayColor cICache{255, 192, 160, 255};
    putLine(cFCache, "FileCache:  %5zu (pin %5zu)",
            latest.file_cache_count, latest.file_cache_pinned);
    putLine(cICache, "ImageCache: %5zu (pin %5zu)",
            latest.image_cache_count, latest.image_cache_pinned);

    // GlobalAllocStats (operator new + TJS_malloc + SDL3 alloc を一元集計)。
    // pool_cap > 0 なら pool 経路、0 は無効化または tracking 未活性。
    // fallback > 0 は pool 容量超過の累積回数 (赤系で警告色)。
    // GblS (SDL) 行は KRKRZ_SDLMEMORY_STAT=ON のときだけ表示。
    const OverlayColor cGblK    {200, 255, 200, 255};
    const OverlayColor cGblWarn {255, 120, 120, 255};
    if (latest.krkrz_pool_cap > 0) {
        bool ovf = latest.krkrz_fallback_count > 0;
        putLine(ovf ? cGblWarn : cGblK,
                "GblK live%5.1fM pool%5.1f/%4lluM fb%llu",
                latest.krkrz_live      / MB,
                latest.krkrz_pool_used / MB,
                (unsigned long long)(latest.krkrz_pool_cap / (uint64_t)MB),
                (unsigned long long)latest.krkrz_fallback_count);
    } else {
        putLine(cGblK, "GblK live%5.1fM (no pool)", latest.krkrz_live / MB);
    }
#ifdef KRKRZ_SDLMEMORY_STAT
    const OverlayColor cGblS    {200, 220, 255, 255};
    if (latest.sdl_pool_cap > 0) {
        bool ovf = latest.sdl_fallback_count > 0;
        putLine(ovf ? cGblWarn : cGblS,
                "GblS live%5.1fM pool%5.1f/%4lluM fb%llu",
                latest.sdl_live      / MB,
                latest.sdl_pool_used / MB,
                (unsigned long long)(latest.sdl_pool_cap / (uint64_t)MB),
                (unsigned long long)latest.sdl_fallback_count);
    } else {
        putLine(cGblS, "GblS live%5.1fM (no pool)", latest.sdl_live / MB);
    }
#endif

    // システム空き容量 (iTVPSystemAllocatorInfo 経由)。
    // コンソール機等のプラットフォーム固有実装では正確な空き容量が取得できる。
    // 一般 OS ではシステムの空き物理メモリが近似値として入る。
    // パネル高は kPanelH 側で常に確保済みなので、データ未取得時は "--" を表示。
    const OverlayColor cSysFree{128, 255, 220, 255};
    if (latest.sys_total_free > 0 || latest.sys_allocatable > 0) {
        putLine(cSysFree, "SysFree: %6.1fM  Allocatable: %6.1fM",
                latest.sys_total_free  / MB,
                latest.sys_allocatable / MB);
    } else {
        putLine(cSysFree, "SysFree: --       Allocatable: --");
    }

#ifdef KRKRZ_DRAW_STATS
    //-------- DrawStats ----------
    static TVPDrawThreadStatsSnapshot s_prev_stats = {};
    static bool                      s_stats_inited = false;
    static uint32_t                  s_stats_last_refresh_ms = 0;
    static double s_disp_begin_per_sec = 0.0;
    static int    s_disp_t1_pct = 0;
    static int    s_disp_nt_pct = 0;
    static double s_disp_worker_ms_per_sec = 0.0;
    static double s_disp_main_ms_per_sec   = 0.0;
    static double s_disp_spin_ms_per_sec   = 0.0;
    {
        const uint32_t now_ms = GetTicksMs();
        if (!s_stats_inited) {
            TVPGetDrawThreadStats(s_prev_stats);
            s_stats_last_refresh_ms = now_ms;
            s_stats_inited = true;
        } else if (now_ms - s_stats_last_refresh_ms >= kStatsRefreshMs) {
            TVPDrawThreadStatsSnapshot cur;
            TVPGetDrawThreadStats(cur);
            const uint32_t elapsed = now_ms - s_stats_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_begin = cur.begin_count - s_prev_stats.begin_count;
            tjs_uint64 d_t1    = cur.task_hist[1] - s_prev_stats.task_hist[1];
            tjs_uint64 d_wkr   = cur.worker_active_ns - s_prev_stats.worker_active_ns;
            tjs_uint64 d_main  = cur.main_active_ns - s_prev_stats.main_active_ns;
            tjs_uint64 d_spin  = cur.wait_spin_ns - s_prev_stats.wait_spin_ns;
            s_disp_begin_per_sec = (double)d_begin * per_sec;
            s_disp_t1_pct = (d_begin > 0) ? (int)((d_t1 * 100) / d_begin) : 0;
            s_disp_nt_pct = (d_begin > 0) ? 100 - s_disp_t1_pct : 0;
            s_disp_worker_ms_per_sec = (double)d_wkr / 1.0e6 * per_sec;
            s_disp_main_ms_per_sec   = (double)d_main / 1.0e6 * per_sec;
            s_disp_spin_ms_per_sec   = (double)d_spin / 1.0e6 * per_sec;
            s_prev_stats = cur;
            s_stats_last_refresh_ms = now_ms;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "DrawStats: FPS=%.1f Draw=%.0f/s 1T=%d%% NT=%d%% Wkr=%.1fms/s Main=%.1fms/s Spin=%.1fms/s",
                    s_fps_value, s_disp_begin_per_sec, s_disp_t1_pct, s_disp_nt_pct,
                    s_disp_worker_ms_per_sec, s_disp_main_ms_per_sec, s_disp_spin_ms_per_sec);
                TVPAddLog(ttstr(log_buf));
                // Top 3 callsite (delta)
                TVPDrawCallsiteSnapshot sites[TVPDrawCallsiteMax];
                static TVPDrawCallsiteSnapshot s_prev_sites[TVPDrawCallsiteMax] = {};
                TVPGetDrawCallsiteSnapshots(sites);
                struct SiteDelta { const char *site; tjs_uint64 d_count; tjs_uint64 d_t1; };
                std::vector<SiteDelta> deltas;
                for (int i = 0; i < TVPDrawCallsiteMax; ++i) {
                    if (sites[i].site) {
                        bool found = false;
                        for (int j = 0; j < TVPDrawCallsiteMax; ++j) {
                            if (s_prev_sites[j].site == sites[i].site) {
                                tjs_uint64 dc = sites[i].count - s_prev_sites[j].count;
                                tjs_uint64 dt = sites[i].t1_count - s_prev_sites[j].t1_count;
                                if (dc > 0) deltas.push_back({sites[i].site, dc, dt});
                                found = true;
                                break;
                            }
                        }
                        if (!found && sites[i].count > 0) {
                            deltas.push_back({sites[i].site, sites[i].count, sites[i].t1_count});
                        }
                    }
                }
                std::sort(deltas.begin(), deltas.end(),
                    [](const SiteDelta &a, const SiteDelta &b){ return a.d_count > b.d_count; });
                if (!deltas.empty()) {
                    char site_buf[512];
                    int off = std::snprintf(site_buf, sizeof(site_buf), "DrawSites:");
                    for (size_t i = 0; i < deltas.size() && i < 3 && off < (int)sizeof(site_buf); ++i) {
                        off += std::snprintf(site_buf + off, sizeof(site_buf) - off,
                            " %s=%llu/%llu", TrimSitePath(deltas[i].site),
                            (unsigned long long)deltas[i].d_count,
                            (unsigned long long)deltas[i].d_t1);
                    }
                    TVPAddLog(ttstr(site_buf));
                }
                std::memcpy(s_prev_sites, sites, sizeof(sites));
            }
        }
    }
    const OverlayColor cDraw{220, 180, 255, 255};
    putLine(cDraw, "Draw   %5.0f/s  1T:%2d%% NT:%2d%%",
            s_disp_begin_per_sec, s_disp_t1_pct, s_disp_nt_pct);
    putLine(cDraw, "Wkr:%5.1f Main:%5.1f Spin:%5.1f ms/s",
            s_disp_worker_ms_per_sec, s_disp_main_ms_per_sec, s_disp_spin_ms_per_sec);

    //-------- RenderStats ----------
    static TVPRenderStatsSnapshot s_prev_render = {};
    static bool                   s_render_inited = false;
    static uint32_t               s_render_last_refresh_ms = 0;
    static double s_disp_tex_update_ms_per_sec = 0.0;
    static double s_disp_tex_render_ms_per_sec = 0.0;
    static double s_disp_tex_mb_per_sec        = 0.0;
    {
        const uint32_t now_ms_r = GetTicksMs();
        if (!s_render_inited) {
            TVPGetRenderStats(s_prev_render);
            s_render_last_refresh_ms = now_ms_r;
            s_render_inited = true;
        } else if (now_ms_r - s_render_last_refresh_ms >= kStatsRefreshMs) {
            TVPRenderStatsSnapshot cur_r;
            TVPGetRenderStats(cur_r);
            const uint32_t elapsed = now_ms_r - s_render_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_up = cur_r.tex_update_ns - s_prev_render.tex_update_ns;
            tjs_uint64 d_rn = cur_r.tex_render_ns - s_prev_render.tex_render_ns;
            tjs_uint64 d_by = cur_r.tex_bytes     - s_prev_render.tex_bytes;
            s_disp_tex_update_ms_per_sec = (double)d_up / 1.0e6 * per_sec;
            s_disp_tex_render_ms_per_sec = (double)d_rn / 1.0e6 * per_sec;
            s_disp_tex_mb_per_sec        = (double)d_by / (1024.0 * 1024.0) * per_sec;
            s_prev_render = cur_r;
            s_render_last_refresh_ms = now_ms_r;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "RenderStats: TexUp=%.1fms/s TexRen=%.1fms/s Copy=%.1fMB/s",
                    s_disp_tex_update_ms_per_sec, s_disp_tex_render_ms_per_sec,
                    s_disp_tex_mb_per_sec);
                TVPAddLog(ttstr(log_buf));
            }
        }
    }
    const OverlayColor cTex{255, 200, 120, 255};
    putLine(cTex, "TexUp:%4.0f Ren:%4.0f Copy:%5.0fMB/s",
            s_disp_tex_update_ms_per_sec, s_disp_tex_render_ms_per_sec, s_disp_tex_mb_per_sec);

    //-------- ShowStats ----------
    static double s_disp_show_clear_ms_per_sec   = 0.0;
    static double s_disp_show_tex_ms_per_sec     = 0.0;
    static double s_disp_show_overlay_ms_per_sec = 0.0;
    static double s_disp_show_present_ms_per_sec = 0.0;
    {
        static TVPRenderStatsSnapshot s_prev_show = {};
        static bool                   s_show_inited = false;
        static uint32_t               s_show_last_refresh_ms = 0;
        const uint32_t now_ms_s = GetTicksMs();
        if (!s_show_inited) {
            TVPGetRenderStats(s_prev_show);
            s_show_last_refresh_ms = now_ms_s;
            s_show_inited = true;
        } else if (now_ms_s - s_show_last_refresh_ms >= kStatsRefreshMs) {
            TVPRenderStatsSnapshot cur_s;
            TVPGetRenderStats(cur_s);
            const uint32_t elapsed = now_ms_s - s_show_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_clr = cur_s.show_clear_ns   - s_prev_show.show_clear_ns;
            tjs_uint64 d_tx  = cur_s.show_tex_ns     - s_prev_show.show_tex_ns;
            tjs_uint64 d_ov  = cur_s.show_overlay_ns - s_prev_show.show_overlay_ns;
            tjs_uint64 d_pr  = cur_s.show_present_ns - s_prev_show.show_present_ns;
            s_disp_show_clear_ms_per_sec   = (double)d_clr / 1.0e6 * per_sec;
            s_disp_show_tex_ms_per_sec     = (double)d_tx  / 1.0e6 * per_sec;
            s_disp_show_overlay_ms_per_sec = (double)d_ov  / 1.0e6 * per_sec;
            s_disp_show_present_ms_per_sec = (double)d_pr  / 1.0e6 * per_sec;
            s_prev_show = cur_s;
            s_show_last_refresh_ms = now_ms_s;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "ShowStats: Clear=%.1fms/s Tex=%.1fms/s Overlay=%.1fms/s Present=%.1fms/s",
                    s_disp_show_clear_ms_per_sec, s_disp_show_tex_ms_per_sec,
                    s_disp_show_overlay_ms_per_sec, s_disp_show_present_ms_per_sec);
                TVPAddLog(ttstr(log_buf));
            }
        }
    }
    const OverlayColor cShow{180, 220, 255, 255};
    putLine(cShow, "Show Clr:%4.0f Tex:%4.0f Ovl:%4.0f Pres:%4.0f",
            s_disp_show_clear_ms_per_sec, s_disp_show_tex_ms_per_sec,
            s_disp_show_overlay_ms_per_sec, s_disp_show_present_ms_per_sec);

    //-------- FrameStats ----------
    static double s_disp_frame_update_ms_per_sec   = 0.0;
    static double s_disp_frame_show_ms_per_sec     = 0.0;
    static double s_disp_frame_dispatch_ms_per_sec = 0.0;
    {
        static TVPRenderStatsSnapshot s_prev_frame = {};
        static bool                   s_frame_inited = false;
        static uint32_t               s_frame_last_refresh_ms = 0;
        const uint32_t now_ms_f = GetTicksMs();
        if (!s_frame_inited) {
            TVPGetRenderStats(s_prev_frame);
            s_frame_last_refresh_ms = now_ms_f;
            s_frame_inited = true;
        } else if (now_ms_f - s_frame_last_refresh_ms >= kStatsRefreshMs) {
            TVPRenderStatsSnapshot cur_f;
            TVPGetRenderStats(cur_f);
            const uint32_t elapsed = now_ms_f - s_frame_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_up = cur_f.frame_update_ns   - s_prev_frame.frame_update_ns;
            tjs_uint64 d_sh = cur_f.frame_show_ns     - s_prev_frame.frame_show_ns;
            tjs_uint64 d_ds = cur_f.frame_dispatch_ns - s_prev_frame.frame_dispatch_ns;
            s_disp_frame_update_ms_per_sec   = (double)d_up / 1.0e6 * per_sec;
            s_disp_frame_show_ms_per_sec     = (double)d_sh / 1.0e6 * per_sec;
            s_disp_frame_dispatch_ms_per_sec = (double)d_ds / 1.0e6 * per_sec;
            s_prev_frame = cur_f;
            s_frame_last_refresh_ms = now_ms_f;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "FrameStats: Update=%.1fms/s Show=%.1fms/s Dispatch=%.1fms/s",
                    s_disp_frame_update_ms_per_sec, s_disp_frame_show_ms_per_sec,
                    s_disp_frame_dispatch_ms_per_sec);
                TVPAddLog(ttstr(log_buf));
            }
        }
    }
    const OverlayColor cFrame{200, 255, 200, 255};
    putLine(cFrame, "Frame Up:%4.0f Sho:%4.0f Dsp:%4.0f",
            s_disp_frame_update_ms_per_sec, s_disp_frame_show_ms_per_sec,
            s_disp_frame_dispatch_ms_per_sec);

    //-------- LayerStats ----------
    static double s_disp_layer_complete_window_ms_per_sec = 0.0;
    static double s_disp_layer_complete_ms_per_sec        = 0.0;
    static double s_disp_layer_draw_ms_per_sec            = 0.0;
    {
        static TVPRenderStatsSnapshot s_prev_layer = {};
        static bool                   s_layer_inited = false;
        static uint32_t               s_layer_last_refresh_ms = 0;
        const uint32_t now_ms_l = GetTicksMs();
        if (!s_layer_inited) {
            TVPGetRenderStats(s_prev_layer);
            s_layer_last_refresh_ms = now_ms_l;
            s_layer_inited = true;
        } else if (now_ms_l - s_layer_last_refresh_ms >= kStatsRefreshMs) {
            TVPRenderStatsSnapshot cur_l;
            TVPGetRenderStats(cur_l);
            const uint32_t elapsed = now_ms_l - s_layer_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_cw = cur_l.layer_complete_window_ns - s_prev_layer.layer_complete_window_ns;
            tjs_uint64 d_cm = cur_l.layer_complete_ns        - s_prev_layer.layer_complete_ns;
            tjs_uint64 d_dr = cur_l.layer_draw_ns            - s_prev_layer.layer_draw_ns;
            s_disp_layer_complete_window_ms_per_sec = (double)d_cw / 1.0e6 * per_sec;
            s_disp_layer_complete_ms_per_sec        = (double)d_cm / 1.0e6 * per_sec;
            s_disp_layer_draw_ms_per_sec            = (double)d_dr / 1.0e6 * per_sec;
            s_prev_layer = cur_l;
            s_layer_last_refresh_ms = now_ms_l;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "LayerStats: CompleteW=%.1fms/s Complete=%.1fms/s Draw=%.1fms/s",
                    s_disp_layer_complete_window_ms_per_sec,
                    s_disp_layer_complete_ms_per_sec, s_disp_layer_draw_ms_per_sec);
                TVPAddLog(ttstr(log_buf));
            }
        }
    }
    const OverlayColor cLayer{220, 200, 255, 255};
    putLine(cLayer, "Layer CmpW:%4.0f Cmp:%4.0f Drw:%4.0f",
            s_disp_layer_complete_window_ms_per_sec,
            s_disp_layer_complete_ms_per_sec, s_disp_layer_draw_ms_per_sec);

    //-------- LayerExStats ----------
    static double s_disp_layer_before_ms_per_sec = 0.0;
    static double s_disp_layer_after_ms_per_sec  = 0.0;
    {
        static TVPRenderStatsSnapshot s_prev_ba = {};
        static bool                   s_ba_inited = false;
        static uint32_t               s_ba_last_refresh_ms = 0;
        const uint32_t now_ms_ba = GetTicksMs();
        if (!s_ba_inited) {
            TVPGetRenderStats(s_prev_ba);
            s_ba_last_refresh_ms = now_ms_ba;
            s_ba_inited = true;
        } else if (now_ms_ba - s_ba_last_refresh_ms >= kStatsRefreshMs) {
            TVPRenderStatsSnapshot cur_ba;
            TVPGetRenderStats(cur_ba);
            const uint32_t elapsed = now_ms_ba - s_ba_last_refresh_ms;
            const double per_sec = elapsed > 0 ? (1000.0 / (double)elapsed) : 0.0;
            tjs_uint64 d_b = cur_ba.layer_before_completion_ns - s_prev_ba.layer_before_completion_ns;
            tjs_uint64 d_a = cur_ba.layer_after_completion_ns  - s_prev_ba.layer_after_completion_ns;
            s_disp_layer_before_ms_per_sec = (double)d_b / 1.0e6 * per_sec;
            s_disp_layer_after_ms_per_sec  = (double)d_a / 1.0e6 * per_sec;
            s_prev_ba = cur_ba;
            s_ba_last_refresh_ms = now_ms_ba;
            if (TVPDrawStatsLogEnabled) {
                char log_buf[256];
                std::snprintf(log_buf, sizeof(log_buf),
                    "LayerExStats: Before=%.1fms/s After=%.1fms/s",
                    s_disp_layer_before_ms_per_sec, s_disp_layer_after_ms_per_sec);
                TVPAddLog(ttstr(log_buf));
            }
        }
    }
    const OverlayColor cLayerEx{200, 180, 240, 255};
    putLine(cLayerEx, "LayerEx Bef:%4.0f Aft:%4.0f",
            s_disp_layer_before_ms_per_sec, s_disp_layer_after_ms_per_sec);
#endif

    // グラフ
    const int graph_x = px + 6;
    const int graph_y = py + kHeaderH;
    const int graph_w = kPanelW - 12;
    const int graph_h = kPanelH - kHeaderH - 6;
    DrawSeriesLineGL(shape_batch, samples, &ExtractFile,   cFile, max_bytes, graph_x, graph_y, graph_w, graph_h);
    DrawSeriesLineGL(shape_batch, samples, &ExtractBitmap, cBmap, max_bytes, graph_x, graph_y, graph_w, graph_h);
    DrawSeriesLineGL(shape_batch, samples, &ExtractSound,  cSnd,  max_bytes, graph_x, graph_y, graph_w, graph_h);
    DrawSeriesLineGL(shape_batch, samples, &ExtractRSS,    cRSS,  max_bytes, graph_x, graph_y, graph_w, graph_h);

    // 描画 (shape は texture なし、text は font texture)
    FlushBatch(shape_batch, false);
    FlushBatch(text_batch,  true);
}

#else // !KRKRZ_ENABLE_MEMORY_OVERLAY

// OFF 時: OGLDrawDevice / SDLOGLDrawDevice から呼ばれても何もしない stub。
void TVPRenderMemoryOverlayGL() {}

#endif // KRKRZ_ENABLE_MEMORY_OVERLAY
