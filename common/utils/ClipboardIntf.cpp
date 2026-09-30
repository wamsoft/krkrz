//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Clipboard Class interface
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include "ClipboardIntf.h"
#include "LayerIntf.h"
#include "MsgIntf.h"


//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
tTJSNI_BaseClipboard::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *dsp)
{
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD
tTJSNI_BaseClipboard::Invalidate()
{
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
//----------------------------------------------------------------------
// 引数の Layer から native instance を取り出す
//   もとは clipboardEx.dll (吉里吉里2 の標準プラグイン) が提供していた機能。
//   使われていたのは画像の出し入れだけなので、本体の Clipboard クラスへ移した。
//----------------------------------------------------------------------
static tTJSNI_BaseLayer * TVPGetLayerFromVariant(tTJSVariant * param)
{
	tTJSVariantClosure clo = param->AsObjectClosureNoAddRef();
	tTJSNI_BaseLayer * lay = NULL;
	if(!clo.Object ||
		TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_Layer::ClassID, (iTJSNativeInstance**)&lay)) || !lay)
		TVPThrowExceptionMessage(TJS_W("Clipboard: Layer を渡してください"));
	return lay;
}
//----------------------------------------------------------------------

tjs_uint32 tTJSNC_Clipboard::ClassID = -1;
//---------------------------------------------------------------------------
tTJSNC_Clipboard::tTJSNC_Clipboard(): inherited(TJS_W("Clipboard"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Clipboard) // constructor
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL_NO_INSTANCE(/*TJS class name*/Clipboard)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Clipboard)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/hasFormat)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTVPClipboardFormat format  = (tTVPClipboardFormat)(tjs_int)*param[0];

	bool has = TVPClipboardHasFormat(format);

	if(result)
		*result = has;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/hasFormat)
//----------------------------------------------------------------------

TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setAsBitmap)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSNI_BaseLayer * lay = TVPGetLayerFromVariant(param[0]);

	const void * bits = lay->GetMainImagePixelBuffer();
	tjs_int pitch = lay->GetMainImagePixelBufferPitch();
	tjs_int w = (tjs_int)lay->GetImageWidth();
	tjs_int h = (tjs_int)lay->GetImageHeight();

	bool done = false;
	if(bits && w > 0 && h > 0)
		done = TVPClipboardSetBitmap(bits, w, h, pitch);

	if(result) *result = done;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/setAsBitmap)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/getAsBitmap)
{
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;

	tTJSNI_BaseLayer * lay = TVPGetLayerFromVariant(param[0]);

	std::vector<tjs_uint32> src;
	tjs_int w = 0, h = 0;
	bool got = TVPClipboardGetBitmap(src, w, h);

	if(got && w > 0 && h > 0)
	{
		lay->SetImageSize((tjs_uint)w, (tjs_uint)h);
		tjs_uint8 * dest = (tjs_uint8*)lay->GetMainImagePixelBufferForWrite();
		tjs_int pitch = lay->GetMainImagePixelBufferPitch();
		if(dest)
		{
			for(tjs_int y = 0; y < h; y++)
				memcpy(dest + (tjs_int64)pitch * y, &src[(size_t)w * y],
					(size_t)w * sizeof(tjs_uint32));
			lay->Update();
		}
		else got = false;
	}
	else got = false;

	if(result) *result = got;

	return TJS_S_OK;
}
TJS_END_NATIVE_STATIC_METHOD_DECL(/*func. name*/getAsBitmap)
//----------------------------------------------------------------------

//-- events

//----------------------------------------------------------------------
//----------------------------------------------------------------------

//--properties

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(asText)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		ttstr text;
		bool got = TVPClipboardGetText(text);
		if(got)
			*result = text;
		else
			result->Clear();
			// returns void if the clipboard does not have a text data
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TVPClipboardSetText(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_STATIC_PROP_DECL(asText)
//----------------------------------------------------------------------

	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNC_Clipboard
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_Clipboard::CreateNativeInstance()
{
	return NULL;
}
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_Clipboard()
{
	tTJSNativeClass *cls = new tTJSNC_Clipboard();

	return cls;
}
//---------------------------------------------------------------------------
