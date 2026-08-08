
#define NOMINMAX
#include "tjsCommHead.h"
#include "DrawDevice.h"
#include "BitmapDrawDevice.h"
#include "LayerIntf.h"
#include "MsgImpl.h"
#include "SysInitIntf.h"
#include "WindowIntf.h"
#include "DebugIntf.h"
#include "ThreadIntf.h"
#include "ComplexRect.h"
#include "EventIntf.h"
#include "WindowImpl.h"
#include "BitmapInfomation.h"

#include <algorithm>

//---------------------------------------------------------------------------
tTVPBitmapDrawDevice::tTVPBitmapDrawDevice(iTJSDispatch2 *self)
 : BitmapInstance(nullptr)
 , Self(self)
{
	if (Self) Self->AddRef();

	// BitmapClass
	BitmapClass = TVPCreateNativeClass_Bitmap();
}

//---------------------------------------------------------------------------
tTVPBitmapDrawDevice::~tTVPBitmapDrawDevice()
{
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::Show()
{
	if( Self ) {
		static ttstr eventname( TJS_W( "onDraw" ) );
		TVPPostEvent( Self, Self, eventname, 0, TVP_EPT_IMMEDIATE, 0, nullptr );
	}
}

//---------------------------------------------------------------------------
void tTVPBitmapDrawDevice::DestroyBitmap() 
{
	// invalidate Bitmap object
	if (BitmapObject.Type() == tvtObject) {
		BitmapObject.AsObjectClosureNoAddRef().Invalidate(0, nullptr, nullptr, BitmapObject.AsObjectNoAddRef());
	}
	BitmapObject.Clear();
	BitmapInstance = nullptr;
}

//------------------------------------------------------g---------------------
void tTVPBitmapDrawDevice::CreateBitmap() {

	if (!BitmapInstance && BitmapClass) {

		tjs_int w, h;
		GetSrcSize( w, h );
		if (w > 0 && h > 0) {

			iTJSDispatch2 * newobj = NULL;
			try
			{
				tTJSVariant param_w = w;
				tTJSVariant param_h = h;
				tTJSVariant *pparam[2] = { &param_w,  &param_h };
				if( TJS_FAILED( BitmapClass->CreateNew( 0, nullptr, nullptr, &newobj, 2, pparam, BitmapClass ) ) )
					TVPThrowExceptionMessage( TVPInternalError, TJS_W( "tTJSNI_Bitmap::Construct" ) );
				BitmapObject = tTJSVariant( newobj, newobj );
			} catch( ... )
			{
				if( newobj ) newobj->Release();
				throw;
			}
			if( newobj ) newobj->Release();

			// extract interface
			if (BitmapObject.Type() == tvtObject) {
				tTJSVariantClosure clo = BitmapObject.AsObjectClosureNoAddRef();
				if( clo.Object ) {
					if(TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE, tTJSNC_Bitmap::ClassID, (iTJSNativeInstance**)&BitmapInstance)))
					{
						BitmapInstance = nullptr;
						TVPThrowExceptionMessage(TVPCannotRetriveInstance, TJS_W("Bitmap"));
					}
				}
			}
		
			RequestInvalidation(tTVPRect(0, 0, DestRect.get_width(), DestRect.get_height()));
		}
	}
}


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::Destruct()
{
	// release canvas
	DestroyBitmap();
	if (Self) Self->Release(); Self = nullptr;

	if (BitmapClass) BitmapClass->Release(); BitmapClass = nullptr;
	inherited::Destruct();
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	if(inherited::Managers.size() > 0)
	{
		// "Bitmap" デバイスでは２つ以上のLayer Managerを登録できない
		TVPThrowExceptionMessage(TVPBasicDrawDeviceDoesNotSupporteLayerManagerMoreThanOne);
	}
	inherited::AddLayerManager(manager);

	manager->SetDesiredLayerType(ltOpaque); // ltOpaque な出力を受け取りたい
}

void TJS_INTF_METHOD tTVPBitmapDrawDevice::SetTargetWindow(HWND wnd, bool is_main)
{
	// 何もしない
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	inherited::NotifyLayerResize(manager);
	if (BitmapInstance) {
		tjs_int w, h;
		GetSrcSize( w, h );
		BitmapInstance->SetSize(w, h);
	} else {
		CreateBitmap();
	}
}

void TJS_INTF_METHOD tTVPBitmapDrawDevice::StartBitmapCompletion(iTVPLayerManager * manager)
{
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::NotifyBitmapCompleted(iTVPLayerManager * manager,
	tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bmpinfo,
	const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity)
{
	if (!BitmapInstance) return;

	int _width  = bmpinfo->bmiHeader.biWidth;
	int _height = bmpinfo->bmiHeader.biHeight;
	int _pitch  = bmpinfo->bmiHeader.biSizeImage / _height;

	int src_w = cliprect.get_width();
	int src_h = cliprect.get_height();

	// bits, bitmapinfo で表されるビットマップの cliprect の領域を、x, y に描画
	// する。
	// opacity と type は無視するしかないので無視する
	tjs_int w, h;
	GetSrcSize( w, h );
	if (!(x < 0 || y < 0 ||
			x + src_w > w ||
			y + src_h > h) &&
		!(cliprect.left < 0 || cliprect.top < 0 ||
			cliprect.right > _width ||
			cliprect.bottom > _height))
	{
		// bitmapinfo で表された cliprect の領域を x,y にコピーする
		long src_y       = cliprect.top;
		long src_y_limit = cliprect.bottom;
		long src_x       = cliprect.left;
		long width_bytes   = cliprect.get_width() * 4; // 32bit
		const tjs_uint8 * src_p = (const tjs_uint8 *)bits;
		long src_pitch;

		if(_height < 0)
		{
			// bottom-down
			src_pitch = _pitch;
		}
		else
		{
			// bottom-up
			src_pitch = - _pitch;
			src_p += _pitch * (_height - 1);
		}

		tjs_uint8 * dest_p = (tjs_uint8*)BitmapInstance->GetPixelBufferForWrite();	
		long dest_pitch = BitmapInstance->GetPixelBufferPitch();
		dest_p += (y * dest_pitch) + (x * 4);

		for(; src_y < src_y_limit; src_y++)
		{
			memcpy(dest_p, src_p + (src_x * 4), width_bytes);
			src_p += src_pitch;
			dest_p += dest_pitch;
		}

	}
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPBitmapDrawDevice::EndBitmapCompletion(iTVPLayerManager * manager)
{
}


//---------------------------------------------------------------------------
// tTJSNI_BitmapDrawDevice : BitmapDrawDevice TJS native class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_BitmapDrawDevice::ClassID = (tjs_uint32)-1;
tTJSNC_BitmapDrawDevice::tTJSNC_BitmapDrawDevice() :
	tTJSNativeClass(TJS_W("BitmapDrawDevice"))
{
	// register native methods/properties
	TJS_BEGIN_NATIVE_MEMBERS(BitmapDrawDevice)
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
// constructor/methods
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_BitmapDrawDevice,
	/*TJS class name*/BitmapDrawDevice)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/BitmapDrawDevice)


//----------------------------------------------------------------------
// properties
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(interface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BitmapDrawDevice);
		*result = reinterpret_cast<tjs_int64>(_this->GetDevice());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(interface)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(bitmap)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_BitmapDrawDevice);
		*result = _this->GetDevice()->GetBitmapObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(bitmap)
//----------------------------------------------------------------------
	TJS_END_NATIVE_MEMBERS
}
//---------------------------------------------------------------------------
iTJSNativeInstance *tTJSNC_BitmapDrawDevice::CreateNativeInstance()
{
	return new tTJSNI_BitmapDrawDevice();
}
//---------------------------------------------------------------------------
tTJSNI_BitmapDrawDevice::tTJSNI_BitmapDrawDevice()
{
	Device = nullptr;
}
//---------------------------------------------------------------------------
tTJSNI_BitmapDrawDevice::~tTJSNI_BitmapDrawDevice()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
	tTJSNI_BitmapDrawDevice::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Device = new tTVPBitmapDrawDevice(tjs_obj);
	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BitmapDrawDevice::Invalidate()
{
	if(Device) Device->Destruct(), Device = NULL;
}
//---------------------------------------------------------------------------

