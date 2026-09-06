#include "tjsCommHead.h"
#include "LogIntf.h"
#include "GLTexture.h"

#include <memory>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <atomic>

extern int TVPOpenGLESVersion;

// ---------------------------------------------------------------------------
// テクスチャメモリ計測
//
// GL ドライバ内部の実確保量ではなく「こちらから要求したバイト数」の積算。
// GL ドライバが通常ヒープから確保する環境 (一部のコンソール機) では、
// この値がそのままヒープ圧迫量の目安になる。
// ---------------------------------------------------------------------------
namespace {
std::atomic<std::uint64_t> s_gltex_bytes{0};
std::atomic<std::uint64_t> s_glpbo_bytes{0};
std::atomic<std::uint64_t> s_glfbo_bytes{0};
std::atomic<std::uint64_t> s_gltex_peak{0};
std::atomic<std::uint32_t> s_gltex_count{0};
std::atomic<std::uint32_t> s_glpbo_count{0};
std::atomic<std::uint32_t> s_glfbo_count{0};
//! 最後にログへ出した合計値 (これとの差が閾値を超えたら再度出す)
std::atomic<std::uint64_t> s_gltex_logged{0};

constexpr double kMiB = 1024.0 * 1024.0;

//! ピーク更新と、 一定量変化したときのログ出力
void TVPNoteGLTextureMemory()
{
    const std::uint64_t tex   = s_gltex_bytes.load(std::memory_order_relaxed);
    const std::uint64_t pbo   = s_glpbo_bytes.load(std::memory_order_relaxed);
    const std::uint64_t fbo   = s_glfbo_bytes.load(std::memory_order_relaxed);
    const std::uint64_t total = tex + pbo + fbo;

    std::uint64_t peak = s_gltex_peak.load(std::memory_order_relaxed);
    while (total > peak &&
           !s_gltex_peak.compare_exchange_weak(peak, total, std::memory_order_relaxed)) {
        // peak は compare_exchange_weak が最新値へ更新する
    }

    if (!GLTexture::MemLogEnabled) return;

    const std::uint64_t logged = s_gltex_logged.load(std::memory_order_relaxed);
    const std::uint64_t diff = (total > logged) ? (total - logged) : (logged - total);
    if (diff < GLTexture::MemLogStepBytes) return;
    s_gltex_logged.store(total, std::memory_order_relaxed);

    TVPLOG_INFO("GLTexMem: total={:.1f}MiB (tex={:.1f}MiB n={} / pbo={:.1f}MiB n={} / fbo={:.1f}MiB n={}) peak={:.1f}MiB",
                total / kMiB,
                tex / kMiB, s_gltex_count.load(std::memory_order_relaxed),
                pbo / kMiB, s_glpbo_count.load(std::memory_order_relaxed),
                fbo / kMiB, s_glfbo_count.load(std::memory_order_relaxed),
                s_gltex_peak.load(std::memory_order_relaxed) / kMiB);
}
} // namespace

// 調査用ログ。 既定 OFF。 TJS の System.setTextureMemoryLog(true) で有効化する
// (NX の GfxMem ログもこのフラグに連動する)。
bool GLTexture::MemLogEnabled = false;

void
GLTexture::GetMemStats(MemStats &out)
{
    out.texture_bytes    = s_gltex_bytes.load(std::memory_order_relaxed);
    out.pbo_bytes        = s_glpbo_bytes.load(std::memory_order_relaxed);
    out.fbo_bytes        = s_glfbo_bytes.load(std::memory_order_relaxed);
    out.total_bytes      = out.texture_bytes + out.pbo_bytes + out.fbo_bytes;
    out.peak_total_bytes = s_gltex_peak.load(std::memory_order_relaxed);
    out.texture_count    = s_gltex_count.load(std::memory_order_relaxed);
    out.pbo_count        = s_glpbo_count.load(std::memory_order_relaxed);
    out.fbo_count        = s_glfbo_count.load(std::memory_order_relaxed);
}

void
GLTexture::ResetMemPeak()
{
    const std::uint64_t total = s_gltex_bytes.load(std::memory_order_relaxed) +
                                s_glpbo_bytes.load(std::memory_order_relaxed) +
                                s_glfbo_bytes.load(std::memory_order_relaxed);
    s_gltex_peak.store(total, std::memory_order_relaxed);
}

void
GLTexture::NoteFboMemory(std::int64_t bytes_delta, std::int32_t count_delta)
{
    if (bytes_delta > 0) {
        s_glfbo_bytes.fetch_add((std::uint64_t)bytes_delta, std::memory_order_relaxed);
    } else if (bytes_delta < 0) {
        s_glfbo_bytes.fetch_sub((std::uint64_t)(-bytes_delta), std::memory_order_relaxed);
    }
    if (count_delta > 0) {
        s_glfbo_count.fetch_add((std::uint32_t)count_delta, std::memory_order_relaxed);
    } else if (count_delta < 0) {
        s_glfbo_count.fetch_sub((std::uint32_t)(-count_delta), std::memory_order_relaxed);
    }
    TVPNoteGLTextureMemory();
}

void
GLTexture::addMemStats(std::uint64_t tex_bytes, std::uint64_t pbo_bytes)
{
    // create() の呼び直しで二重計上しないよう、 既存分は一度戻す
    if (tex_bytes_ || pbo_bytes_) subMemStats();

    tex_bytes_ = tex_bytes;
    pbo_bytes_ = pbo_bytes;
    if (tex_bytes_) {
        s_gltex_bytes.fetch_add(tex_bytes_, std::memory_order_relaxed);
        s_gltex_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (pbo_bytes_) {
        s_glpbo_bytes.fetch_add(pbo_bytes_, std::memory_order_relaxed);
        s_glpbo_count.fetch_add(1, std::memory_order_relaxed);
    }
    TVPNoteGLTextureMemory();
}

void
GLTexture::subMemStats()
{
    if (tex_bytes_) {
        s_gltex_bytes.fetch_sub(tex_bytes_, std::memory_order_relaxed);
        s_gltex_count.fetch_sub(1, std::memory_order_relaxed);
        tex_bytes_ = 0;
    }
    if (pbo_bytes_) {
        s_glpbo_bytes.fetch_sub(pbo_bytes_, std::memory_order_relaxed);
        s_glpbo_count.fetch_sub(1, std::memory_order_relaxed);
        pbo_bytes_ = 0;
    }
    TVPNoteGLTextureMemory();
}

// ---------------------------------------------------------------------------
// TJS / オーバレイ向けアクセサ
// GL のヘッダを include できない翻訳単位からも呼べるよう、 自由関数で公開する
// (呼び出し側は extern 宣言だけで使える)。
// ---------------------------------------------------------------------------
void TVPGetGLTextureMemory(tjs_uint64 *texture_bytes, tjs_uint64 *pbo_bytes,
                           tjs_uint64 *peak_bytes,
                           tjs_uint32 *texture_count, tjs_uint32 *pbo_count)
{
    GLTexture::MemStats st;
    GLTexture::GetMemStats(st);
    if (texture_bytes) *texture_bytes = st.texture_bytes;
    if (pbo_bytes)     *pbo_bytes     = st.pbo_bytes;
    if (peak_bytes)    *peak_bytes    = st.peak_total_bytes;
    if (texture_count) *texture_count = st.texture_count;
    if (pbo_count)     *pbo_count     = st.pbo_count;
}

void TVPSetGLTextureMemoryLog(bool enable)
{
    GLTexture::MemLogEnabled = enable;
}

bool TVPGetGLTextureMemoryLogEnabled()
{
    return GLTexture::MemLogEnabled;
}

void TVPResetGLTextureMemoryPeak()
{
    GLTexture::ResetMemPeak();
}

//! 現在値を 1 行ログへ出す (シーン境界等での目印用)
void TVPLogGLTextureMemory(const char *tag)
{
    GLTexture::MemStats st;
    GLTexture::GetMemStats(st);
    TVPLOG_INFO("GLTexMem[{}]: total={:.1f}MiB (tex={:.1f}MiB n={} / pbo={:.1f}MiB n={}) peak={:.1f}MiB",
                tag ? tag : "",
                st.total_bytes / kMiB,
                st.texture_bytes / kMiB, st.texture_count,
                st.pbo_bytes / kMiB, st.pbo_count,
                st.peak_total_bytes / kMiB);
}

GLuint
GLTexture::ensurePBO()
{
    if (pbo_) return pbo_;
    if (texture_id_ == 0) return 0;
    if (format_ == tTVPTextureColorFormat::Alpha) return 0; // α テクスチャは PBO 経路を使わない

    const std::size_t size = (std::size_t)width_ * height_ * 4;
    if (size == 0) return 0;

    glGenBuffers(1, &pbo_);
    if (pbo_ == 0) return 0;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, size, 0, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    CheckGLErrorAndLog("glBufferData(PBO)");

    pbo_bytes_ = size;
    s_glpbo_bytes.fetch_add(pbo_bytes_, std::memory_order_relaxed);
    s_glpbo_count.fetch_add(1, std::memory_order_relaxed);
    TVPNoteGLTextureMemory();
    return pbo_;
}

void 
GLTexture::create( GLuint w, GLuint h, const GLvoid* bits, tTVPTextureColorFormat format) 
{
    int pixel_size;
    GLuint fmt;
    
    if (format == tTVPTextureColorFormat::Alpha) {
        pixel_size = 1;
        glformat_ = GL_ALPHA;
        fmt = GL_R8;
    } else {
        pixel_size = 4;
        if (GLTexture::SupportBGRAFormat()) {
            glformat_ = GL_BGRA_EXT;
            fmt = GL_BGRA8_EXT;
        } else {
            glformat_ = GL_RGBA;
            fmt = GL_RGBA8;
        }
    }

    format_ = format;
    width_ = w;
    height_ = h;

    glPixelStorei( GL_UNPACK_ALIGNMENT, pixel_size);
    glGenTextures( 1, &texture_id_ );
    glBindTexture( GL_TEXTURE_2D, texture_id_ );

    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,stretchType_);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,stretchType_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT_);

    if (format == tTVPTextureColorFormat::Alpha) {
		glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, bits );
		glBindTexture( GL_TEXTURE_2D, 0 );
        addMemStats((std::uint64_t)w * h * pixel_size, 0);
        return;
    }

    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    CheckGLErrorAndLog("glTexStorage2D");

    // 初期アップロードは直接転送で行う。
    // PBO は「更新されるテクスチャ」だけが必要とするので、 実際に
    // UpdateTexture() が呼ばれた時点で ensurePBO() が確保する (遅延確保)。
    // ここで常に確保していた頃は、 静的テクスチャもテクスチャ実体と同サイズの
    // PBO を持ち続けてしまい、 GPU メモリを丸ごと 2 倍消費していた。
    if (bits) {
        //glTexImage2D( GL_TEXTURE_2D, 0, fmt, w, h, 0, format, GL_UNSIGNED_BYTE, bits );
        //CheckGLErrorAndLog("glTexImage2D");
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, glformat_, GL_UNSIGNED_BYTE, bits);
        CheckGLErrorAndLog("glTexSubImage2D");
    }

    addMemStats((std::uint64_t)w * h * pixel_size, 0);

    if (glformat_ == GL_RGBA && _support_swizzle) {
        // スウィズルで R と B を入れ替える 
        TVPLOG_DEBUG("GLES: Create Texture && Set Swizzle");
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }

    glBindTexture( GL_TEXTURE_2D, 0 );
}

void
GLTexture::createMipmapTexture( std::vector<GLTextreImageSet>& img )
{
    if( img.size() > 0 ) {
        GLuint w = img[0].width;
        GLuint h = img[0].height;
        glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
        glGenTextures( 1, &texture_id_ );
        glBindTexture( GL_TEXTURE_2D, texture_id_ );

        GLint count = img.size();
        if( count > 1 ) hasMipmap_ = true;

        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, stretchType_ );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, getMipmapFilter( stretchType_ ) );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS_ );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT_ );
        // ミップマップの最小と最大レベルを指定する、これがないと存在しないレベルを参照しようとすることが発生しうる
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
        glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, count - 1 );

        // C: 入力 bitmap は BGRA byte order (DIB 内部表現と同じ) を期待する。
        //    BGRA_EXT サポートあり → そのまま GL_BGRA_EXT で upload
        //    swizzle あり → GL_RGBA で upload + sampling 時に R/B swizzle
        //    両方無し → fallback として GL_RGBA で upload (色が反転して見えるが
        //               この呼び出しは glmNormalRGBA 系の経路では使われない前提)
        GLenum uploadFormat;
        GLint internalFormat;
        if( SupportBGRAFormat() ) {
            uploadFormat  = GL_BGRA_EXT;
            internalFormat = GL_BGRA_EXT;
        } else {
            uploadFormat  = GL_RGBA;
            internalFormat = GL_RGBA;
        }

        if( TVPOpenGLESVersion < 300 ) {
            // OpenGL ES2.0 の時は、glGenerateMipmap しないと正しくミップマップ描画できない模様
            GLTextreImageSet& tex = img[0];
            glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, tex.width, tex.height, 0, uploadFormat, GL_UNSIGNED_BYTE, tex.bits );
            glHint( GL_GENERATE_MIPMAP_HINT, GL_FASTEST );
            glGenerateMipmap( GL_TEXTURE_2D );
            // 自前で生成したものに一部置き換える
            for( GLint i = 1; i < count; i++ ) {
                GLTextreImageSet& tex = img[i];
                glTexSubImage2D( GL_TEXTURE_2D, i, 0, 0, tex.width, tex.height, uploadFormat, GL_UNSIGNED_BYTE, tex.bits );
            }
        } else {
            for( GLint i = 0; i < count; i++ ) {
                GLTextreImageSet& tex = img[i];
                glTexImage2D( GL_TEXTURE_2D, i, internalFormat, tex.width, tex.height, 0, uploadFormat, GL_UNSIGNED_BYTE, tex.bits );
            }
        }

        // BGRA_EXT 不可 + swizzle 可なら sampling 時に R/B 入れ替え
        if( uploadFormat == GL_RGBA && _support_swizzle ) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }

        glBindTexture( GL_TEXTURE_2D, 0 );
        format_ = tTVPTextureColorFormat::RGBA;
        glformat_ = uploadFormat;
        width_ = w;
        height_ = h;

        // 統計: 全ミップレベル分の合計
        std::uint64_t bytes = 0;
        for( std::size_t i = 0; i < img.size(); i++ ) {
            bytes += (std::uint64_t)img[i].width * img[i].height * 4;
        }
        addMemStats(bytes, 0);
    }
}

void
GLTexture::destory()
{
    if( texture_id_ != 0 ) {
        glDeleteTextures( 1, &texture_id_ );
        texture_id_ = 0;
        hasMipmap_ = false;
    }
    if (pbo_) {
        glDeleteBuffers(1, &pbo_);
        pbo_ = 0;
    }
    subMemStats();
}


void 
GLTexture::UpdateTexture(GLuint tex_id, GLuint pbo, int format, int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator)
{
    int size = w*h*4;
    int pitch = w*4;

    if (pbo && GLTexture::UsePBOForUpload((std::size_t)size)) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        // GL_MAP_INVALIDATE_BUFFER_BIT で「以前の PBO 内容は破棄して良い」を driver に伝える。
        // これにより GPU が前 frame の PBO を読み中でも CPU map が wait せず、
        // driver が orphan して新しい backing store を割り当てる。
        // PBO single buffer のままで multi-buffer 相当の効果が得られる (Phase C-α、
        // 2026-05-10 SDLOGLDrawDevice 計測で idle frame TexUp=240 ms/s と CPU map wait
        // らしき症状が見られたための対策)。
        GLubyte *texPixels = (GLubyte *)glMapBufferRange(
            GL_PIXEL_UNPACK_BUFFER, 0, size,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (texPixels) {
            updator((char*)texPixels, pitch);
            glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        }

        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, 0);
        CheckGLErrorAndLog("glTexSubImage2D");
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    } else {

        // 中間バッファは使い回す (毎フレーム数 MB の new/delete を避ける)。
        // 転送は GL 呼び出し側 = 描画スレッドに限られるので thread_local で足りる。
        static thread_local std::vector<char> buffer;
        if ((int)buffer.size() < size) buffer.resize(size);
        updator(buffer.data(), pitch);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D( GL_TEXTURE_2D, 0, x, y, w, h, format, GL_UNSIGNED_BYTE, buffer.data());
        CheckGLErrorAndLog("glTexSubImage2D");
        glBindTexture(GL_TEXTURE_2D, 0);
    }
};

//---------------------------------------------------------------------------
// 転送経路の強制指定 (-1 = 既定、 1 = PBO、 0 = 直接)。
// 既定は用途で違う (GLTexture.h のコメント参照)。
namespace {
    int g_upload_override = -2;   // -2 = 未初期化 (環境変数をまだ読んでいない)
}

void
GLTexture::SetUploadOverride(int v)
{
    g_upload_override = (v == 0 || v == 1) ? v : -1;
}

int
GLTexture::GetUploadOverride()
{
    if (g_upload_override == -2) {
        const char * e = std::getenv("KRKRZ_GLTEXUP");
        if (e && std::strcmp(e, "pbo") == 0)          g_upload_override = 1;
        else if (e && std::strcmp(e, "direct") == 0)  g_upload_override = 0;
        else                                          g_upload_override = -1;
    }
    return g_upload_override;
}

bool
GLTexture::IsANGLE()
{
    static int cached = -1;
    if (cached < 0) {
        const char * r = (const char *)glGetString(GL_RENDERER);
        cached = (r && std::strstr(r, "ANGLE")) ? 1 : 0;
    }
    return cached != 0;
}

bool
GLTexture::UsePBOForUpload(std::size_t bytes)
{
    const int o = GetUploadOverride();
    if (o >= 0) return o == 1;          // 計測用の強制指定
    if (IsANGLE()) return false;        // ANGLE は PBO がサイズに関係なく遅い
    return bytes >= PBOUploadThreshold; // 小さい転送は固定コストの小さい直接転送
}

void
GLTexture::UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator)
{
    if (w==0) w = width_;
    if (h==0) h = height_;

    // PBO は「実際に更新されるテクスチャ」だけが必要。 PBO 経路を使う
    // 転送サイズになった時点で初めて確保する (静的テクスチャは PBO を持たない)。
    GLuint pbo = pbo_;
    if (pbo == 0 && UsePBOForUpload((std::size_t)w * h * 4)) {
        pbo = ensurePBO();
    }
    UpdateTexture(texture_id_, pbo, glformat_, x, y, w, h, updator);
}

void
GLTexture::UpdateTextureDirect(int x, int y, int w, int h, const void * src, int src_pitch)
{
    if (!texture_id_ || !src) return;
    if (w == 0) w = width_;
    if (h == 0) h = height_;
    if (w <= 0 || h <= 0) return;

    const int row_bytes = w * 4;
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    if (src_pitch == row_bytes) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, glformat_, GL_UNSIGNED_BYTE, src);
    } else if (TVPOpenGLESVersion >= 300) {
        // GLES3: 行間の読み飛ばしを GL に任せる (コピー不要)
        glPixelStorei(GL_UNPACK_ROW_LENGTH, src_pitch / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, glformat_, GL_UNSIGNED_BYTE, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        // GLES2 には GL_UNPACK_ROW_LENGTH が無いので詰め直してから 1 回で送る
        std::unique_ptr<char[]> buf(new char[static_cast<std::size_t>(row_bytes) * h]);
        const char * sp = static_cast<const char *>(src);
        for (int row = 0; row < h; ++row) {
            memcpy(buf.get() + static_cast<std::size_t>(row) * row_bytes,
                   sp + static_cast<std::size_t>(row) * src_pitch, row_bytes);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, glformat_, GL_UNSIGNED_BYTE, buf.get());
    }
    CheckGLErrorAndLog("glTexSubImage2D (direct)");
    glBindTexture(GL_TEXTURE_2D, 0);
}

//---------------------------------------------------------------------------
// テクスチャべた書き
//---------------------------------------------------------------------------

#include "GLShaderUtil.h"

static const char *vsSource = 
"attribute vec2 a_position;"
"attribute vec2 a_texCoord;"
"varying vec2 v_texCoord;"
"void main()"
"{"
"gl_Position = vec4( a_position, 0.0, 1.0 );"
"v_texCoord = a_texCoord;"
"}"
;

static const char *fsSource = 
"precision mediump float;"
"varying vec2 v_texCoord;"
"uniform sampler2D s_texture;"
"void main()"
"{"
"gl_FragColor = texture2D( s_texture, v_texCoord );"
"}"
;


GLTextureDrawer::GLTextureDrawer()
    : _shader_program(0)
    , _attr_position(0)
    , _attr_texCoord(0)
    , _unif_texture(0)
{
}

GLTextureDrawer::~GLTextureDrawer()
{
    Done();
}

void
GLTextureDrawer::Init()
{
    if (!_shader_program) {
        // べた書き用シェーダー
        _shader_program = CompileProgram(vsSource, fsSource);
        _attr_position  = glGetAttribLocation(_shader_program, "a_position");
        _attr_texCoord  = glGetAttribLocation(_shader_program, "a_texCoord");
        _unif_texture   = glGetUniformLocation(_shader_program, "s_texture");
    }
}

void
GLTextureDrawer::Done()
{
    if (_shader_program) {
        glDeleteProgram(_shader_program);
        _shader_program = 0;
    }
}

// 描画範囲にべた書き処理
void
GLTextureDrawer::DrawTexture(GLTexture *tex, int scr_w, int scr_h, float position[], int tex_w, int tex_h, bool blend, bool premultiplied)
{
	if (_shader_program && tex) {

        glViewport(0, 0, scr_w, scr_h);

        GLfloat u = tex_w == 0 ? 1.0 : (float)tex_w / tex->width();
        GLfloat v = tex_h == 0 ? 1.0 : (float)tex_h / tex->height();

		// UV補正
    	GLfloat _uv[8];
		_uv[0] = 0;
		_uv[1] = v;
		_uv[2] = 0;
		_uv[3] = 0;
		_uv[4] = u;
		_uv[5] = v;
		_uv[6] = u;
		_uv[7] = 0;

        // 描画調整
		glDisable( GL_DEPTH_TEST );
		glDisable( GL_STENCIL_TEST );
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_CULL_FACE );
		if (blend) {
			glEnable( GL_BLEND );
			if (premultiplied) {
				// premultiplied-alpha 合成。 src の RGB に既にアルファが
				// 掛かっているので、 src 係数は GL_ONE。 ThorVG (Elements の
				// canvas) の出力がこれ。 GL_SRC_ALPHA を使うとアルファが
				// 二重に掛かり、 半透明部分が薄くなる。
				glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
			} else {
				// straight-alpha 合成 (動画ミキサ等)
				glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
			}
		} else {
			glDisable( GL_BLEND );
		}

		// シェーダー設定
		glUseProgram(_shader_program);
		glEnableVertexAttribArray(_attr_position);
    	glEnableVertexAttribArray(_attr_texCoord);
		glUniform1i(_unif_texture, 0);

		// テクスチャをバインド
		glBindTexture(GL_TEXTURE_2D, tex->id());
		// パラメータ設定
        glVertexAttribPointer(_attr_position, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*) position);
        glVertexAttribPointer(_attr_texCoord, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*) _uv);
		// 描画実行
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

bool GLTexture::_support_inited = false;
bool GLTexture::_support_bgra = false;
bool GLTexture::_support_swizzle = false;
bool GLTexture::_support_copy_image = false;
bool GLTexture::_support_srgb_write_control = false;

void 
GLTexture::InitSupported()
{
    if (!_support_inited) {

        bool ext_texture_bgra = false;
        bool ext_texture_storage = false;
        bool ext_copy_image = false;
        bool ext_srgb_write_control = false;

        int NumberOfExtensions;
        glGetIntegerv(GL_NUM_EXTENSIONS, &NumberOfExtensions);
        for (int i=0; i<NumberOfExtensions; i++) {
            const char *name = (const char*)glGetStringi(GL_EXTENSIONS, i);
			TVPLOG_DEBUG("OpenGL Extension:{}", name);
            if (strcmp(name, "GL_EXT_texture_format_BGRA8888") == 0 ||
                strcmp(name, "GL_APPLE_texture_format_BGRA8888") == 0) {
                ext_texture_bgra = true;
            } else if (strcmp(name, "GL_EXT_texture_storage") == 0) {
                ext_texture_storage = true;
            } else if (strcmp(name, "GL_EXT_copy_image") == 0) {
                ext_copy_image = true;
            } else if (strcmp(name, "GL_EXT_sRGB_write_control") == 0) {
                ext_srgb_write_control = true;
            }
        }
        _support_bgra = ext_texture_bgra && ext_texture_storage;
        _support_copy_image = ext_copy_image;
        _support_srgb_write_control = ext_srgb_write_control;

        GLint swizzleR;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &swizzleR);
        _support_swizzle = glGetError() == GL_NO_ERROR;
        _support_inited = true;

		if (_support_bgra) {
			TVPLOG_INFO("GLES: BGRA Texture Supported");
		}
		if (_support_swizzle) {
			TVPLOG_INFO("GLES: Texture Swizzle Supported");
		}
		if (_support_copy_image) {
			TVPLOG_INFO("GLES: Copy Image Extension Supported");
		}
		if (_support_srgb_write_control) {
			TVPLOG_INFO("GLES: sRGB Write Control Supported");
		}
    }
}

void 
GLTexture::copyFrom(const GLTexture& source)
{
    if (source.texture_id_ == 0) {
        return; // ソーステクスチャが無効
    }

    // 既存のテクスチャを削除
    destory();

    // ソーステクスチャのパラメータをコピー
    format_ = source.format_;
    glformat_ = source.glformat_;
    width_ = source.width_;
    height_ = source.height_;
    stretchType_ = source.stretchType_;
    wrapS_ = source.wrapS_;
    wrapT_ = source.wrapT_;
    hasMipmap_ = source.hasMipmap_;

    // 空のテクスチャを作成
    create(width_, height_, nullptr, format_);

    // GPU上でコピー実行
    copyTextureOnGPU(source);

    // パラメータを設定
    setStretchType(stretchType_);
    setWrapS(wrapS_);
    setWrapT(wrapT_);
}

void 
GLTexture::copyTextureOnGPU(const GLTexture& source)
{
    if (texture_id_ == 0 || source.texture_id_ == 0) {
        return;
    }

    // OpenGL ES 3.2 以上の場合は glCopyImageSubData を使用
    if (TVPOpenGLESVersion >= 320) {
        // 直接テクスチャ間でコピー
        glCopyImageSubData(source.texture_id_, GL_TEXTURE_2D, 0, 0, 0, 0,
                          texture_id_, GL_TEXTURE_2D, 0, 0, 0, 0,
                          width_, height_, 1);
        CheckGLErrorAndLog("glCopyImageSubData");
        return;
    }

    // GL_EXT_copy_image 拡張がある場合は glCopyImageSubDataEXT を使用
    if (SupportCopyImage()) {
        glCopyImageSubDataEXT(source.texture_id_, GL_TEXTURE_2D, 0, 0, 0, 0,
                             texture_id_, GL_TEXTURE_2D, 0, 0, 0, 0,
                             width_, height_, 1);
        CheckGLErrorAndLog("glCopyImageSubDataEXT");
        return;
    }

    // フォールバック: フレームバッファブリットを使用
    // 現在のフレームバッファを保存
    GLint currentFBO;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &currentFBO);

    // フレームバッファオブジェクトを作成
    GLuint srcFBO, dstFBO;
    glGenFramebuffers(1, &srcFBO);
    glGenFramebuffers(1, &dstFBO);

    // ソーステクスチャをリードフレームバッファにアタッチ
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source.texture_id_, 0);

    // ターゲットテクスチャをドローフレームバッファにアタッチ
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id_, 0);

    // フレームバッファの完全性をチェック
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        
        // GPU上でブリット（コピー）実行
        glBlitFramebuffer(0, 0, width_, height_, 
                         0, 0, width_, height_, 
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
        CheckGLErrorAndLog("glBlitFramebuffer");
    }

    // リソースをクリーンアップ
    glBindFramebuffer(GL_FRAMEBUFFER, currentFBO);
    glDeleteFramebuffers(1, &srcFBO);
    glDeleteFramebuffers(1, &dstFBO);
}

