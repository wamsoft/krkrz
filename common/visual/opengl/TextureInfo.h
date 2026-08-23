
#ifndef TextureInfoH
#define TextureInfoH

enum class tTVPTextureColorFormat : tjs_int {
	RGBA = 0,
	Alpha = 1,
	// Luminance or Compressed texture
};

/**
 * TJS の Texture(filename/bitmap, format) にだけ指定できる拡張フォーマット値
 * (SysInitScript の tcfAlphaChannel)。 α チャンネルをそのまま 8bit テクスチャにする。
 *
 * tcfAlpha (=1) は「色を輝度化して 8bit」なので、 輝度が白一色で α に形状を持つ
 * マスク画像には使えない。 クリッピングマスクは α しか参照しない (GLClip /
 * clipmask.frag とも .a) ため、 こちらを使えば RGBA (4byte/px) の 1/4 のメモリで
 * 同じ結果が得られる。 生成されたテクスチャの format() は Alpha になる。
 */
const tjs_int TVP_TCF_ALPHA_CHANNEL = 2;

class iTVPTextureInfoIntrface {
public:

	/**
	 * 幅を取得
	 * @return テクスチャ幅
	 */
	virtual tjs_uint GetWidth() const = 0;

	/**
	 * 高さを取得
	 * @return テクスチャ高さ
	 */
	virtual tjs_uint GetHeight() const = 0;

	/**
	 * ネイティブハンドルを取得。OpenGL ES2/3実装ではテクスチャID
	 * @return ネイティブハンドル
	 */
	virtual tjs_int64 GetNativeHandle() const = 0;

	/**
	 * テクスチャ転送用バッファのハンドルを返す。
	 * @return PBO ID、0の時PBOがない
	 */
	virtual tjs_int64 GetPBOHandle() const = 0;


	/**
	 * テクスチャ全体を表す頂点データのVBOをハンドルを返す。
	 * @return VBO ID、0の時VBOがない
	 */
	virtual tjs_int64 GetVBOHandle() const = 0;

	/**
	 * テクスチャフォーマットを返す
	 */
	virtual tTVPTextureColorFormat format() const = 0;

	/**
	 * OpenGLのテクスチャフォーマットを返す
	 */
	virtual GLint glformat() const = 0;
};

#endif
