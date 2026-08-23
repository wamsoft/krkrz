//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//!@file 描画デバイス管理
//---------------------------------------------------------------------------

#include "tjsCommHead.h"

#include <algorithm>
#include "DrawDevice.h"
#include "MsgIntf.h"
#include "LayerIntf.h"
#include "LayerManager.h"
#include "WindowIntf.h"
#include "DebugIntf.h"
#include "NullDrawDevice.h"
#include "SysInitIntf.h"
#include "ThreadIntf.h"  // KRKRZ_RENDER_STATS_SCOPE / TVPRenderStatsAddFrameUpdate

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsDialogManager.h"
// Elements ダイアログがアクティブなら入力を Elements 側へ転送する。 Forward*
// が「消費した (true)」を返したときだけ return してゲーム入力処理を止める。
// 非モーダル UI で、 ダイアログに当たらない入力は素通しさせるため。
#define TVP_DIALOG_INTERCEPT(forward_call) \
	do { \
		if (tTVPElementsDialogManager::Instance().IsModalActive()) { \
			if (tTVPElementsDialogManager::Instance().forward_call) \
				return; \
		} \
	} while(0)
#else
#define TVP_DIALOG_INTERCEPT(forward_call) ((void)0)
#endif

//---------------------------------------------------------------------------
tTVPDrawDevice::tTVPDrawDevice()
{
	// コンストラクタ
	Window = NULL;
	PrimaryLayerManagerIndex = 0;
	DestRect.clear();
	ClipRect.clear();
	ViewportBgColor = 0xff000000;
	ViewportWallpaperFit = vfCover;
	ViewportWpAlignX = 0.5;
	ViewportWpAlignY = 0.5;
	ViewportWallpaperGen = 0;
}

//---------------------------------------------------------------------------
// 描画デバイスの TJS オブジェクトから余白塗りの登録口を得る。
// 規定プロパティ "viewportBackgroundHost" にポインタ値が入っている
// (videoPresenterHost と同じ規約)。 非対応デバイスは NULL。
//---------------------------------------------------------------------------
iTVPViewportBackgroundHost * TVPQueryViewportBackgroundHost(const tTJSVariant &ddobj)
{
	if( ddobj.Type() != tvtObject ) return NULL;
	tTJSVariantClosure clo = ddobj.AsObjectClosureNoAddRef();
	if( clo.Object == NULL ) return NULL;
	tTJSVariant val;
	if( TJS_FAILED( clo.PropGet(0, TJS_W("viewportBackgroundHost"), NULL, &val, NULL) ) )
		return NULL;
	tjs_int64 p = (tjs_int64)val;
	if( p == 0 ) return NULL;
	return reinterpret_cast<iTVPViewportBackgroundHost*>((intptr_t)p);
}
//---------------------------------------------------------------------------
void tTVPDrawDevice::SetViewportBackgroundColor(tjs_uint32 color)
{
	ViewportBgColor = color;
}
//---------------------------------------------------------------------------
void tTVPDrawDevice::SetViewportWallpaper(const tTJSVariant &image,
	tTVPViewportFit fit, double alignX, double alignY)
{
	// tTJSVariant がオブジェクト参照を保持するのでイメージデータは維持される。
	// 内部 bitmap のコピーは持たず、描画時に PropGet で画像を取得する。
	if (image.Type() == tvtObject && image.AsObjectNoAddRef()) {
		ViewportWallpaper = image;
	} else {
		ViewportWallpaper.Clear();
	}
	ViewportWallpaperFit = fit;
	ViewportWpAlignX = alignX;
	ViewportWpAlignY = alignY;
	ViewportWallpaperGen++;  // 描画デバイス側のテクスチャ再アップロードを促す
}
//---------------------------------------------------------------------------
void tTVPDrawDevice::ClearViewportWallpaper()
{
	ViewportWallpaper.Clear();
	ViewportWallpaperGen++;
}
//---------------------------------------------------------------------------
bool tTVPDrawDevice::GetViewportWallpaperImage(tjs_int &w, tjs_int &h,
	tjs_int &pitch, const tjs_uint8 *&buffer) const
{
	if (ViewportWallpaper.Type() != tvtObject) return false;
	iTJSDispatch2 *obj = ViewportWallpaper.AsObjectNoAddRef();
	if (!obj) return false;

	// Layer / Bitmap 共通のプロパティ経由で画像イメージを取得する。
	tTJSVariant val;
	if (TJS_FAILED(obj->PropGet(0, TJS_W("imageWidth"), nullptr, &val, obj)))
		return false;
	w = (tjs_int)val;
	obj->PropGet(0, TJS_W("imageHeight"), nullptr, &val, obj);
	h = (tjs_int)val;
	obj->PropGet(0, TJS_W("mainImageBufferPitch"), nullptr, &val, obj);
	pitch = (tjs_int)val;
	obj->PropGet(0, TJS_W("mainImageBuffer"), nullptr, &val, obj);
	buffer = reinterpret_cast<const tjs_uint8 *>(static_cast<tjs_intptr_t>((tjs_int64)val));

	if (w <= 0 || h <= 0 || !buffer) return false;
	return true;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
tTVPDrawDevice::~tTVPDrawDevice()
{
	// すべての managers を開放する
	//TODO: プライマリレイヤ無効化、あるいはウィンドウ破棄時の終了処理が正しいか？
	// managers は 開放される際、自身の登録解除を行うために
	// RemoveLayerManager() を呼ぶかもしれないので注意。
	// そのため、ここではいったん配列をコピーしてからそれぞれの
	// Release() を呼ぶ。
	std::vector<iTVPLayerManager *> backup = Managers;
	for(std::vector<iTVPLayerManager *>::iterator i = backup.begin(); i != backup.end(); i++)
		(*i)->Release();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool tTVPDrawDevice::TransformToPrimaryLayerManager(tjs_int &x, tjs_int &y)
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return false;

	// プライマリレイヤマネージャのプライマリレイヤのサイズを得る
	tjs_int pl_w, pl_h;
	if(!manager->GetPrimaryLayerSize(pl_w, pl_h)) return false;

	// x , y は DestRect の 0, 0 を原点とした座標として渡されてきている
	tjs_int w = DestRect.get_width();
	tjs_int h = DestRect.get_height();
	x = w ? (x * pl_w / w) : 0;
	y = h ? (y * pl_h / h) : 0;

	return true;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
bool tTVPDrawDevice::TransformFromPrimaryLayerManager(tjs_int &x, tjs_int &y)
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return false;

	// プライマリレイヤマネージャのプライマリレイヤのサイズを得る
	tjs_int pl_w, pl_h;
	if(!manager->GetPrimaryLayerSize(pl_w, pl_h)) return false;

	// x , y は DestRect の 0, 0 を原点とした座標として渡されてきている
	x = pl_w ? (x * DestRect.get_width()  / pl_w) : 0;
	y = pl_h ? (y * DestRect.get_height() / pl_h) : 0;

	return true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
bool tTVPDrawDevice::TransformToPrimaryLayerManager(tjs_real &x, tjs_real &y)
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return false;

	// プライマリレイヤマネージャのプライマリレイヤのサイズを得る
	tjs_int pl_w, pl_h;
	if(!manager->GetPrimaryLayerSize(pl_w, pl_h)) return false;

	// x , y は DestRect の 0, 0 を原点とした座標として渡されてきている
	tjs_int w = DestRect.get_width();
	tjs_int h = DestRect.get_height();
	x = w ? (x * pl_w / w) : 0.0;
	y = h ? (y * pl_h / h) : 0.0;

	return true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::Destruct()
{
	delete this;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetWindowInterface(iTVPWindow * window)
{
	Window = window;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::AddLayerManager(iTVPLayerManager * manager)
{
	// Managers に manager を push する。AddRefするのを忘れないこと。
	Managers.push_back(manager);
	manager->AddRef();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::RemoveLayerManager(iTVPLayerManager * manager)
{
	// Managers から manager を削除する。Releaseする。
	std::vector<iTVPLayerManager *>::iterator i = std::find(Managers.begin(), Managers.end(), manager);
	if(i == Managers.end())
		TVPThrowInternalError;
	(*i)->Release();
	Managers.erase(i);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetDestRectangle(const tTVPRect & rect)
{
	DestRect = rect;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetClipRectangle(const tTVPRect & rect)
{
	ClipRect = rect;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::GetSrcSize(tjs_int &w, tjs_int &h)
{
	w = 0;
	h = 0;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;
	if(!manager->GetPrimaryLayerSize(w, h))
	{
		w = 0;
		h = 0;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::NotifyLayerResize(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(primary_manager == manager)
		Window->NotifySrcResize();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::NotifyLayerImageChange(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(primary_manager == manager)
		Window->RequestUpdate();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnClick(tjs_int x, tjs_int y)
{
	TVP_DIALOG_INTERCEPT(ForwardClick(x, y));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyClick(x, y);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnDoubleClick(tjs_int x, tjs_int y)
{
	TVP_DIALOG_INTERCEPT(ForwardDoubleClick(x, y));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyDoubleClick(x, y);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	TVP_DIALOG_INTERCEPT(ForwardMouseDown(x, y, mb, flags));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMouseDown(x, y, mb, flags);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags)
{
	TVP_DIALOG_INTERCEPT(ForwardMouseUp(x, y, mb, flags));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMouseUp(x, y, mb, flags);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
{
	TVP_DIALOG_INTERCEPT(ForwardMouseMove(x, y, flags));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMouseMove(x, y, flags);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnReleaseCapture()
{
	TVP_DIALOG_INTERCEPT(ForwardReleaseCapture());
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->ReleaseCapture();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMouseOutOfWindow()
{
	TVP_DIALOG_INTERCEPT(ForwardMouseOutOfWindow());
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMouseOutOfWindow();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnKeyDown(tjs_uint key, tjs_uint32 shift)
{
	TVP_DIALOG_INTERCEPT(ForwardKeyDown(key, shift));
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyKeyDown(key, shift);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnKeyUp(tjs_uint key, tjs_uint32 shift)
{
	TVP_DIALOG_INTERCEPT(ForwardKeyUp(key, shift));
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyKeyUp(key, shift);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnKeyPress(tjs_char key)
{
	TVP_DIALOG_INTERCEPT(ForwardKeyPress(key));
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyKeyPress(key);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMouseWheel(tjs_uint32 shift, tjs_int delta, tjs_int x, tjs_int y)
{
	TVP_DIALOG_INTERCEPT(ForwardMouseWheel(shift, delta, x, y));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMouseWheel(shift, delta, x, y);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnTouchDown( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id )
{
	TVP_DIALOG_INTERCEPT(ForwardTouchDown(x, y, cx, cy, id));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyTouchDown(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnTouchUp( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id )
{
	TVP_DIALOG_INTERCEPT(ForwardTouchUp(x, y, cx, cy, id));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyTouchUp(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnTouchMove( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id )
{
	TVP_DIALOG_INTERCEPT(ForwardTouchMove(x, y, cx, cy, id));
	if(!TransformToPrimaryLayerManager(x, y)) return;
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyTouchMove(x, y, cx, cy, id);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnTouchScaling( tjs_real startdist, tjs_real curdist, tjs_real cx, tjs_real cy, tjs_int flag )
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyTouchScaling(startdist, curdist, cx, cy, flag);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnTouchRotate( tjs_real startangle, tjs_real curangle, tjs_real dist, tjs_real cx, tjs_real cy, tjs_int flag )
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyTouchRotate(startangle, curangle, dist, cx, cy, flag);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnMultiTouch()
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->NotifyMultiTouch();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::OnDisplayRotate( tjs_int orientation, tjs_int rotate, tjs_int bpp, tjs_int width, tjs_int height )
{
	// 何もしない
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::RecheckInputState()
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;

	manager->RecheckInputState();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::PresentDialogOverlay()
{
#ifdef KRKRZ_HAS_ELEMENTS
	tTVPElementsDialogManager::Instance().PaintOverlay(this);
#endif
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetDefaultMouseCursor(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->SetDefaultMouseCursor();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetMouseCursor(iTVPLayerManager * manager, tjs_int cursor)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->SetMouseCursor(cursor);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::GetCursorPos(iTVPLayerManager * manager, tjs_int &x, tjs_int &y)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	Window->GetCursorPos(x, y);
	if(primary_manager != manager || !TransformToPrimaryLayerManager(x, y))
	{
		// プライマリレイヤマネージャ以外には座標 0,0 で渡しておく
		 x = 0;
		 y = 0;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetCursorPos(iTVPLayerManager * manager, tjs_int x, tjs_int y)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		if(TransformFromPrimaryLayerManager(x, y))
			Window->SetCursorPos(x, y);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::WindowReleaseCapture(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->WindowReleaseCapture();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetHintText(iTVPLayerManager * manager, iTJSDispatch2* sender, const ttstr & text)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->SetHintText(sender,text);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetAttentionPoint(iTVPLayerManager * manager, tTJSNI_BaseLayer *layer,
				tjs_int l, tjs_int t)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		if(TransformFromPrimaryLayerManager(l, t))
			Window->SetAttentionPoint(layer, l, t);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::DisableAttentionPoint(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->DisableAttentionPoint();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetImeMode(iTVPLayerManager * manager, tTVPImeMode mode)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->SetImeMode(mode);
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::ResetImeMode(iTVPLayerManager * manager)
{
	iTVPLayerManager * primary_manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!primary_manager) return;
	if(primary_manager == manager)
	{
		Window->ResetImeMode();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
tTJSNI_BaseLayer * TJS_INTF_METHOD tTVPDrawDevice::GetPrimaryLayer()
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return NULL;
	return manager->GetPrimaryLayer();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
tTJSNI_BaseLayer * TJS_INTF_METHOD tTVPDrawDevice::GetFocusedLayer()
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return NULL;
	return manager->GetFocusedLayer();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetFocusedLayer(tTJSNI_BaseLayer * layer)
{
	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;
	manager->SetFocusedLayer(layer);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::RequestInvalidation(const tTVPRect & rect)
{
	tjs_int l = rect.left, t = rect.top, r = rect.right, b = rect.bottom;
	if(!TransformToPrimaryLayerManager(l, t)) return;
	if(!TransformToPrimaryLayerManager(r, b)) return;
	r ++; // 誤差の吸収(本当はもうちょっと厳密にやらないとならないがそれが問題になることはない)
	b ++;

	iTVPLayerManager * manager = GetLayerManagerAt(PrimaryLayerManagerIndex);
	if(!manager) return;
	manager->RequestInvalidation(tTVPRect(l, t, r, b));
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::Update()
{
	KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddFrameUpdate);
	// すべての layer manager の UpdateToDrawDevice を呼ぶ
	for(std::vector<iTVPLayerManager *>::iterator i = Managers.begin(); i != Managers.end(); i++)
	{
		(*i)->UpdateToDrawDevice();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::Show()
{
	// なにもしない
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPDrawDevice::WaitForVBlank( tjs_int* in_vblank, tjs_int* delayed )
{
	return false;
}

//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::DumpLayerStructure()
{
	// すべての layer manager の DumpLayerStructure を呼ぶ
	for(std::vector<iTVPLayerManager *>::iterator i = Managers.begin(); i != Managers.end(); i++)
	{
		(*i)->DumpLayerStructure();
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::SetShowUpdateRect(bool b)
{
	// なにもしない
}

//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTVPDrawDevice::SwitchToFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color, bool changeresolution )
{
	bool success = false;
	return success;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTVPDrawDevice::RevertFromFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color )
{
}
//---------------------------------------------------------------------------
