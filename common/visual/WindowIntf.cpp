//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Window" TJS Class implementation
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include <algorithm>
#include "MsgIntf.h"
#include "WindowIntf.h"
#include "LayerIntf.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "LayerBitmapIntf.h"
#include "LayerIntf.h"
#include "SysInitIntf.h"
#include "LayerManager.h"
#include "VideoOvlIntf.h"
#include "DrawDevice.h"

#ifdef TVP_USE_OPENGL
#include "OpenGLContext.h"   // iTVPGLContext / InitGLES
#include "OpenGLHeader.h"    // TVPGLGetProcAddress
#include "WindowImpl.h"      // tTJSNI_Window::GetForm()
#ifdef __WINVER__
#include "WindowFormUnit.h"
#else
#include "WindowForm.h"
#endif
#endif

#include "Application.h"

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
// Window レベルの入力イベントを Elements ダイアログがアクティブな場合に
// onClick/onMouseDown/onKeyDown 等の TJS 通知より前に Elements 側へ転送する。
#define TVP_DIALOG_INTERCEPT(forward_call) \
	do { \
		if (tTVPElementsDialogManager::Instance().IsModalActive()) { \
			/* cursor-warp ナビ用に入力元 window (this) を記録 */ \
			tTVPElementsDialogManager::Instance().NoteInputWindow(this); \
			if (tTVPElementsDialogManager::Instance().forward_call) \
				return; \
		} \
	} while(0)
#else
#define TVP_DIALOG_INTERCEPT(forward_call) ((void)0)
#endif

extern tjs_int TVPGetCursor( const ttstr & name );
//---------------------------------------------------------------------------
// Window List
//---------------------------------------------------------------------------
tTJSNI_Window * TVPMainWindow = NULL; // main window
static std::vector<tTJSNI_Window*> TVPWindowVector;
//---------------------------------------------------------------------------
static void TVPRegisterWindowToList(tTJSNI_Window *window)
{
	if(TVPMainWindow == NULL && TVPWindowVector.size() == 0)
	{
		// first time the window is registered
		TVPMainWindow = window; // set as main window
	}
	TVPWindowVector.push_back(window);

	// notify that the layer must lost capture state
	std::vector<tTJSNI_Window*>::iterator i;
	for(i = TVPWindowVector.begin(); i!=TVPWindowVector.end(); i++)
	{
		(*i)->PostReleaseCaptureEvent();
	}
}
//---------------------------------------------------------------------------
static void TVPUnregisterWindowToList(tTJSNI_Window *window)
{
	std::vector<tTJSNI_Window*>::iterator i;
	i = std::find(TVPWindowVector.begin(), TVPWindowVector.end(), window);
	if(i != TVPWindowVector.end())
	{
		bool flag = false;
		if(*i == TVPMainWindow) flag = true;

		TVPWindowVector.erase(i);

		if(flag)
		{
			TVPMainWindowClosed(); // MainWindow had been closed
			TVPMainWindow = NULL;
		}
	}
}
//---------------------------------------------------------------------------
tTJSNI_Window * TVPGetWindowListAt(tjs_int idx) { return TVPWindowVector[idx]; }
//---------------------------------------------------------------------------
tjs_int TVPGetWindowCount() { return (tjs_int)TVPWindowVector.size(); }
//---------------------------------------------------------------------------
void TVPClearAllWindowInputEvents()
{
	std::vector<tTJSNI_Window*>::iterator i;
	for(i = TVPWindowVector.begin(); i!=TVPWindowVector.end(); i++)
	{
		(*i)->ClearInputEvents();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool TVPIsWaitVSync()
{
	bool result = false;
	std::vector<tTJSNI_Window*>::iterator i;
	for(i = TVPWindowVector.begin(); i!=TVPWindowVector.end(); i++)
	{
		result |= (*i)->GetWaitVSync();
	}
	return result;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// Input Events
//---------------------------------------------------------------------------
// For each input event tag
tTVPUniqueTagForInputEvent tTVPOnCloseInputEvent              ::Tag;
tTVPUniqueTagForInputEvent tTVPOnResizeInputEvent             ::Tag;
tTVPUniqueTagForInputEvent tTVPOnClickInputEvent              ::Tag;
tTVPUniqueTagForInputEvent tTVPOnDoubleClickInputEvent        ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseDownInputEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseUpInputEvent            ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseMoveInputEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnReleaseCaptureInputEvent     ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseOutOfWindowInputEvent   ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseEnterInputEvent         ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseLeaveInputEvent         ::Tag;
tTVPUniqueTagForInputEvent tTVPOnKeyDownInputEvent            ::Tag;
tTVPUniqueTagForInputEvent tTVPOnKeyUpInputEvent              ::Tag;
tTVPUniqueTagForInputEvent tTVPOnKeyPressInputEvent           ::Tag;
tTVPUniqueTagForInputEvent tTVPOnFileDropInputEvent           ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMouseWheelInputEvent         ::Tag;
tTVPUniqueTagForInputEvent tTVPOnPopupHideInputEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnWindowActivateEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnTouchDownInputEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnTouchUpInputEvent            ::Tag;
tTVPUniqueTagForInputEvent tTVPOnTouchMoveInputEvent          ::Tag;
tTVPUniqueTagForInputEvent tTVPOnTouchScalingInputEvent       ::Tag;
tTVPUniqueTagForInputEvent tTVPOnTouchRotateInputEvent        ::Tag;
tTVPUniqueTagForInputEvent tTVPOnMultiTouchInputEvent         ::Tag;
tTVPUniqueTagForInputEvent tTVPOnHintChangeInputEvent         ::Tag;
tTVPUniqueTagForInputEvent tTVPOnDisplayRotateInputEvent      ::Tag;
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
// tTJSNI_BaseWindow
//---------------------------------------------------------------------------
tTJSNI_BaseWindow::tTJSNI_BaseWindow()
{
	WaitVSync = true;
	ObjectVectorLocked = false;
	DrawBuffer = NULL;
	WindowExposedRegion.clear();
	WindowUpdating = false;
	DrawDevice = NULL;
	LayerEventTarget = NULL;
}
//---------------------------------------------------------------------------
tTJSNI_BaseWindow::~tTJSNI_BaseWindow()
{
	TVPUnregisterWindowToList(static_cast<tTJSNI_Window*>(this));  // making sure...
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
tTJSNI_BaseWindow::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	Owner = tjs_obj; // no addref
	TVPRegisterWindowToList(static_cast<tTJSNI_Window*>(this));
	return TJS_S_OK;
}

/**
 * @brief Initialize Draw Context (DrawDevice)
 */
void 
tTJSNI_BaseWindow::InitDrawContext(iTJSDispatch2 *tjs_obj)
{
	// set default draw device object "Basic" or "Null"
	{
		iTJSDispatch2 * cls = NULL;
		iTJSDispatch2 * newobj = NULL;
		try
		{
			cls = TVPGetDefaultDrawDevice();
			if(TJS_FAILED(cls->CreateNew(0, NULL, NULL, &newobj, 0, NULL, cls)))
				TVPThrowExceptionMessage(TVPInternalError,
					TJS_W("tTJSNI_Window::Construct"));
			SetDrawDeviceObject(tTJSVariant(newobj, newobj));
		}
		catch(...)
		{
			if(cls) cls->Release();
			if(newobj) newobj->Release();
			throw;
		}
		if(cls) cls->Release();
		if(newobj) newobj->Release();
	}
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD
tTJSNI_BaseWindow::Invalidate()
{
	// remove from list
	TVPUnregisterWindowToList(static_cast<tTJSNI_Window*>(this));

#ifdef TVP_USE_OPENGL
	// GLGetProcAddress で遅延取得した GL コンテキストを解放
	if( PluginGLContext ) {
		PluginGLContext->Release();
		PluginGLContext = nullptr;
	}
#endif

	// remove all events
	TVPCancelSourceEvents(Owner);
	TVPCancelInputEvents(this);

	// clear all window update events
	TVPRemoveWindowUpdate((tTJSNI_Window*)this);

	// sever the primarylayer
//	if(LayerManager) LayerManager->DetachPrimary();

	// free DrawBuffer
	if(DrawBuffer) delete DrawBuffer;

	// disconnect all VideoOverlay objects
	{
		tObjectListSafeLockHolder<tTJSNI_BaseVideoOverlay> holder(VideoOverlay);
		tjs_int count = VideoOverlay.GetSafeLockedObjectCount();
		for(tjs_int i = 0; i < count; i++)
		{
			tTJSNI_BaseVideoOverlay * item = VideoOverlay.GetSafeLockedObjectAt(i);
			if(!item) continue;
			
			item->Disconnect();
		}
	}

	// invalidate all registered objects
	ObjectVectorLocked = true;

	for(std::vector<tTJSVariantClosure>::iterator i = ObjectVector.begin(); i != ObjectVector.end(); i++)
	{
		// invalidate each --
		// objects may throw an exception while invalidating,
		// but here we cannot care for them.
		try
		{
			i->Invalidate(0, NULL, NULL, NULL);
			i->Release();
		}
		catch(eTJSError &e)
		{
			TVPAddLog(e.GetMessage()); // just in case, log the error
		}
	}

	// remove all events (again)
	TVPCancelSourceEvents(Owner);
	TVPCancelInputEvents(this);

	// notify that the window is no longer available
//	if(LayerManager) LayerManager->SetWindow(NULL);

	// clear all window update events
	TVPRemoveWindowUpdate((tTJSNI_Window*)this);

	// release layer event target
	SetLayerEventTargetObject(tTJSVariant());

	// release draw device
	SetDrawDeviceObject(tTJSVariant());

	// release layermanagers
	{
		std::vector<iTVPLayerManager *>::iterator i = Managers.begin(); 
		while (i != Managers.end()) {
			(*i)->Release();
			i = Managers.erase(i);
		}
	}

	inherited::Invalidate();

	/* NOTE: at this point, Owner is still non-null.
	   Caller must ensure that the Owner being null at the end of the
	   invalidate chain. */
}
//---------------------------------------------------------------------------
bool tTJSNI_BaseWindow::IsMainWindow() const
{
	return TVPMainWindow == static_cast<const tTJSNI_Window*>(this);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::FireOnActivate(bool activate_or_deactivate)
{
	// fire Window.onActivate or Window.onDeactivate event
	TVPPostInputEvent(
		new tTVPOnWindowActivateEvent(this, activate_or_deactivate),
		TVP_EPT_REMOVE_POST // to discard redundant events
		);
}


//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::SetDrawDeviceObject(const tTJSVariant & val)
{
	// invalidate existing draw device
	if(DrawDeviceObject.Type() == tvtObject)
		DrawDeviceObject.AsObjectClosureNoAddRef().Invalidate(0, NULL, NULL, DrawDeviceObject.AsObjectNoAddRef());

	// assign new device
	DrawDeviceObject = val;
	DrawDevice = NULL;

	// extract interface
	if(DrawDeviceObject.Type() == tvtObject)
	{
		tTJSVariantClosure clo = DrawDeviceObject.AsObjectClosureNoAddRef();
		tTJSVariant iface_v;
		if(TJS_FAILED(clo.PropGet(0, TJS_W("interface"), NULL, &iface_v, NULL)))
			TVPThrowExceptionMessage( TVPCannotRetriveInterfaceFromDrawDevice );
		DrawDevice =
			reinterpret_cast<iTVPDrawDevice *>((tjs_intptr_t)(tjs_int64)iface_v);
		DrawDevice->SetWindowInterface(const_cast<tTJSNI_BaseWindow*>(this));
		ResetDrawDevice();

		// reassign layermanagers  to drawdevice
		for(std::vector<iTVPLayerManager *>::iterator i = Managers.begin(); i != Managers.end(); i++) {
			DrawDevice->AddLayerManager(*i);
		}
	}
}
//---------------------------------------------------------------------------

void tTJSNI_BaseWindow::SetLayerEventTargetObject(const tTJSVariant & val)
{
	// assign new device
	LayerEventTargetObject = val;
	LayerEventTarget = NULL;

	// extract interface
	if(LayerEventTargetObject.Type() == tvtObject)
	{
		tTJSVariantClosure clo = LayerEventTargetObject.AsObjectClosureNoAddRef();
		tTJSVariant iface_v;
		if(TJS_FAILED(clo.PropGet(0, TJS_W("layerEventTargetInterface"), NULL, &iface_v, NULL)))
			// XXX エラーメッセージ割当
			TVPThrowExceptionMessage(TVPCannotGetLayerEventTargetInterface );
		LayerEventTarget =
			reinterpret_cast<tTVPLayerTreeOwner *>((tjs_intptr_t)(tjs_int64)iface_v);

		// 座標変換用に DrawDeviceと同じDestRectを設定
		if (LayerEventTarget) {
			LayerEventTarget->SetDestRectangle(DestRect);
		}

	}
}

//---------------------------------------------------------------------------

void tTJSNI_BaseWindow::SetDestRectangle(tTVPRect &destRect)
{
	DestRect = destRect;
	if (DrawDevice) {
		DrawDevice->SetDestRectangle(destRect);
	}
	if (LayerEventTarget) {
		LayerEventTarget->SetDestRectangle(destRect);
	}
}

//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnClose()
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[1] = {true};
		static ttstr eventname(TJS_W("onCloseQuery"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnResize()
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		static ttstr eventname(TJS_W("onResize"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 0, NULL);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnClick(tjs_int x, tjs_int y)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardClick(x, y));
	if(Owner)
	{
		tTJSVariant arg[2] = { x, y };
		static ttstr eventname(TJS_W("onClick"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireClick(x, y);
	else if(DrawDevice) DrawDevice->OnClick(x, y);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnDoubleClick(tjs_int x, tjs_int y)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardDoubleClick(x, y));
	if(Owner)
	{
		tTJSVariant arg[2] = { x, y };
		static ttstr eventname(TJS_W("onDoubleClick"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireDoubleClick(x, y);
	else if(DrawDevice) DrawDevice->OnDoubleClick(x, y);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb,
	tjs_uint32 flags)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardMouseDown(x, y, mb, flags));
	if(Owner)
	{
		tTJSVariant arg[4] = { x, y, (tjs_int64)mb, (tjs_int64)flags };
		static ttstr eventname(TJS_W("onMouseDown"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireMouseDown(x, y, mb, flags);
	else if(DrawDevice) DrawDevice->OnMouseDown(x, y, mb, flags);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb,
	tjs_uint32 flags)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardMouseUp(x, y, mb, flags));
	if(Owner)
	{
		tTJSVariant arg[4] = { x, y, (tjs_int)mb, (tjs_int)flags };
		static ttstr eventname(TJS_W("onMouseUp"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireMouseUp(x, y, mb, flags);
	else if(DrawDevice) DrawDevice->OnMouseUp(x, y, mb, flags);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardMouseMove(x, y, flags));
	if(Owner)
	{
		static ttstr eventname(TJS_W("onMouseMove"));
			tTJSVariant arg[3] = { x, y, (tjs_int64)flags };
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_DISCARDABLE|TVP_EPT_IMMEDIATE
			/*discardable!!*/,
			3, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireMouseMove(x, y, flags);
	else if(DrawDevice) DrawDevice->OnMouseMove(x, y, flags);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnTouchDown( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[5] = { x, y, cx, cy, (tjs_int64)id };
		static ttstr eventname(TJS_W("onTouchDown"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 5, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireTouchDown(x, y, cx, cy, id);
	else if(DrawDevice) DrawDevice->OnTouchDown(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnTouchUp( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[5] = { x, y, cx, cy, (tjs_int64)id };
		static ttstr eventname(TJS_W("onTouchUp"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 5, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireTouchUp(x, y, cx, cy, id);
	else if(DrawDevice) DrawDevice->OnTouchUp(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnTouchMove( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[5] = { x, y, cx, cy, (tjs_int64)id };
		static ttstr eventname(TJS_W("onTouchMove"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 5, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireTouchMove(x, y, cx, cy, id);
	else if(DrawDevice) DrawDevice->OnTouchMove(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnPointerDown( tjs_int type, tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 flags, tjs_uint32 id ) {
	if( !CanDeliverEvents() ) return;
	if( Owner )
	{
		tTJSVariant arg[7] = { type, x, y, cx, cy, (tjs_int64)flags, (tjs_int64)id };
		static ttstr eventname( TJS_W( "onPointerDown" ) );
		TVPPostEvent( Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 7, arg );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnPointerMove( tjs_int type, tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 flags, tjs_uint32 id ) {
	if( !CanDeliverEvents() ) return;
	if( Owner )
	{
		tTJSVariant arg[7] = { type, x, y, cx, cy, (tjs_int64)flags, (tjs_int64)id };
		static ttstr eventname( TJS_W( "onPointerMove" ) );
		TVPPostEvent( Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 7, arg );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnPointerUp( tjs_int type, tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 flags, tjs_uint32 id ) {
	if( !CanDeliverEvents() ) return;
	if( Owner )
	{
		tTJSVariant arg[7] = { type, x, y, cx, cy, (tjs_int64)flags, (tjs_int64)id };
		static ttstr eventname( TJS_W( "onPointerUp" ) );
		TVPPostEvent( Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 7, arg );
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnTouchScaling( tjs_real startdist, tjs_real curdist, tjs_real cx, tjs_real cy, tjs_int flag ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[5] = { startdist, curdist, cx, cy, flag };
		static ttstr eventname(TJS_W("onTouchScaling"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 5, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireTouchScaling(startdist, curdist, cx, cy, flag);
	else if(DrawDevice) DrawDevice->OnTouchScaling(startdist, curdist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnTouchRotate( tjs_real startangle, tjs_real curangle, tjs_real dist, tjs_real cx, tjs_real cy, tjs_int flag ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[6] = { startangle, curangle, dist, cx, cy, flag };
		static ttstr eventname(TJS_W("onTouchRotate"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 6, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireTouchRotate(startangle, curangle, dist, cx, cy, flag);
	else if(DrawDevice) DrawDevice->OnTouchRotate(startangle, curangle, dist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMultiTouch() {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		static ttstr eventname(TJS_W("onMultiTouch"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 0, NULL);
	}
	if(LayerEventTarget) LayerEventTarget->FireMultiTouch();
	else if(DrawDevice) DrawDevice->OnMultiTouch();
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnReleaseCapture()
{
	if(!CanDeliverEvents()) return;
	if(LayerEventTarget) LayerEventTarget->FireReleaseCapture();
	else if(DrawDevice) DrawDevice->OnReleaseCapture();
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseOutOfWindow()
{
	if(!CanDeliverEvents()) return;
	if(LayerEventTarget) LayerEventTarget->FireMouseOutOfWindow();
	else if(DrawDevice) DrawDevice->OnMouseOutOfWindow();
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseEnter()
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		static ttstr eventname(TJS_W("onMouseEnter"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 0, NULL);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseLeave()
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		static ttstr eventname(TJS_W("onMouseLeave"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 0, NULL);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnKeyDown(tjs_uint key, tjs_uint32 shift)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardKeyDown(key, shift));
	if(Owner)
	{
		tTJSVariant arg[2] = { (tjs_int)key, (tjs_int)shift };
		static ttstr eventname(TJS_W("onKeyDown"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireKeyDown(key, shift);
	else if(DrawDevice) DrawDevice->OnKeyDown(key, shift);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnKeyUp(tjs_uint key, tjs_uint32 shift)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardKeyUp(key, shift));
	if(Owner)
	{
		tTJSVariant arg[2] = { (tjs_int)key, (tjs_int)shift };
		static ttstr eventname(TJS_W("onKeyUp"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 2, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireKeyUp(key, shift);
	else if(DrawDevice) DrawDevice->OnKeyUp(key, shift);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnKeyPress(tjs_char key)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardKeyPress(key));
	if(Owner)
	{
		tjs_char buf[2];
		buf[0] = (tjs_char)key;
		buf[1] = 0;
		tTJSVariant arg[1] = { buf };
		static ttstr eventname(TJS_W("onKeyPress"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireKeyPress(key);
	else if(DrawDevice) DrawDevice->OnKeyPress(key);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnFileDrop(const tTJSVariant &array)
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[1] = { array };
		static ttstr eventname(TJS_W("onFileDrop"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnMouseWheel(tjs_uint32 shift, tjs_int delta,
	tjs_int x, tjs_int y)
{
	if(!CanDeliverEvents()) return;
	TVP_DIALOG_INTERCEPT(ForwardMouseWheel(shift, delta, x, y));
	if(Owner)
	{
		tTJSVariant arg[4] = { (tjs_int)shift, delta, x, y };
		static ttstr eventname(TJS_W("onMouseWheel"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, arg);
	}
	if(LayerEventTarget) LayerEventTarget->FireMouseWheel(shift, delta, x, y);
	else if(DrawDevice) DrawDevice->OnMouseWheel(shift, delta, x, y);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnPopupHide()
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		static ttstr eventname(TJS_W("onPopupHide"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 0, NULL);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnActivate(bool activate_or_deactivate)
{
	if(!CanDeliverEvents()) return;

	// re-check the window activate state
	if(GetWindowActive() == activate_or_deactivate)
	{
		if(Owner)
		{
			static ttstr a_eventname(TJS_W("onActivate"));
			static ttstr d_eventname(TJS_W("onDeactivate"));
			TVPPostEvent(Owner, Owner, activate_or_deactivate?a_eventname:d_eventname,
				0, TVP_EPT_IMMEDIATE, 0, NULL);
		}
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnHintChange( const ttstr& text, tjs_int x, tjs_int y, bool isshow )
{
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[6] = { text, x, y, isshow ? 1 : 0 };
		static ttstr eventname(TJS_W("onHintChanged"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, arg);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::OnDisplayRotate( tjs_int orientation, tjs_int rotate, tjs_int bpp, tjs_int hresolution, tjs_int vresolution ) {
	if(!CanDeliverEvents()) return;
	if(Owner)
	{
		tTJSVariant arg[5] = { orientation, rotate, bpp, hresolution, vresolution };
		static ttstr eventname(TJS_W("onDisplayRotate"));
		TVPPostEvent(Owner, Owner, eventname, 0, TVP_EPT_IMMEDIATE, 5, arg);
	}
	// このイベントは両方に通知
	if(LayerEventTarget) LayerEventTarget->FireDisplayRotate(orientation, rotate, bpp, hresolution, vresolution);
	if(DrawDevice) DrawDevice->OnDisplayRotate(orientation, rotate, bpp, hresolution, vresolution);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::ClearInputEvents()
{
	TVPCancelInputEvents(this);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::PostReleaseCaptureEvent()
{
	TVPPostInputEvent(
		new tTVPOnReleaseCaptureInputEvent(this));
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BaseWindow::RegisterLayerManager(iTVPLayerManager * manager)
{
	if( DrawDevice ) DrawDevice->AddLayerManager(manager);
	// Window側でも記録する
	Managers.push_back(manager);
	manager->AddRef();
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BaseWindow::UnregisterLayerManager(iTVPLayerManager * manager)
{
	// Managers から manager を削除する。Releaseする。
	std::vector<iTVPLayerManager *>::iterator i = std::find(Managers.begin(), Managers.end(), manager);
	if(i != Managers.end()) {
		(*i)->Release();
		Managers.erase(i);
	}
	if( DrawDevice ) DrawDevice->RemoveLayerManager(manager);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::NotifyWindowExposureToLayer(const tTVPRect &cliprect)
{
	if( DrawDevice ) DrawDevice->RequestInvalidation(cliprect);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::NotifyUpdateRegionFixed(const tTVPComplexRect &updaterects)
{
	// is called by layer manager
	BeginUpdate(updaterects);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::BeginUpdate(const tTVPComplexRect & rects)
{
	WindowUpdating = true;
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::EndUpdate()
{
	WindowUpdating = false;
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::DumpPrimaryLayerStructure()
{
	if( DrawDevice ) DrawDevice->DumpLayerStructure();
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::RecheckInputState()
{
	// slow timer tick (about 1 sec interval, inaccurate)
	// このイベントは両方に通知
	if( LayerEventTarget ) LayerEventTarget->FireRecheckInputState();
	if( DrawDevice ) DrawDevice->RecheckInputState();
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::SetShowUpdateRect(bool b)
{
	// show update rectangle if possible
	if( DrawDevice ) DrawDevice->SetShowUpdateRect(b);
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BaseWindow::RequestUpdate()
{
	// is called from primary layer

	// post update event to self
	TVPPostWindowUpdate((tTJSNI_Window*)this);
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_BaseWindow::NotifySrcResize()
{
	// is called from primary layer
	if(WindowUpdating)
		TVPThrowExceptionMessage(TVPInvalidMethodInUpdating);
}

//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::RegisterVideoOverlayObject(tTJSNI_BaseVideoOverlay * ovl)
{
	VideoOverlay.Add(ovl);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::UnregisterVideoOverlayObject(tTJSNI_BaseVideoOverlay * ovl)
{
	VideoOverlay.Remove(ovl);
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::SetCursorByStorage( const ttstr &storage )
{
	Cursor = TVPGetCursor( storage );
	SetWindowMouseCursor( Cursor );
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::SetCursorByNumber( tjs_int num )
{
	Cursor = num;
	SetWindowMouseCursor( Cursor );
}
//---------------------------------------------------------------------------



//---- methods

//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::Add(tTJSVariantClosure clo)
{
	if(ObjectVectorLocked) return;
	if(ObjectVector.end() == std::find(ObjectVector.begin(), ObjectVector.end(), clo))
	{
		ObjectVector.push_back(clo);
		clo.AddRef();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::Remove(tTJSVariantClosure clo)
{
	if(ObjectVectorLocked) return;
	std::vector<tTJSVariantClosure>::iterator i;
	i = std::find(ObjectVector.begin(), ObjectVector.end(), clo);
	if(i != ObjectVector.end())
	{
		clo.Release();
		ObjectVector.erase(i);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_BaseWindow::SetWaitVSync( bool enable )
{
	WaitVSync = enable;
	UpdateWaitVSync();
	TVPUpdateContinuousEvent();
}
//---------------------------------------------------------------------------
bool tTJSNI_BaseWindow::GetWaitVSync() const
{
	return WaitVSync;
}

//---------------------------------------------------------------------------
// ビューポート fit 名 ⇔ tTVPViewportFit 変換
//---------------------------------------------------------------------------
static tjs_int TVPViewportFitFromString(const ttstr &s)
{
	if(s == TJS_W("contain")) return vfContain;
	if(s == TJS_W("cover"))   return vfCover;
	if(s == TJS_W("fill"))    return vfFill;
	if(s == TJS_W("none"))    return vfNone;
	if(s == TJS_W("integer")) return vfInteger;
	if(s == TJS_W("custom"))  return vfCustom;
	return vfContain;
}
static const tjs_char * TVPViewportFitToString(tjs_int f)
{
	switch(f) {
	case vfCover:   return TJS_W("cover");
	case vfFill:    return TJS_W("fill");
	case vfNone:    return TJS_W("none");
	case vfInteger: return TJS_W("integer");
	case vfCustom:  return TJS_W("custom");
	case vfContain:
	default:        return TJS_W("contain");
	}
}

//---------------------------------------------------------------------------
// tTJSNC_Window : TJS Window class
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_Window::ClassID = -1;
tTJSNC_Window::tTJSNC_Window() : tTJSNativeClass(TJS_W("Window"))
{
	// registration of native members

	TJS_BEGIN_NATIVE_MEMBERS(Window) // constructor
	TJS_DECL_EMPTY_FINALIZE_METHOD
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(/*var.name*/_this, /*var.type*/tTJSNI_Window,
	/*TJS class name*/Window)
{
	return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/Window)
//----------------------------------------------------------------------




//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/close)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->Close();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/close)
//----------------------------------------------------------------------
#ifdef USE_OBSOLETE_FUNCTIONS
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/beginMove)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->BeginMove();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/beginMove)
#endif
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/bringToFront)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->BringToFront();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/bringToFront)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/update)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	tTVPUpdateType type = utNormal;
	if(numparams >= 2 && param[1]->Type() != tvtVoid)
		type = (tTVPUpdateType)(tjs_int)*param[0];
	_this->Update(type);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/update)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(GLGetProcAddress)
{
	// GLES 系プラグイン (EffekseerDevice 等) の oglbase として使う、GL エントリ
	// ポイント解決関数のポインタ。初回アクセス時に GL コンテキストが未初期化なら
	// 遅延生成してカレントにする (OGLDrawDevice を使わない状況でも取得可能)。
	// GL を取得できない環境では 0 (null) を返す。
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		tjs_int64 ret = 0;
#ifdef TVP_USE_OPENGL
		try {
			if( !_this->PluginGLContext ) {
#ifdef __WINVER__
				void *nw = _this->GetForm() ? _this->GetForm()->GetHandle() : nullptr;
#else
				void *nw = _this->GetForm() ? _this->GetForm()->NativeWindowHandle() : nullptr;
#endif
				iTVPGLContext *ctx = nw ? iTVPGLContext::GetContext( nw ) : nullptr;
				if( ctx ) {
					ctx->MakeCurrent();
					InitGLES();
					_this->PluginGLContext = ctx;
				}
			} else {
				_this->PluginGLContext->MakeCurrent();
			}
			if( _this->PluginGLContext )
				ret = (tjs_int64)(void*)( &TVPGLGetProcAddress );
		} catch( ... ) {
			ret = 0;  // GL 非対応環境等では null
		}
#endif
		*result = ret;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(GLGetProcAddress)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/requestUpdate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->RequestUpdate();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/requestUpdate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/showModal)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->ShowModal();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/showModal)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setMaskRegion)
{
#ifdef __WINVER__
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	tjs_int threshold = 1;
	if(numparams >= 1 && param[0]->Type() != tvtVoid)
		threshold = (tjs_int)*param[0];
	_this->SetMaskRegion(threshold);
	return TJS_S_OK;
#else
	return TJS_E_NOTIMPL;
#endif
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setMaskRegion)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/removeMaskRegion)
{
#ifdef __WINVER__
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->RemoveMaskRegion();
	return TJS_S_OK;
#else
	return TJS_E_NOTIMPL;
#endif
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/removeMaskRegion)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/add)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();
	_this->Add(clo);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/add)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/remove)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tTJSVariantClosure clo = param[0]->AsObjectClosureNoAddRef();
	_this->Remove(clo);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/remove)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setSize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetSize(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setSize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setMinSize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetMinSize(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setMinSize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setMaxSize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetMaxSize(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setMaxSize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setPos) // not setPosition
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetPosition(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setPos)
//----------------------------------------------------------------------
#ifdef USE_OBSOLETE_FUNCTIONS
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setLayerPos) // not setLayerPosition
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetLayerPosition(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setLayerPos)
#endif
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setInnerSize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetInnerSize(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setInnerSize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setZoom)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	if(numparams < 2) return TJS_E_BADPARAMCOUNT;
	_this->SetZoom(*param[0], *param[1]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setZoom)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/hideMouseCursor)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
	_this->HideMouseCursor();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/hideMouseCursor)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/postInputEvent)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	if(numparams < 1)  return TJS_E_BADPARAMCOUNT;
	ttstr eventname;
	iTJSDispatch2 * eventparams = NULL;

	eventname = *param[0];
	if(numparams >= 2) eventparams = param[1]->AsObjectNoAddRef();

	_this->PostInputEvent(eventname, eventparams);

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/postInputEvent)
//----------------------------------------------------------------------



//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onResize)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onResize", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onResize)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseEnter)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onMouseEnter", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseEnter)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseLeave)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onMouseLeave", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseLeave)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onClick)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(2, "onClick", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onClick)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onDoubleClick)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(2, "onDoubleClick", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onDoubleClick)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseDown)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(4, "onMouseDown", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("button");
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseDown)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseUp)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(4, "onMouseUp", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("button");
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseUp)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseMove)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(3, "onMouseMove", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseMove)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMouseWheel)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(4, "onMouseWheel", objthis);
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_MEMBER("delta");
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMouseWheel)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onTouchDown)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(5, "onTouchDown", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("cx");
	TVP_ACTION_INVOKE_MEMBER("cy");
	TVP_ACTION_INVOKE_MEMBER("id");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onTouchDown)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onTouchUp)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(5, "onTouchUp", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("cx");
	TVP_ACTION_INVOKE_MEMBER("cy");
	TVP_ACTION_INVOKE_MEMBER("id");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onTouchUp)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onTouchMove)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(5, "onTouchMove", objthis);
	TVP_ACTION_INVOKE_MEMBER("x");
	TVP_ACTION_INVOKE_MEMBER("y");
	TVP_ACTION_INVOKE_MEMBER("cx");
	TVP_ACTION_INVOKE_MEMBER("cy");
	TVP_ACTION_INVOKE_MEMBER("id");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onTouchMove)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onTouchScaling)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(5, "onTouchScaling", objthis);
	TVP_ACTION_INVOKE_MEMBER("startdistance");
	TVP_ACTION_INVOKE_MEMBER("currentdistance");
	TVP_ACTION_INVOKE_MEMBER("cx");
	TVP_ACTION_INVOKE_MEMBER("cy");
	TVP_ACTION_INVOKE_MEMBER("flag");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onTouchScaling)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onTouchRotate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(6, "onTouchRotate", objthis);
	TVP_ACTION_INVOKE_MEMBER("startangle");
	TVP_ACTION_INVOKE_MEMBER("currentangle");
	TVP_ACTION_INVOKE_MEMBER("distance");
	TVP_ACTION_INVOKE_MEMBER("cx");
	TVP_ACTION_INVOKE_MEMBER("cy");
	TVP_ACTION_INVOKE_MEMBER("flag");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onTouchRotate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onMultiTouch)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onMultiTouch", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onMultiTouch)

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onKeyDown)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(2, "onKeyDown", objthis);
	TVP_ACTION_INVOKE_MEMBER("key");
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onKeyDown)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onKeyUp)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(2, "onKeyUp", objthis);
	TVP_ACTION_INVOKE_MEMBER("key");
	TVP_ACTION_INVOKE_MEMBER("shift");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onKeyUp)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onKeyPress)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(1, "onKeyPress", objthis);
	TVP_ACTION_INVOKE_MEMBER("key");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onKeyPress)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onFileDrop)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(1, "onFileDrop", objthis);
	TVP_ACTION_INVOKE_MEMBER("files");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onFileDrop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onCloseQuery)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
/*
	// this event does not call "action" method
	TVP_ACTION_INVOKE_BEGIN(1, "onCloseQuery", objthis);
	TVP_ACTION_INVOKE_MEMBER("canClose");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));
*/
	_this->OnCloseQueryCalled( 0 != (tjs_int)*param[0]);

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onCloseQuery)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onPopupHide)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onPopupHide", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onPopupHide)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onActivate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onActivate", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onActivate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onDeactivate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(0, "onDeactivate", objthis);
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onDeactivate)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/onDisplayRotate)
{
	TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

	TVP_ACTION_INVOKE_BEGIN(5, "onDisplayRotate", objthis);
	TVP_ACTION_INVOKE_MEMBER("orientation");
	TVP_ACTION_INVOKE_MEMBER("angle");
	TVP_ACTION_INVOKE_MEMBER("bpp");
	TVP_ACTION_INVOKE_MEMBER("width");
	TVP_ACTION_INVOKE_MEMBER("height");
	TVP_ACTION_INVOKE_END(tTJSVariantClosure(objthis, objthis));

	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/onDisplayRotate)
//----------------------------------------------------------------------

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(visible)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetVisible();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetVisible(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(visible)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(caption)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		ttstr res;
		_this->GetCaption(res);
		*result = res;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetCaption(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(caption)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(width)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetWidth(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(width)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(height)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetHeight(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(height)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(minWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetMinWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetMinWidth(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(minWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(minHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetMinHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetMinHeight(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(minHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(maxWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetMaxWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetMaxWidth(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(maxWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(maxHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetMaxHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetMaxHeight(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(maxHeight)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(left)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetLeft();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetLeft(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(left)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(top)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetTop();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetTop(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(top)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(focusable)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = (tjs_int) _this->GetFocusable();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetFocusable( 0 != (tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(focusable)
//----------------------------------------------------------------------
#ifdef USE_OBSOLETE_FUNCTIONS
TJS_BEGIN_NATIVE_PROP_DECL(layerLeft)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetLayerLeft();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetLayerLeft(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(layerLeft)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(layerTop)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetLayerTop();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetLayerTop(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(layerTop)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(innerSunken)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetInnerSunken();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetInnerSunken(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(innerSunken)
#endif
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(innerWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetInnerWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetInnerWidth(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(innerWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(innerHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetInnerHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetInnerHeight(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(innerHeight)
//----------------------------------------------------------------------
// 装飾ぶんのサイズ (width - innerWidth / height - innerHeight)。
// 枠を持たない環境 (モバイル / コンソール / ボーダレス) では 0。
// outer 基準で書かれたコードを inner 基準へ移植するための橋渡し。
TJS_BEGIN_NATIVE_PROP_DECL(frameWidth)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetFrameWidth();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(frameWidth)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(frameHeight)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetFrameHeight();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(frameHeight)
//----------------------------------------------------------------------
// ビューポート (ゲーム画面の表示画角制御)。外側 surface (innerWidth/Height)
// 内に内側ゲーム (primaryLayer) を fit/zoom/align/offset で配置する。
// 配置計算は全バリアント共通 (src/core/doc/WindowGeometry.md §3 の「軸B」)。
// 余白の背景色/壁紙だけは DrawDevice 実装が要るので下の方で Generic 限定。
//----------------------------------------------------------------------
//----------------------------------------------------------------------
// 画面比率の固定。 "16:9" のような文字列で指定し、 "" で解除。
// 有効にするとウィンドウのリサイズがこの比率へ拘束される (デスクトップ SDL3)。
// ゲーム画面自体の配置は viewport が行うので、 レイヤ比率とは独立。
TJS_BEGIN_NATIVE_PROP_DECL(aspectLock)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		tjs_int w = _this->GetAspectLockW(), h = _this->GetAspectLockH();
		if(w <= 0 || h <= 0) { *result = ttstr(); return TJS_S_OK; }
		ttstr s(w); s += TJS_W(":"); s += ttstr(h);
		*result = s;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		ttstr s(*param);
		tjs_int w = 0, h = 0;
		const tjs_char *p = s.c_str();
		if(p) {
			// "W:H" / "W/H" を読む。 数字以外は区切りとして扱う。
			while(*p >= TJS_W('0') && *p <= TJS_W('9')) { w = w * 10 + (*p - TJS_W('0')); p++; }
			if(*p) p++;
			while(*p >= TJS_W('0') && *p <= TJS_W('9')) { h = h * 10 + (*p - TJS_W('0')); p++; }
		}
		if(w <= 0 || h <= 0) { w = 0; h = 0; }
		_this->SetAspectLock(w, h);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(aspectLock)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportFit)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = ttstr(TVPViewportFitToString(_this->GetViewportFit()));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportFit(TVPViewportFitFromString(ttstr(*param)));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportFit)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportZoom)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = (tjs_real)_this->GetViewportZoom();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportZoom((double)(tjs_real)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportZoom)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportAlignX)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = (tjs_real)_this->GetViewportAlignX();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportAlignX((double)(tjs_real)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportAlignX)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportAlignY)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = (tjs_real)_this->GetViewportAlignY();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportAlignY((double)(tjs_real)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportAlignY)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportOffsetX)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = _this->GetViewportOffsetX();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportOffsetX((tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportOffsetX)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(viewportOffsetY)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = _this->GetViewportOffsetY();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportOffsetY((tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportOffsetY)
//----------------------------------------------------------------------
// 余白の背景色 / 壁紙。配置計算と同じく全バリアント共通。
TJS_BEGIN_NATIVE_PROP_DECL(viewportBgColor)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		*result = (tjs_int64)(tjs_uint32)_this->GetViewportBgColor();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER
	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
		_this->SetViewportBgColor((tjs_uint32)(tjs_int64)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(viewportBgColor)
//----------------------------------------------------------------------
// setViewportWallpaper(image [, fit="cover" [, alignX=0.5 [, alignY=0.5]]])
//   image: ストレージ名 (文字列) または Layer / Bitmap オブジェクト。
//          オブジェクトは Variant のまま参照保持され、描画デバイスが
//          imageWidth/imageHeight/mainImageBuffer/mainImageBufferPitch から取得する。
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setViewportWallpaper)
{
	TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	tjs_int fit = vfCover;
	if(numparams >= 2 && param[1]->Type() != tvtVoid)
		fit = TVPViewportFitFromString(ttstr(*param[1]));
	double ax = (numparams >= 3 && param[2]->Type() != tvtVoid) ? (double)(tjs_real)*param[2] : 0.5;
	double ay = (numparams >= 4 && param[3]->Type() != tvtVoid) ? (double)(tjs_real)*param[3] : 0.5;
	_this->SetViewportWallpaper(*param[0], fit, ax, ay);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setViewportWallpaper)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/clearViewportWallpaper)
{
	TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
	_this->ClearViewportWallpaper();
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/clearViewportWallpaper)
//----------------------------------------------------------------------
// setViewport(fit [, zoom [, alignX [, alignY [, offsetX [, offsetY]]]]])
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/setViewport)
{
	TJS_GET_NATIVE_INSTANCE(_this, tTJSNI_Window);
	if(numparams < 1) return TJS_E_BADPARAMCOUNT;
	_this->SetViewportFit(TVPViewportFitFromString(ttstr(*param[0])));
	if(numparams >= 2 && param[1]->Type() != tvtVoid) _this->SetViewportZoom((double)(tjs_real)*param[1]);
	if(numparams >= 3 && param[2]->Type() != tvtVoid) _this->SetViewportAlignX((double)(tjs_real)*param[2]);
	if(numparams >= 4 && param[3]->Type() != tvtVoid) _this->SetViewportAlignY((double)(tjs_real)*param[3]);
	if(numparams >= 5 && param[4]->Type() != tvtVoid) _this->SetViewportOffsetX((tjs_int)*param[4]);
	if(numparams >= 6 && param[5]->Type() != tvtVoid) _this->SetViewportOffsetY((tjs_int)*param[5]);
	return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/setViewport)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(zoomNumer)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetZoomNumer();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetZoomNumer(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(zoomNumer)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(zoomDenom)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetZoomDenom();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetZoomDenom(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(zoomDenom)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(borderStyle)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = (tjs_int)_this->GetBorderStyle();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetBorderStyle((tTVPBorderStyle)(tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(borderStyle)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(stayOnTop)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetStayOnTop();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetStayOnTop(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(stayOnTop)
//----------------------------------------------------------------------
#ifdef USE_OBSOLETE_FUNCTIONS
TJS_BEGIN_NATIVE_PROP_DECL(showScrollBars)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetShowScrollBars();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetShowScrollBars(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(showScrollBars)
#endif
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(useMouseKey)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetUseMouseKey();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetUseMouseKey(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(useMouseKey)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(trapKey)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetTrapKey();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetTrapKey(param->operator bool());
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(trapKey)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(imeMode) // not defaultImeMode
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
#ifdef __WINVER__
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = (tjs_int)_this->GetDefaultImeMode();
		return TJS_S_OK;
#else
		return TJS_E_NOTIMPL;
#endif
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
#ifdef __WINVER__
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetDefaultImeMode((tTVPImeMode)(tjs_int)*param);
		return TJS_S_OK;
#else
		return TJS_E_NOTIMPL;
#endif
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(imeMode)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(mouseCursorState)
{
	// generic (SDL) 側も tTJSNI_Window::Get/SetMouseCursorState を実装済み
	// (SetCursorVisible → SDL_Hide/ShowCursor) なので WINVER 限定を撤廃。
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = (tjs_int)_this->GetMouseCursorState();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetMouseCursorState((tTVPMouseCursorState)(tjs_int)*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(mouseCursorState)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(fullScreen)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetFullScreen();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetFullScreen( 0 != (tjs_int)*param );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(fullScreen)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(mainWindow) /* static */
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		if(TVPMainWindow)
		{
			iTJSDispatch2 *dsp = TVPMainWindow->GetOwnerNoAddRef();
			*result = tTJSVariant(dsp, dsp);
		}
		else
		{
			*result = tTJSVariant((iTJSDispatch2*)NULL);
		}
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(mainWindow)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(focusedLayer)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		tTVPLayerTreeOwner *target = _this->GetLayerEventTarget();
		tTJSNI_BaseLayer *lay = target ? target->GetFocusedLayer() : _this->GetDrawDevice()->GetFocusedLayer();
		if(lay && lay->GetOwnerNoAddRef())
			*result = tTJSVariant(lay->GetOwnerNoAddRef(), lay->GetOwnerNoAddRef());
		else
			*result = tTJSVariant((iTJSDispatch2*)NULL);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);

		tTJSNI_BaseLayer *to = NULL;

		if(param->Type() != tvtVoid)
		{
			tTJSVariantClosure clo = param->AsObjectClosureNoAddRef();
			if(clo.Object)
			{
				if(TJS_FAILED(clo.Object->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
					tTJSNC_Layer::ClassID, (iTJSNativeInstance**)&to)))
					TVPThrowExceptionMessage(TVPSpecifyLayer);
			}
		}

		tTVPLayerTreeOwner *target = _this->GetLayerEventTarget();
		if (target) {
			target->SetFocusedLayer(to);
		} else {
			_this->GetDrawDevice()->SetFocusedLayer(to);
		}

		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(focusedLayer)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(primaryLayer)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		tTJSNI_BaseLayer *pri = _this->GetDrawDevice()->GetPrimaryLayer();
		if(!pri) TVPThrowExceptionMessage(TVPWindowHasNoLayer);

		if(pri && pri->GetOwnerNoAddRef())
			*result = tTJSVariant(pri->GetOwnerNoAddRef(), pri->GetOwnerNoAddRef());
		else
			*result = tTJSVariant((iTJSDispatch2*)NULL);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(primaryLayer)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(waitVSync)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetWaitVSync() ? 1 : 0;
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetWaitVSync( ((tjs_int)*param) ? true : false );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(waitVSync)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(layerTreeOwnerInterface)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = reinterpret_cast<tjs_int64>(static_cast<iTVPLayerTreeOwner*>(_this));
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(layerTreeOwnerInterface)
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(layerEventTarget)
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		*result = _this->GetLayerEventTargetObject();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window);
		_this->SetLayerEventTargetObject(*param);
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(layerEventTarget)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL( displayDensity )
{
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		*result = Application->GetDensity();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_DENY_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL( displayDensity )
//---------------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL( mouseCursor ) {
	TJS_BEGIN_NATIVE_PROP_GETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window );
		*result = _this->GetCursor();
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_GETTER

	TJS_BEGIN_NATIVE_PROP_SETTER
	{
		TJS_GET_NATIVE_INSTANCE(/*var. name*/_this, /*var. type*/tTJSNI_Window );
		if( param->Type() == tvtString )
			_this->SetCursorByStorage( *param );
		else
			_this->SetCursorByNumber( *param );
		return TJS_S_OK;
	}
	TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL( mouseCursor )
//----------------------------------------------------------------------
	

	TJS_END_NATIVE_MEMBERS

}
//---------------------------------------------------------------------------

