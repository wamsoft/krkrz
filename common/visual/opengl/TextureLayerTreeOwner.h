//---------------------------------------------------------------------------
/**
 * 描画先を Texture とする Layer Tree Owner
 * レイヤーに描かれて、合成された内容は、このクラスの保持する Texture に描かれる
 * イベントは非対応
 */
//---------------------------------------------------------------------------
//!@file レイヤーツリーオーナー
//---------------------------------------------------------------------------

#ifndef TextureLayerTreeOwner_H
#define TextureLayerTreeOwner_H

#include "LayerTreeOwnerImpl.h"
#include "TextureUpdateRect.h"
#include <functional>

class tTJSNI_TextureLayerTreeOwner : public tTJSNativeInstance, public tTVPLayerTreeOwner
{
	iTJSDispatch2 *Owner;

	// primaryLayer 描画用テクスチャ
	tTJSNativeClass *TextureClass;
	tTJSVariant TextureObject;
	class tTJSNI_Texture *TextureInstance;
	TextureUpdateRect mTextureUpdateRect;

	void CreateTexture();
	void DestroyTexture();
	void UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator);

public:
	tTJSNI_TextureLayerTreeOwner();
	~tTJSNI_TextureLayerTreeOwner();

	tjs_int GetWidth() const;
	tjs_int GetHeight() const;

	// tTJSNativeInstance
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	tTJSVariant &GetTextureObject();

	// tTVPLayerTreeOwner
	iTJSDispatch2 * TJS_INTF_METHOD GetOwnerNoAddRef() const { return Owner; }

	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(class iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const class BitmapInfomation * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

	virtual void OnSetMouseCursor( tjs_int cursor );
	virtual void OnGetCursorPos(tjs_int &x, tjs_int &y);
	virtual void OnSetCursorPos(tjs_int x, tjs_int y);
	virtual void OnReleaseMouseCapture();
	virtual void OnSetHintText(iTJSDispatch2* sender, const ttstr &hint);

	virtual void OnResizeLayer( tjs_int w, tjs_int h );
	virtual void OnChangeLayerImage();

	virtual void OnSetAttentionPoint(tTJSNI_BaseLayer *layer, tjs_int x, tjs_int y);
	virtual void OnDisableAttentionPoint();
	virtual void OnSetImeMode(tjs_int mode);
	virtual void OnResetImeMode();
};

class tTJSNC_TextureLayerTreeOwner : public tTJSNativeClass {
	typedef tTJSNativeClass inherited;
public:
	tTJSNC_TextureLayerTreeOwner();
	static tjs_uint32 ClassID;

protected:
	tTJSNativeInstance *CreateNativeInstance();
};

extern tTJSNativeClass * TVPCreateNativeClass_TextureLayerTreeOwner();
#endif
