/**
 * Texture クラス
 */

#ifndef TextureIntfH
#define TextureIntfH

#include "tjsNative.h"
#include "GLTexture.h"
#include "TextureInfo.h"
#include "GLVertexBufferObject.h"
#include "ComplexRect.h"
#include "LayerBitmapIntf.h"


class tTJSNI_Texture : public tTJSNativeInstance, public iTVPTextureInfoIntrface
{
	GLTexture Texture;
	GLVertexBufferObject VertexBuffer;
	tjs_uint SrcWidth = 0;
	tjs_uint SrcHeight = 0;
	// 9patch描画用情報
	tTVPRect Scale9Patch = { -1, -1, -1, -1 };
	tTVPRect Margin9Patch = { -1, -1, -1, -1 };

	tTJSVariant MarginRectObject;
	class tTJSNI_Rect* MarginRectInstance = nullptr;

	void LoadTexture( const class tTVPBaseBitmap* bitmap, tTVPTextureColorFormat color);

	/**
	 * 32bit ビットマップの α チャンネルだけを 8bit テクスチャ (tcfAlpha) にする。
	 *
	 * tcfAlpha は本来「色を輝度化して 8bit にする」経路なので、
	 * 輝度が白一色で α にデータを持つマスク画像には使えなかった。
	 * クリッピングマスクは α しか参照しない (GLClip / clipmask.frag とも .a) ため、
	 * この経路なら RGBA (4byte/px) の 1/4 のメモリで同じ結果になる。
	 *
	 * @param bitmap 32bpp のビットマップ
	 */
	void LoadTextureFromAlphaChannel( const class tTVPBaseBitmap* bitmap );

	tjs_error LoadMipmapTexture( const class tTVPBaseBitmap* bitmap, class tTJSArrayNI* sizeList, enum tTVPBBStretchType type, tjs_real typeopt );

	void SetMarginRectObject( const tTJSVariant & val );

public:
	tTJSNI_Texture();
	~tTJSNI_Texture() override;
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj) override;
	void TJS_INTF_METHOD Invalidate() override;

	const tTJSVariant& GetMarginRectObject() const { return MarginRectObject; }

	void CopyBitmap( tjs_int left, tjs_int top, const class tTVPBaseBitmap* bitmap, const tTVPRect& srcRect );
	void CopyBitmap( const class tTVPBaseBitmap* bitmap );

	void CopyTexture(GLTexture *src);

	GLTexture *GetTexture() { return &Texture; }
	tjs_uint GetWidth() const override { return SrcWidth; }
	tjs_uint GetHeight() const override { return SrcHeight; }
	tjs_uint GetMemoryWidth() const { return Texture.width(); }
	tjs_uint GetMemoryHeight() const { return Texture.height(); }
	bool IsGray() const;
	bool IsPowerOfTwo() const;
	tjs_int64 GetNativeHandle() const override { return Texture.id(); }
	tjs_int64 GetPBOHandle() const override { return Texture.pbo(); }
	tjs_int64 GetVBOHandle() const override;
	// VBOに描画サイズを設定しておき、テクスチャサイズ以外で描画させる
	void SetDrawSize( tjs_uint width, tjs_uint height );

	tTVPTextureColorFormat format() const override { return Texture.format(); }
	GLint glformat() const override { return Texture.glformat(); }

	void UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator) {
		Texture.UpdateTexture(x, y, w, h, updator);
	}

	//! @brief 連続メモリから中間バッファ無しで転送する。GLTexture::UpdateTextureDirect 参照。
	void UpdateTextureDirect(int x, int y, int w, int h, const void *src, int src_pitch) {
		Texture.UpdateTextureDirect(x, y, w, h, src, src_pitch);
	}

	static inline bool IsPowerOfTwo( tjs_uint x ) { return (x & (x - 1)) == 0; }
	static inline tjs_uint ToPowerOfTwo( tjs_uint x ) {
		// 組み込み関数等でMSBを取得してシフトしてもいいが、32からシフトしてループで得ることにする。
		if( IsPowerOfTwo( x ) == false ) {
			tjs_uint r = 32;
			while( r < x ) r = r << 1;
			return r;
		}
		return x;
	}
	
	tjs_int GetStretchType() const;
	void SetStretchType( tjs_int v );
	tjs_int GetWrapModeHorizontal() const;
	void SetWrapModeHorizontal( tjs_int v );
	tjs_int GetWrapModeVertical() const;
	void SetWrapModeVertical( tjs_int v );

	const tTVPRect& GetScale9Patch() const { return Scale9Patch; }
	const tTVPRect& GetMargin9Patch() const { return Margin9Patch; }

	friend class tTJSNI_Offscreen;

	// サイズ変更調整
	bool Resize(tjs_int width, tjs_int height);
};


//---------------------------------------------------------------------------
// tTJSNC_Texture : TJS Texture class
//---------------------------------------------------------------------------
class tTJSNC_Texture : public tTJSNativeClass
{
public:
	tTJSNC_Texture();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance() override { return new tTJSNI_Texture(); }
};
//---------------------------------------------------------------------------
extern tTJSNativeClass * TVPCreateNativeClass_Texture();
#endif
