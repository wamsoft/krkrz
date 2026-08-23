#ifndef __GL_TEXTURE_H__
#define __GL_TEXTURE_H__

#include "OpenGLHeader.h"
#include <functional>
#include <cstdint>

#include "TextureInfo.h"

struct GLTextreImageSet {
	GLuint width;
	GLuint height;
	const GLvoid* bits;
	GLTextreImageSet( GLuint w, GLuint h, const GLvoid* b ) : width( w ), height( h ), bits( b ) {}
};

class GLTexture {

protected:
	GLuint texture_id_;
	tTVPTextureColorFormat format_;
	GLuint glformat_;
	GLuint width_;
	GLuint height_;
	GLuint pbo_;

	GLenum stretchType_;
	GLenum wrapS_;
	GLenum wrapT_;
	bool hasMipmap_ = false;

	//! このインスタンスが計上しているテクスチャ実体のバイト数 (統計用)
	std::uint64_t tex_bytes_ = 0;
	//! このインスタンスが計上しているアップロード用 PBO のバイト数 (統計用)
	std::uint64_t pbo_bytes_ = 0;

private:
	/**
	 * GPU上でテクスチャをコピーするヘルパーメソッド
	 */
	void copyTextureOnGPU(const GLTexture& source);

	/**
	 * アップロード用 PBO を必要になった時点で確保する (遅延確保)。
	 *
	 * 以前は create() で必ず PBO を確保していたが、 それだと
	 * 「ファイルから読んだきり更新しない静的テクスチャ」 (立ち絵 / 背景 /
	 * マスク / UI / オフスクリーン) でもテクスチャ実体と同サイズの PBO を
	 * 常時保持することになり、 GPU メモリを丸ごと 2 倍消費していた。
	 * GL ドライバが通常ヒープからメモリを確保する環境 (一部のコンソール機) では、
	 * これが GL_OUT_OF_MEMORY の直接の原因になりうる。
	 *
	 * @return 確保できた PBO 名。 RGBA 以外や失敗時は 0 (直接転送で動作する)
	 */
	GLuint ensurePBO();

	//! 統計カウンタ更新 (生成時)
	void addMemStats(std::uint64_t tex_bytes, std::uint64_t pbo_bytes);
	//! 統計カウンタ更新 (破棄時)。 このインスタンス分を差し引く
	void subMemStats();

public:
	GLTexture() : texture_id_(0), width_(0), height_(0), pbo_(0), format_(tTVPTextureColorFormat::RGBA), stretchType_(GL_LINEAR), wrapS_(GL_CLAMP_TO_EDGE), wrapT_(GL_CLAMP_TO_EDGE) {}
	GLTexture( GLuint w, GLuint h, const GLvoid* bits=0, tTVPTextureColorFormat format=tTVPTextureColorFormat::RGBA)
	: texture_id_(0), width_(w), height_(h), pbo_(0), format_(format), stretchType_(GL_LINEAR), wrapS_(GL_CLAMP_TO_EDGE), wrapT_(GL_CLAMP_TO_EDGE) {
		create( w, h, bits, format );
	}
	// コピーコンストラクタ
	GLTexture(const GLTexture& other) : texture_id_(0), width_(0), height_(0), pbo_(0), format_(tTVPTextureColorFormat::RGBA), stretchType_(GL_LINEAR), wrapS_(GL_CLAMP_TO_EDGE), wrapT_(GL_CLAMP_TO_EDGE) {
		copyFrom(other);
	}
	// コピー代入演算子
	GLTexture& operator=(const GLTexture& other) {
		if (this != &other) {
			destory();
			copyFrom(other);
		}
		return *this;
	}
	~GLTexture() {
		destory();
	}

	void create( GLuint w, GLuint h, const GLvoid* bits=0, tTVPTextureColorFormat format=tTVPTextureColorFormat::RGBA);

	/**
	 * 既存のGLTextureから内容を複製する
	 */
	void copyFrom(const GLTexture& source);

	/**
	* ミップマップを持つテクスチャを生成する
	* 今のところ GL_RGBA 固定
	*/
	void createMipmapTexture( std::vector<GLTextreImageSet>& img );

	void destory();

	/**
	 * フィルタタイプに応じたミップマップテクスチャフィルタを返す
	 * GL_NEAREST_MIPMAP_LINEAR/GL_LINEAR_MIPMAP_LINEAR は使用していない
	 */
	static GLint getMipmapFilter( GLint filter ) {
		switch( filter ) {
		case GL_NEAREST:
			return GL_NEAREST_MIPMAP_NEAREST;
		case GL_LINEAR:
			return GL_LINEAR_MIPMAP_NEAREST;
		case GL_NEAREST_MIPMAP_NEAREST:
			return GL_NEAREST_MIPMAP_NEAREST;
		case GL_LINEAR_MIPMAP_NEAREST:
			return GL_LINEAR_MIPMAP_NEAREST;
		case GL_NEAREST_MIPMAP_LINEAR:
			return GL_NEAREST_MIPMAP_LINEAR;
		case GL_LINEAR_MIPMAP_LINEAR:
			return GL_LINEAR_MIPMAP_LINEAR;
		default:
			return GL_LINEAR_MIPMAP_NEAREST;
		}
	}

	static int getMaxTextureSize() {
		GLint maxTex;
		glGetIntegerv( GL_MAX_TEXTURE_SIZE, &maxTex );
		return maxTex;
	}
	GLuint width() const { return width_; }
	GLuint height() const { return height_; }
	GLuint id() const { return texture_id_; }
	GLint glformat() const { return glformat_; }
	GLint pbo() const { return pbo_; }

	tTVPTextureColorFormat format() const { return format_; }

	GLenum stretchType() const { return stretchType_; }
	void setStretchType( GLenum s ) {
		if( texture_id_ && stretchType_ != s ) {
			glBindTexture( GL_TEXTURE_2D, texture_id_ );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s );
			if( hasMipmap_ == false ) {
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s );
			} else {
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, getMipmapFilter(s) );
			}
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		stretchType_ = s;
	}
	GLenum wrapS() const { return wrapS_; }
	void setWrapS( GLenum s ) {
		if( texture_id_ && wrapS_ != s ) {
			glBindTexture( GL_TEXTURE_2D, texture_id_ );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		wrapS_ = s;
	}
	GLenum wrapT() const { return wrapT_; }
	void setWrapT( GLenum s ) {
		if( texture_id_ && wrapT_ != s ) {
			glBindTexture( GL_TEXTURE_2D, texture_id_ );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, s );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		wrapT_ = s;
	}

    static void UpdateTexture(GLuint tex_id, GLuint pbo, int format, int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator);
    void UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator);

    /**
     * PBO を経由せず、 呼出側が既に持っている連続メモリから直接
     * glTexSubImage2D する。 src_pitch (バイト) が w*4 と異なる場合は
     * GL_UNPACK_ROW_LENGTH で読み飛ばすので、 大きなバッファの部分矩形を
     * コピーなしで送れる。
     *
     * PBO 経路は「CPU が map したメモリへ書く → GPU が非同期に吸い上げる」形を
     * 狙ったものだが、 ANGLE (GLES→D3D11) では PBO からの glTexSubImage2D が
     * 内部でバッファを読み戻すため、 毎フレーム書き換えるテクスチャでは
     * かえって同期待ち (しかもビジーウェイト) になる。 CPU 側に既にデータが
     * あるならこちらの方が速い。
     */
    void UpdateTextureDirect(int x, int y, int w, int h, const void * src, int src_pitch);

    /**
     * この 1 回の転送に PBO を使うべきか。 実測 (data/upload_bench) に基づく:
     *
     *  - PBO 経路は **1 回あたりの固定コストが大きく、 大量転送は速い**
     *    (NX 実測で 121us/回 + 2.5GB/s)
     *  - 直接転送は **固定コストが小さく、 大量転送は遅い**
     *    (同 16us/回 + 1.25GB/s)
     *  → 交差点は約 256KB。 それ未満は直接、 以上は PBO が速い。
     *
     *  - ただし **ANGLE (Windows の GLES→D3D11) は PBO からの転送で内部的に
     *    バッファを読み戻すため、 サイズに関係なく PBO が致命的に遅い**
     *    (全画面 1 枚で 10〜13ms、 実時間の 60〜80% を消費)。 ANGLE では常に直接。
     *
     * 立ち絵 + UI が散在して個別更新される実案件の形 (小矩形が多数) では
     * 1 回あたりの固定コストが効くので、 この判定が大きく効く
     * (NX 実測: 小矩形 40 個/フレームで PBO 30.0% 対 直接 5.8%)。
     *
     * @param bytes この転送で送るバイト数
     */
    static bool UsePBOForUpload(std::size_t bytes);

    /**
     * 転送経路の強制指定 (計測・比較用)。 環境変数 KRKRZ_GLTEXUP=pbo|direct と
     * TJS の System.texUploadUsePBO から設定される。 環境変数を渡せない実機では
     * TJS 側を使う。
     *
     * @param v  1=PBO 強制 / 0=直接転送 強制 / -1=サイズ判定に戻す (既定)
     */
    static void SetUploadOverride(int v);
    static int  GetUploadOverride();

    //! @brief 実装が ANGLE (GLES→D3D11 エミュレーション) か。要 current context。
    static bool IsANGLE();

    //! PBO を使う下限バイト数 (これ未満は直接転送)。
    static const std::size_t PBOUploadThreshold = 256 * 1024;

    /**
     * 外部から所有権を引き取る形で texture_id_ を差し替える。
     * 旧 ID は glDeleteTextures せず単に上書きする (所有権は呼び出し側で
     * 別コンテナに移譲済みであることが前提)。Offscreen の ExchangeTexture
     * のような GL handle 入れ替え操作で使う。
     *
     * 注意: width / height / format / glformat / pbo は更新しないので、
     *       同一サイズ・同一フォーマットのテクスチャ間でのみ使うこと。
     */
    void AdoptTextureId(GLuint id) { texture_id_ = id; }

public:
	static bool _support_inited;
	static bool _support_bgra;
	static bool _support_swizzle;
	static bool _support_copy_image;
	static bool _support_srgb_write_control;
	static void InitSupported();

	static bool SupportBGRAFormat() { 
		InitSupported();
		return _support_bgra;
	}

	static bool SupportBGRA() { 
		InitSupported();
		return _support_bgra || _support_swizzle;
	}

	static bool SupportCopyImage() {
		InitSupported();
		return _support_copy_image;
	}

	// GL_EXT_sRGB_write_control (GLES 拡張)。未対応環境で
	// glEnable/glDisable(GL_FRAMEBUFFER_SRGB_EXT) を呼ぶと GL_INVALID_ENUM に
	// なるため、対応時のみ呼ぶ判定に使う。
	static bool SupportSRGBWriteControl() {
		InitSupported();
		return _support_srgb_write_control;
	}

	// -----------------------------------------------------------------
	// テクスチャメモリ計測
	//
	// GL ドライバが実際に確保する量そのものではないが、 こちらから要求した
	// バイト数 (テクスチャ実体 + アップロード用 PBO) を積算する。
	// GL ドライバが通常ヒープを使う環境 (一部のコンソール機) で、 どれだけ
	// GPU リソースを積んでいるかの実測値として使う。
	// -----------------------------------------------------------------
	struct MemStats {
		std::uint64_t texture_bytes;    //!< テクスチャ実体の合計
		std::uint64_t pbo_bytes;        //!< アップロード用 PBO の合計
		std::uint64_t fbo_bytes;        //!< FBO (カラーテクスチャ + レンダーバッファ + PBO) の合計
		std::uint64_t total_bytes;      //!< 上記の合計
		std::uint64_t peak_total_bytes; //!< total_bytes の最大値
		std::uint32_t texture_count;    //!< 生存テクスチャ数
		std::uint32_t pbo_count;        //!< 生存 PBO 数
		std::uint32_t fbo_count;        //!< 生存 FBO 数
	};

	/**
	 * GLTexture を経由しない GPU メモリ (GLFrameBufferObject のカラーテクスチャ /
	 * レンダーバッファ / PBO) を同じカウンタへ計上する。
	 * Offscreen はこちらを通るので、 これが無いと実測から丸ごと抜け落ちる。
	 *
	 * @param bytes_delta 増減バイト数 (解放時は負)
	 * @param count_delta 生存数の増減 (通常 +1 / -1、 増減なしは 0)
	 */
	static void NoteFboMemory(std::int64_t bytes_delta, std::int32_t count_delta);

	//! 現在のテクスチャメモリ統計を取得する (スレッド安全)
	static void GetMemStats(MemStats &out);

	//! peak_total_bytes を現在値へリセットする
	static void ResetMemPeak();

	/**
	 * 合計が一定量 (MemLogStepBytes) 増減するたびにログへ書き出す。
	 * 既定 false (調査時のみ有効化する)。
	 * TJS からは System.setTextureMemoryLog(true/false)。
	 */
	static bool MemLogEnabled;

	//! ログを出す増減幅 (バイト)
	static const std::uint64_t MemLogStepBytes = 8ull * 1024 * 1024;
};

class GLTextureDrawer
{

public:
	GLTextureDrawer();
	~GLTextureDrawer();

	void Init();
	void Done();

	//! @param blend  true なら GL_BLEND を有効にし (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)
	//!               で straight-alpha 合成。false (default) なら GL_BLEND を無効化。
	void DrawTexture(GLTexture *tex, int scr_w, int scr_h, float position[], int tex_w=0, int tex_h=0, bool blend=false);

private:
	GLuint _shader_program;
	GLint _attr_position;
	GLint _attr_texCoord;
	GLint _unif_texture;
};


#endif // __GL_TEXTURE_H__
