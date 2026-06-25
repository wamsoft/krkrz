
#include "tjsCommHead.h"

#include <cstring>
#include "WindowForm.h"
#include "Application.h"
#include "TickCount.h"
#include "Random.h"
#include "MsgImpl.h"
#include "LogIntf.h"
#include "VideoOvlIntf.h"
#include "PluginImpl.h"

// Androidでは変換しない, ssShiftなどで統一的に扱っている。
tjs_uint32 TVP_TShiftState_To_uint32(TShiftState state) { return (tjs_uint32)state; }
TShiftState TVP_TShiftState_From_uint32(tjs_uint32 state){ return (TShiftState)state; }

// マウスのステート取得
int GetMouseButtonState() {
	int s = 0;
#if 0
	if(TVPGetAsyncKeyState(VK_LBUTTON)) s |= ssLeft;
	if(TVPGetAsyncKeyState(VK_RBUTTON)) s |= ssRight;
	if(TVPGetAsyncKeyState(VK_MBUTTON)) s |= ssMiddle;
#endif
	return s;
}

TTVPWindowForm::TTVPWindowForm( class tTJSNI_Window* ni )
 : LastMouseDownX(0)
 , LastMouseDownY(0)
 , touch_points_(this)
 , TJSNativeInstance(ni)
 , mcs_(mcsVisible)
 , mSurfaceWidth(0)
 , mSurfaceHeight(0)
 , mCursorX(0)
 , mCursorY(0)
 , mViewportBgColor(0xff000000)
 , mViewportWallpaperFit(vfCover)
 , mViewportWpAlignX(0.5)
 , mViewportWpAlignY(0.5)
 , mViewportRenderDirty(true)
 {
	Application->addEventHandler(this);
	Application->AddWindow(this);

	Closing = false;
	ProgramClosing = false;
	CanCloseWork = false;
}

TTVPWindowForm::~TTVPWindowForm() {
	// 登録された Window メッセージレシーバーを破棄
	tjs_int count = WindowMessageReceivers.GetCount();
	for( tjs_int i = 0; i < count; i++ ) {
		tTVPMessageReceiverRecord *item = WindowMessageReceivers[i];
		if( !item ) continue;
		delete item;
		WindowMessageReceivers.Remove(i);
	}

	Application->removeEventHandler(this);
	Application->DelWindow(this);
}

// プラグイン向け WindowMessageReceiver chain への配信。
// receiver が true (block) を返したら true (= 以降のデフォルト処理をスキップ)。
bool TTVPWindowForm::DeliverToReceiver( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) {
	if( WindowMessageReceivers.GetCount() > 0 ) {
		tTVPWindowMessage msg;
		msg.Msg = (tjs_uint32)message;
		msg.WParam = wparam;
		msg.LParam = lparam;
		msg.Result = 0;
		if( InternalDeliverMessageToReceiver(msg) ) {
			// Result は SendAppEvent 経由なので呼び出し元には届かない
			return true;
		}
	}
	return false;
}

// AppEventInterface: 非同期通知系メッセージの処理 (メインスレッド)。
// 入力系 (touch/mouse) は SendTouchMessage/SendMouseMessage で同期処理されるため
// ここには来ない。自分が処理したメッセージなら true を返す。
bool TTVPWindowForm::Dispatch( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) {
	if( DeliverToReceiver( message, wparam, lparam ) ) return true;

	switch( message ) {

	case AM_RESUME:
		OnResume();
		return true;
	case AM_PAUSE:
		OnPause();
		return true;

	case AM_SURFACE_CHANGED:
		// Surfaceが切り替わった
		if( TJSNativeInstance ) {
			TJSNativeInstance->ResetDrawDevice();
			TJSNativeInstance->Update();
		}
		return true;
	case AM_SURFACE_CREATED:
		return true;
	case AM_SURFACE_DESTORYED:
		if( TJSNativeInstance ) {
			TJSNativeInstance->ResetDrawDevice();
		}
		return true;
	case AM_SURFACE_PAINT_REQUEST:
		if( TJSNativeInstance ) {
			TJSNativeInstance->Update();
		}
		return true;

	case AM_REQUEST_UPDATE:
		if( TJSNativeInstance ) {
			TJSNativeInstance->RequestUpdate();
		}
		return true;

	case AM_KEY_DOWN:
		OnKeyDown( (tjs_int)wparam, (int)lparam );
		return true;
	case AM_KEY_UP:
		OnKeyUp( (tjs_int)wparam, (int)lparam );
		return true;

	case AM_DISPLAY_ROTATE:
		OnDisplayRotate( (tjs_int)wparam, (tjs_int)lparam );
		return true;

	case AM_DISPLAY_RESIZE:
		OnResize();
		return true;

	default:
		break;
	}
	return false;
}

void TTVPWindowForm::OnClose()
{
	// オブジェクト破棄
	if ( ProgramClosing ) {
		if( TJSNativeInstance ) {
			// Window を Invalidate 処理
			iTJSDispatch2 * obj = TJSNativeInstance->GetOwnerNoAddRef();
			TJSNativeInstance->NotifyWindowClose();
			obj->Invalidate(0, NULL, NULL, obj);
			TJSNativeInstance = NULL;
		}
	}
}

bool TTVPWindowForm::OnCloseQuery() {
	// closing actions are 3 patterns;
	// 1. closing action by the user
	// 2. "close" method
	// 3. object invalidation

	if( TVPGetBreathing() ) {
		return false;
	}

	// the default event handler will invalidate this object when an onCloseQuery
	// event reaches the handler.
	if(TJSNativeInstance) {
		iTJSDispatch2 * obj = TJSNativeInstance->GetOwnerNoAddRef();
		if(obj) {
			tTJSVariant arg[1] = {true};
			static ttstr eventname(TJS_W("onCloseQuery"));

			if(!ProgramClosing) {
				// close action does not happen immediately
				if(TJSNativeInstance) {
					TVPPostInputEvent( new tTVPOnCloseInputEvent(TJSNativeInstance) );
				}
				Closing = true; // waiting closing...
				return false;
			} else {
				CanCloseWork = true;
				TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
					// this event happens immediately
					// and does not return until done
				return CanCloseWork; // CanCloseWork is set by the event handler
			}
		} else {
			return true;
		}
	} else {
		return true;
	}
}

void TTVPWindowForm::OnCloseQueryCalled( bool b ) 
{
	// closing is allowed by onCloseQuery event handler
	if( !ProgramClosing ) {
		// closing action by the user
		if( b ) {
			SetVisible( false );  // just hide

			Closing = false;
			if( TJSNativeInstance ) {
				if( TJSNativeInstance->IsMainWindow() ) {
					// this is the main window
					iTJSDispatch2 * obj = TJSNativeInstance->GetOwnerNoAddRef();
					obj->Invalidate(0, NULL, NULL, obj);
					// TJSNativeInstance = NULL; // この段階では既にthisが削除されているため、メンバーへアクセスしてはいけない
				}
			} else {
				delete this;
			}
		} else {
			Closing = false;
		}
	} else {
		// closing action by the program
		CanCloseWork = b;
	}
}

void TTVPWindowForm::Close() 
{
	// closing action by "close" method
	if( Closing ) return; // already waiting closing...

	ProgramClosing = true;
	try {
		if (OnCloseQuery() ) {
			OnClose();
		} else {
			OnCloseCancel();
		}
	} catch(...) {
		ProgramClosing = false;
		throw;
	}
	ProgramClosing = false;
}


void TTVPWindowForm::InvalidateClose() 
{
	// closing action by object invalidation;
	// this will not cause any user confirmation of closing the window.
	TJSNativeInstance = nullptr;
	SetVisible(false);
	DestroyNativeWindow();
	delete this;
}

// 定期的に呼び出されるので、定期処理があれば実行する
void TTVPWindowForm::TickBeat() {
}

// キー入力
void TTVPWindowForm::OnKeyDown( tjs_int vk, int shift ) {
	InternalKeyDown( vk, shift );
}

void TTVPWindowForm::InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) {
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tick));
	TVPPushEnvironNoise(&key, sizeof(key));
	TVPPushEnvironNoise(&shift, sizeof(shift));
	if( TJSNativeInstance ) {
		TVPPostInputEvent(new tTVPOnKeyDownInputEvent(TJSNativeInstance, key, shift));
	}
}

void TTVPWindowForm::OnKeyUp( tjs_int vk, int shift ) {
	InternalKeyUp( vk, shift );
}

void TTVPWindowForm::InternalKeyUp( tjs_uint16 key, tjs_uint32 shift ) {
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tick));
	TVPPushEnvironNoise(&key, sizeof(key));
	TVPPushEnvironNoise(&shift, sizeof(shift));
	if( TJSNativeInstance ) {
		TVPPostInputEvent(new tTVPOnKeyUpInputEvent(TJSNativeInstance, key, shift));
	}
}
void TTVPWindowForm::OnKeyPress( tjs_int vk, int repeat, bool prevkeystate, bool convertkey ) {
}

tTVPRect
TTVPWindowForm::CalcDestRect(int w, int h)
{
    // 外側 surface (mSurfaceWidth/Height = innerWidth/innerHeight) 内へ
    // 内側ゲーム (w,h = プライマリレイヤ) を mViewport の指定で配置・スケール。
    tTVPRect r = TVPCalcViewportDestRect(mViewport, mSurfaceWidth, mSurfaceHeight, w, h);
    TVPLOG_VERBOSE("viewport surface:{},{} layer:{},{} dest:{},{},{},{}",
        mSurfaceWidth, mSurfaceHeight, w, h, r.left, r.top, r.right, r.bottom);
    return r;
}

//---------------------------------------------------------------------------
void TTVPWindowForm::NotifyViewportDestRectChanged()
{
    // 配置/スケールが変わったので DestRect を再計算させる。
    if (TJSNativeInstance) TJSNativeInstance->SetUpdateDestRect();
}

//---------------------------------------------------------------------------
void TTVPWindowForm::SetViewportConfig(const tTVPViewportConfig &cfg)
{
    mViewport = cfg;
    NotifyViewportDestRectChanged();
}

//---------------------------------------------------------------------------
void TTVPWindowForm::SetViewportBgColor(tjs_uint32 color)
{
    mViewportBgColor = color;
    mViewportRenderDirty = true;
}

//---------------------------------------------------------------------------
void TTVPWindowForm::SetViewportWallpaper(const tTJSVariant &image,
    tTVPViewportFit fit, double alignX, double alignY)
{
    // tTJSVariant がオブジェクト参照を保持するのでイメージデータは維持される。
    if (image.Type() == tvtObject && image.AsObjectNoAddRef()) {
        mViewportWallpaper = image;
    } else {
        mViewportWallpaper.Clear();
    }
    mViewportWallpaperFit = fit;
    mViewportWpAlignX = alignX;
    mViewportWpAlignY = alignY;
    mViewportRenderDirty = true;
}

void 
TTVPWindowForm::TranslateWindowToDrawArea(int &x, int &y) 
{
	if (TJSNativeInstance) {
		tTVPRect &destRect = TJSNativeInstance->GetDestRect();
		x -= destRect.left;
		y -= destRect.top;
	}
}

void 
TTVPWindowForm::TranslateWindowToDrawArea(float &x, float &y) 
{
	if (TJSNativeInstance) {
		tTVPRect &destRect = TJSNativeInstance->GetDestRect();
		x -= destRect.left;
		y -= destRect.top;
	}
}
void 
TTVPWindowForm::TranslateDrawAreaToWindow(int &x, int &y) 
{
	if (TJSNativeInstance) {
		tTVPRect &destRect = TJSNativeInstance->GetDestRect();
		x += destRect.left;
		y += destRect.top;
	}
}

void TTVPWindowForm::OnMouseDown( int button, int shift, int x, int y ) {

	//if( !CanSendPopupHide() ) DeliverPopupHide();

	TranslateWindowToDrawArea( x, y);
	//SetMouseCapture();
	MouseVelocityTracker.addMovement( TVPGetRoughTickCount32(), (float)x, (float)y );

	LastMouseDownX = x;
	LastMouseDownY = y;

	if(TJSNativeInstance) {
		tjs_uint32 s = TVP_TShiftState_To_uint32(shift);
		s |= GetMouseButtonState();
		TVPPostInputEvent( new tTVPOnMouseDownInputEvent(TJSNativeInstance, x, y, (tTVPMouseButton)button, s));
	}
}

void TTVPWindowForm::OnMouseUp( int button, int shift, int x, int y ) {
	TranslateWindowToDrawArea(x, y);
	//ReleaseMouseCapture();
	MouseVelocityTracker.addMovement( TVPGetRoughTickCount32(), (float)x, (float)y );
	if(TJSNativeInstance) {
		tjs_uint32 s = TVP_TShiftState_To_uint32(shift);
		s |= GetMouseButtonState();
		TVPPostInputEvent( new tTVPOnMouseUpInputEvent(TJSNativeInstance, x, y, (tTVPMouseButton)button, s));
	}
}

void TTVPWindowForm::OnMouseMove( int shift, int x, int y ) {
	TranslateWindowToDrawArea(x, y);
	MouseVelocityTracker.addMovement( TVPGetRoughTickCount32(), (float)x, (float)y );
	if( TJSNativeInstance ) {
		tjs_uint32 s = TVP_TShiftState_To_uint32(shift);
		s |= GetMouseButtonState();
		TVPPostInputEvent( new tTVPOnMouseMoveInputEvent(TJSNativeInstance, x, y, s), TVP_EPT_DISCARDABLE );
	}

	//RestoreMouseCursor();

	int pos = (y << 16) + x;
	TVPPushEnvironNoise(&pos, sizeof(pos));

	//LastMouseMovedPos.x = x;
	//LastMouseMovedPos.y = y;
}

void TTVPWindowForm::OnMouseWheel( int delta, int shift, int x, int y ) {
	TranslateWindowToDrawArea( x, y);
	// wheel
	if( TJSNativeInstance ) {
		tjs_uint32 s = TVP_TShiftState_To_uint32(shift);
		s |= GetMouseButtonState();
		TVPPostInputEvent(new tTVPOnMouseWheelInputEvent(TJSNativeInstance, s, delta, x, y));
	}
}

void 
TTVPWindowForm::HideMouseCursor() 
{
	SetMouseCursorState(mcsTempHidden); 
}

void
TTVPWindowForm::SetMouseCursorState(tTVPMouseCursorState mcs) 
{
	if (mcs != mcs_) {
		mcs_ = mcs;
		SetCursorVisible(mcs_ == mcsVisible);
	}
}

tTVPMouseCursorState 
TTVPWindowForm::GetMouseCursorState() const 
{
	return mcs_; 
}

void
TTVPWindowForm::GetCursorPos(tjs_int &x, tjs_int &y)
{
    x = mCursorX;
    y = mCursorY;
}

void
TTVPWindowForm::SetCursorPos(tjs_int x, tjs_int y)
{
    mCursorX = x;
    mCursorY = y;
}

void
TTVPWindowForm::UpdateCursorPos(tjs_int x, tjs_int y)
{
	// カーソル表示動作
	if (GetMouseCursorState() == mcsTempHidden) {
		SetMouseCursorState(mcsVisible);
	}
	// イベントでおりかえす
	if( TJSNativeInstance ) {
		//tjs_uint32 s = TVP_TShiftState_To_uint32(shift);
		//s |= GetMouseButtonState();
		TVPPostInputEvent( new tTVPOnMouseMoveInputEvent(TJSNativeInstance, x, y, 0), TVP_EPT_DISCARDABLE );
	}
}


// 表示/非表示
void TTVPWindowForm::ShowWindowAsModal() {
	// modal は対応しないので、例外出す
	TVPThrowExceptionMessage(TJS_W("Modal window is not supported."));
}

void TTVPWindowForm::SetInnerWidth(int w) 
{
	SetInnerSize(w, GetInnerHeight());
}

void TTVPWindowForm::SetInnerHeight(int h)
{
	SetInnerSize(GetInnerWidth(), h);
}

void TTVPWindowForm::SetInnerSize(int w, int h) 
{
	ResizeWindow(w, h); 
}

int TTVPWindowForm::GetInnerWidth() const 
{ 
	return mSurfaceWidth; 
};

int TTVPWindowForm::GetInnerHeight() const 
{
	return mSurfaceHeight; 
};

void TTVPWindowForm::ResizeWindow(int w, int h) 
{
	mSurfaceWidth = w;
	mSurfaceHeight = h;
};

void TTVPWindowForm::UpdateVideoOverlay()
{
	if (TJSNativeInstance) {
		TJSNativeInstance->UpdateVideoOverlay();
	}
}

void TTVPWindowForm::OnTouchDown( float x, float y, float cx, float cy, tjs_int id, tjs_uint64 tick ) 
{
	TranslateWindowToDrawArea(x, y);

	TouchVelocityTracker.start( id );
	TouchVelocityTracker.update( id, tick, (float)x, (float)y );

	if(TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnTouchDownInputEvent(TJSNativeInstance, x, y, cx, cy, id));
	}
	touch_points_.TouchDown( x, y ,cx, cy, id, static_cast<tjs_uint>(tick&0xffffffff) );
}

void TTVPWindowForm::OnTouchMove( float x, float y, float cx, float cy, tjs_int id, tjs_uint64 tick ) 
{
	TranslateWindowToDrawArea(x, y);

	TouchVelocityTracker.update( id, tick, (float)x, (float)y );

	if(TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnTouchMoveInputEvent(TJSNativeInstance, x, y, cx, cy, id));
	}
	touch_points_.TouchMove( x, y, cx, cy, id, static_cast<tjs_uint>(tick&0xffffffff) );
}

void TTVPWindowForm::OnTouchUp( float x, float y, float cx, float cy, tjs_int id, tjs_uint64 tick ) 
{
	TranslateWindowToDrawArea(x, y);

	TouchVelocityTracker.update( id, tick, (float)x, (float)y );

	if(TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnTouchUpInputEvent(TJSNativeInstance, x, y, cx, cy, id));
	}
	touch_points_.TouchUp( x, y, cx, cy, id, static_cast<tjs_uint>(tick&0xffffffff) );
}

void TTVPWindowForm::OnTouchScaling( double startdist, double currentdist, double cx, double cy, int flag ) 
{
	if (TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnTouchScalingInputEvent(TJSNativeInstance, startdist, currentdist, cx, cy, flag ));
	}
}

void TTVPWindowForm::OnTouchRotate( double startangle, double currentangle, double distance, double cx, double cy, int flag ) 
{
	if (TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnTouchRotateInputEvent(TJSNativeInstance, startangle, currentangle, distance, cx, cy, flag));
	}
}

void TTVPWindowForm::OnMultiTouch() 
{
	if (TJSNativeInstance ) {
		TVPPostInputEvent( new tTVPOnMultiTouchInputEvent(TJSNativeInstance) );
	}
}

void TTVPWindowForm::OnResume() 
{
	if(TJSNativeInstance) TJSNativeInstance->FireOnActivate(true);
}

void TTVPWindowForm::OnPause() 
{
	if(TJSNativeInstance) TJSNativeInstance->FireOnActivate(false);
}

void TTVPWindowForm::OnResize()
{
	GetSurfaceSize(mSurfaceWidth, mSurfaceHeight);
	if(TJSNativeInstance) {
		// here specifies TVP_EPT_REMOVE_POST, to remove redundant onResize events.
		TJSNativeInstance->SetUpdateDestRect();
		TVPPostInputEvent( new tTVPOnResizeInputEvent(TJSNativeInstance), TVP_EPT_REMOVE_POST );
	}
}

void TTVPWindowForm::OnDisplayRotate( tjs_int orientation, tjs_int density ) 
{
	if (TJSNativeInstance) {
		TVPPostInputEvent( new tTVPOnDisplayRotateInputEvent(TJSNativeInstance, orientation, -1, density, 0, 0));
	}
}

/**
 * システムからのイベント処理
 */
void TTVPWindowForm::SendMessage( tjs_int message, tjs_int64 wparam, tjs_int64 lparam )
{
	// 非同期通知系。Application 経由でメインスレッドの Dispatch に配送される。
	Application->SendAppEvent( message, wparam, lparam );
}

void TTVPWindowForm::SendTouchMessage( tjs_int type, float x, float y, float c, int id, tjs_uint64 tick )
{
	// 入力系は同期処理 (発行元はメインスレッド)。tick を含む 3 値を欠落なく渡せる。
	// receiver chain には旧 NativeEvent と同じ packing (WParamf0/f1, LParamf0/LParam1) で見せる。
	tjs_uint32 xb, yb, cb;
	memcpy(&xb, &x, 4); memcpy(&yb, &y, 4); memcpy(&cb, &c, 4);
	tjs_int64 wparam = (tjs_int64)xb | ((tjs_int64)yb << 32);
	tjs_int64 lparam = (tjs_int64)cb | ((tjs_int64)(tjs_uint32)id << 32);
	if( DeliverToReceiver( type, wparam, lparam ) ) return;
	switch( type ) {
	case AM_TOUCH_DOWN: OnTouchDown( x, y, c, c, id, tick ); break;
	case AM_TOUCH_MOVE: OnTouchMove( x, y, c, c, id, tick ); break;
	case AM_TOUCH_UP:   OnTouchUp( x, y, c, c, id, tick ); break;
	default: break;
	}
}

void TTVPWindowForm::SendMouseMessage( tjs_int type, int button, int shift, int x, int y)
{
	// 入力系は同期処理。receiver chain には旧 packing (WParam0/1, LParam0/1) で見せる。
	tjs_int64 wparam = (tjs_int64)(tjs_uint32)button | ((tjs_int64)(tjs_uint32)shift << 32);
	tjs_int64 lparam = (tjs_int64)(tjs_uint32)x | ((tjs_int64)(tjs_uint32)y << 32);
	if( DeliverToReceiver( type, wparam, lparam ) ) return;
	switch( type ) {
	case AM_MOUSE_DOWN:  OnMouseDown( button, shift, x, y ); break;
	case AM_MOUSE_UP:    OnMouseUp( button, shift, x, y ); break;
	case AM_MOUSE_MOVE:  OnMouseMove( shift, x, y ); break;
	case AM_MOUSE_WHEEL: OnMouseWheel( button, shift, x, y ); break;
	default: break;
	}
}

//---------------------------------------------------------------------------
// プラグイン向け Window メッセージレシーバー API 実装
//---------------------------------------------------------------------------
bool TTVPWindowForm::InternalDeliverMessageToReceiver(tTVPWindowMessage &msg) {
	if( !TJSNativeInstance ) return false;
	if( TVPPluginUnloadedAtSystemExit ) return false;

	tObjectListSafeLockHolder<tTVPMessageReceiverRecord> holder(WindowMessageReceivers);
	tjs_int count = WindowMessageReceivers.GetSafeLockedObjectCount();

	bool block = false;
	for( tjs_int i = 0; i < count; i++ ) {
		tTVPMessageReceiverRecord *item = WindowMessageReceivers.GetSafeLockedObjectAt(i);
		if(!item) continue;
		bool b = item->Deliver(&msg);
		block = block || b;
	}
	return block;
}

void TTVPWindowForm::RegisterWindowMessageReceiver(tTVPWMRRegMode mode, void *proc, const void *userdata) {
	if( mode == wrmRegister ) {
		// 既に登録済みかチェック
		tjs_int count = WindowMessageReceivers.GetCount();
		tjs_int i;
		for( i = 0; i < count; i++ ) {
			tTVPMessageReceiverRecord *item = WindowMessageReceivers[i];
			if( !item ) continue;
			if( (void*)item->Proc == proc ) break; // already registered
		}
		if( i == count ) {
			tTVPMessageReceiverRecord *item = new tTVPMessageReceiverRecord();
			item->Proc = (tTVPWindowMessageReceiver)proc;
			item->UserData = userdata;
			WindowMessageReceivers.Add(item);
		}
	} else if( mode == wrmUnregister ) {
		tjs_int count = WindowMessageReceivers.GetCount();
		for( tjs_int i = 0; i < count; i++ ) {
			tTVPMessageReceiverRecord *item = WindowMessageReceivers[i];
			if( !item ) continue;
			if( (void*)item->Proc == proc ) {
				WindowMessageReceivers.Remove(i);
				delete item;
			}
		}
		WindowMessageReceivers.Compact();
	}
}
