// PadOverlay の OpenGL ES 直接版描画。SDLOGLDrawDevice (SDL_Renderer 不在経路)
// と OGLDrawDevice (Canvas/Texture フル機能版) の両方の Show 末尾から呼べる。
// SDL 依存は無し。パッド状態は Application->GetPadState(0) / HasJoypad(0) /
// GetJoypadType(0) 経由で抽象化済 API から取得する。
//
// 描画レイアウト・色は SDL_Renderer 版 (sdl3/visual/PadOverlayRender.cpp) に
// 準拠。font/shader/VBO 基盤は MemoryOverlayGL.cpp と同等のコードを内蔵
// (lazy init、複数回呼出で同じ resource を再利用)。

#include "tjsCommHead.h"
#include "PadOverlayGL.h"
#include "PadOverlay.h"
#include "Application.h"
#include "CharacterSet.h"
#include "LogIntf.h"

#include "OpenGLHeader.h"
#include "GLShaderUtil.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

namespace {

struct OverlayColor {
    uint8_t r, g, b, a;
};

//===========================================================================
// 8x8 bitmap font (font8x8_basic, public domain)。MemoryOverlayGL.cpp と同じ。
// ASCII 0x20-0x7E、各 8 行、各バイトは 1 行 (LSB が左端)。
//===========================================================================
const uint8_t kFont8x8[95][8] = {
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
// GL state (lazy init)
//===========================================================================
struct GLOverlayState {
    bool   initialized = false;
    GLuint program     = 0;
    GLuint vbo         = 0;
    GLuint font_tex    = 0;
    GLint  loc_screenSize = -1;
    GLint  loc_useTex     = -1;
    GLint  loc_tex        = -1;
    GLint  attr_pos       = -1;
    GLint  attr_uv        = -1;
    GLint  attr_color     = -1;
};

GLOverlayState g_gl;

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
        TVPLOG_ERROR("PadOverlayGL: shader compile failed");
        return false;
    }
    g_gl.loc_screenSize = glGetUniformLocation(g_gl.program, "u_screenSize");
    g_gl.loc_useTex     = glGetUniformLocation(g_gl.program, "u_useTex");
    g_gl.loc_tex        = glGetUniformLocation(g_gl.program, "u_tex");
    g_gl.attr_pos       = glGetAttribLocation (g_gl.program, "a_pos");
    g_gl.attr_uv        = glGetAttribLocation (g_gl.program, "a_uv");
    g_gl.attr_color     = glGetAttribLocation (g_gl.program, "a_color");

    glGenBuffers(1, &g_gl.vbo);

    // Font atlas 128x48 (R8 1 byte/pixel)
    constexpr int kAtlasCols = 16;
    constexpr int kAtlasRows = 6;
    constexpr int kAtlasW = kAtlasCols * kFontCharW;
    constexpr int kAtlasH = kAtlasRows * kFontCharH;
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

struct Batch {
    std::vector<Vertex> verts;
    int   viewport_w = 0;
    int   viewport_h = 0;
    float scale      = 1.0f;

    void Reset(int vw, int vh, float s) {
        verts.clear();
        viewport_w = vw;
        viewport_h = vh;
        scale = s;
    }
};

void AddFilledRect(Batch &b, float x, float y, float w, float h, OverlayColor c) {
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

void AddRectOutline(Batch &b, float x, float y, float w, float h, OverlayColor c) {
    AddFilledRect(b, x,         y,         w, 1, c);
    AddFilledRect(b, x,         y + h - 1, w, 1, c);
    AddFilledRect(b, x,         y,         1, h, c);
    AddFilledRect(b, x + w - 1, y,         1, h, c);
}

void AddText(Batch &b, float x, float y, const char *s, OverlayColor c) {
    constexpr int kAtlasCols = 16;
    constexpr int kAtlasRows = 6;
    constexpr float kAtlasW = (float)(kAtlasCols * kFontCharW);
    constexpr float kAtlasH = (float)(kAtlasRows * kFontCharH);
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

//===========================================================================
// レイアウト定数 (SDL_Renderer 版と同じ)
//===========================================================================
constexpr float kOverlayScale = 1.5f;
constexpr int kPanelMargin = 8;
constexpr int kPanelPad    = 6;
constexpr int kHeaderH     = 12;
constexpr int kCellW       = 32;
constexpr int kCellH       = 16;
constexpr int kCellGap     = 4;
constexpr int kMatrixW     = 4 * kCellW + 3 * kCellGap;
constexpr int kMatrixH     = 4 * kCellH + 3 * kCellGap;
// 軸表示: 3 行 (LX/LY, RX/RY, LT/RT)、各 12 px (font 8 + 行間 4)
constexpr int kAxisLineH   = 12;
constexpr int kAxesRows    = 3;
constexpr int kAxesH       = kAxisLineH * kAxesRows;
constexpr int kPanelW      = kMatrixW + kPanelPad * 2;
constexpr int kPanelH      = kHeaderH + 4 + kMatrixH + 4 + kAxesH + kPanelPad * 2;

const char *kLabels[16] = {
    "A",  "B",  "X",  "Y",
    "L1", "R1", "L2", "R2",
    "BK", "ST", "LS", "RS",
    "Lf", "Up", "Rt", "Dn",
};

//===========================================================================
// パッド状態クエリ。SDL3 build (generic/environ/Application.h) は tTVPApplication
// に HasJoypad/GetPadState/GetJoypadType を持つ。WINVER (win32/environ/
// Application.h) は持たないので stub。
//===========================================================================
#ifdef __WINVER__
bool QueryHasPad() { return false; }
tjs_uint32 QueryPadState() { return 0; }
std::string QueryPadName() { return ""; }
void QueryPadAxes(float (&out)[6]) { for (int i = 0; i < 6; ++i) out[i] = 0.0f; }
#else
bool QueryHasPad() {
    return Application && Application->HasJoypad(0);
}
tjs_uint32 QueryPadState() {
    return QueryHasPad() ? Application->GetPadState(0) : 0;
}
std::string QueryPadName() {
    if (!QueryHasPad()) return "";
    std::string name_u8;
    TVPUtf16ToUtf8(name_u8, Application->GetJoypadType(0));
    return name_u8;
}
void QueryPadAxes(float (&out)[6]) {
    const bool has = QueryHasPad();
    for (int i = 0; i < 6; ++i) {
        out[i] = has ? Application->GetPadAxis(0, i) : 0.0f;
    }
}
#endif

} // anonymous namespace

void TVPRenderPadOverlayGL()
{
    if (!TVPPadOverlay::IsEnabled()) return;
    if (!InitGL()) return;

    // 現 viewport を取得 (Show 末尾呼出時に画面サイズで設定されている前提だが
    // 念のため glGetIntegerv で確認)
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    const int win_w = vp[2];
    const int win_h = vp[3];
    const int scaled_win_w = (int)((float)win_w / kOverlayScale);
    const int scaled_win_h = (int)((float)win_h / kOverlayScale);
    if (scaled_win_w < kPanelW + kPanelMargin * 2) return;
    if (scaled_win_h < kPanelH + kPanelMargin * 2) return;

    // パッド状態取得 (SDL_Renderer 版と同じ source of truth、WINVER は stub)
    const bool has_pad = QueryHasPad();
    const tjs_uint32 state = QueryPadState();
    float axes[6];
    QueryPadAxes(axes);

    Batch shape_batch;
    Batch text_batch;
    shape_batch.Reset(win_w, win_h, kOverlayScale);
    text_batch .Reset(win_w, win_h, kOverlayScale);

    // memoverlay (右上) と被らないよう左上配置
    const int px = kPanelMargin;
    const int py = kPanelMargin;

    // 背景 + 枠
    AddFilledRect(shape_batch, (float)px, (float)py, (float)kPanelW, (float)kPanelH,
                  OverlayColor{0, 0, 0, 180});
    AddRectOutline(shape_batch, (float)px, (float)py, (float)kPanelW, (float)kPanelH,
                   OverlayColor{96, 96, 96, 255});

    // ヘッダ
    char header[80];
    if (has_pad) {
        std::string name_u8 = QueryPadName();
        std::snprintf(header, sizeof(header), "Pad: %s",
            !name_u8.empty() ? name_u8.c_str() : "(unknown)");
    } else {
        std::snprintf(header, sizeof(header), "Pad: (none)");
    }
    AddText(text_batch, (float)(px + kPanelPad), (float)(py + kPanelPad),
            header, OverlayColor{255, 255, 255, 255});

    // 4x4 ボタンマトリクス
    const int mx = px + kPanelPad;
    const int my = py + kPanelPad + kHeaderH + 4;
    for (int i = 0; i < 16; ++i) {
        const int col = i % 4;
        const int row = i / 4;
        const int cx = mx + col * (kCellW + kCellGap);
        const int cy = my + row * (kCellH + kCellGap);
        const bool on = ((state >> i) & 1) != 0;

        OverlayColor fill = on
            ? OverlayColor{80, 200, 80, 220}
            : OverlayColor{48, 48, 48, 200};
        AddFilledRect(shape_batch, (float)cx, (float)cy,
                      (float)kCellW, (float)kCellH, fill);
        AddRectOutline(shape_batch, (float)cx, (float)cy,
                       (float)kCellW, (float)kCellH,
                       OverlayColor{128, 128, 128, 255});

        const char *label = kLabels[i];
        int label_len = 0;
        for (const char *p = label; *p; ++p) ++label_len;
        const int label_px_w = label_len * 8;
        const float tx = (float)(cx + (kCellW - label_px_w) / 2);
        const float ty = (float)(cy + (kCellH - 8) / 2);
        OverlayColor text_color = on
            ? OverlayColor{0, 0, 0, 255}
            : OverlayColor{200, 200, 200, 255};
        AddText(text_batch, tx, ty, label, text_color);
    }

    // 軸値: ボタンマトリクス直下に 3 行 × 2 列
    // "LX +0.45 LY -0.32" 形 (符号付き 5 char、幅 17 char = 136 px、マトリクス内)
    const char *axis_labels[6] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    const int ax = mx;
    const int ay0 = my + kMatrixH + 4;
    const OverlayColor axis_color = has_pad
        ? OverlayColor{220, 220, 220, 255}
        : OverlayColor{128, 128, 128, 255};
    for (int row = 0; row < kAxesRows; ++row) {
        const int aL = row * 2;
        const int aR = row * 2 + 1;
        char line[40];
        std::snprintf(line, sizeof(line), "%s %+.2f %s %+.2f",
            axis_labels[aL], axes[aL],
            axis_labels[aR], axes[aR]);
        AddText(text_batch,
            (float)ax,
            (float)(ay0 + row * kAxisLineH),
            line, axis_color);
    }

    FlushBatch(shape_batch, false);
    FlushBatch(text_batch,  true);
}
