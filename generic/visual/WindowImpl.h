//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// "Window" TJS Class implementation
//---------------------------------------------------------------------------


#ifndef WindowImplH
#define WindowImplH

#include "WindowIntf.h"
#include <functional>

/*[*/
//---------------------------------------------------------------------------
// window message receivers (Generic 版)
//---------------------------------------------------------------------------
// 配信は Application の SendAppEvent 経由の非同期処理なので Result は無効
// （呼び出し元には返らない）。doc/AppEvent.md 参照。
#ifdef __GENERIC__
enum tTVPWMRRegMode { wrmRegister=0, wrmUnregister=1 };
struct tTVPWindowMessage
{
	tjs_uint32 Msg;    // window message id
	tjs_uint64 WParam;
	tjs_uint64 LParam;
	tjs_uint64 Result; // 非同期配信のため無効。互換のため保持
};
typedef bool (* tTVPWindowMessageReceiver)
	(void *userdata, tTVPWindowMessage *Message);

// Generic 用のメッセージ ID 帯域。WM_USER 由来でない固定数値。
// プラグインは TVP_WM_USER 以降を自由に使用可能。
#define TVP_WM_USER 0x8000
#define TVP_WM_DETACH (TVP_WM_USER+106)
#define TVP_WM_ATTACH (TVP_WM_USER+107)
#define TVP_WM_FULLSCREEN_CHANGING (TVP_WM_USER+108)
#define TVP_WM_FULLSCREEN_CHANGED  (TVP_WM_USER+109)
#endif

/*]*/

//---------------------------------------------------------------------------
// tTJSNI_Window : Window Native Instance
//---------------------------------------------------------------------------
class TTVPWindowForm;
class iTVPDrawDevice;
class tTJSNI_BaseLayer;
class tTJSNI_VideoOverlay;

class tTJSNI_Window : public tTJSNI_BaseWindow
{
	TTVPWindowForm *Form;
	ttstr mCaption;

	tjs_int LayerWidth;  //< DrawDeviceのプライマリレイヤサイズ
	tjs_int LayerHeight; //< DrawDeviceのプライマリレイヤサイズ
	bool UpdateDestRect; //< DestRect 更新フラグ
	bool SetWindowHandleToDrawDevice; //< Handle変更フラグ

	// 稼働中 VideoOverlay
	std::vector<tTJSNI_VideoOverlay *> VideoOverlays;

public:
	tTJSNI_Window();
	tjs_error TJS_INTF_METHOD Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

	void SetUpdateDestRect() { UpdateDestRect = true; }

public:
	bool CanDeliverEvents() const; // tTJSNI_BaseWindow::CanDeliverEvents override

public:
	TTVPWindowForm * GetForm() const { return Form; }

	// プラグイン向け Native Window ハンドル取得
	// (例: SDL3 では SDL_Window*)。WIN 版の HWND と同じ位置付け。
	void *GetNativeHandle();

	void NotifyWindowClose();

	void TickBeat();

private:
	bool GetWindowActive();
	void UpdateWaitVSync();

public:
//-- draw device
	virtual void ResetDrawDevice();
	virtual void UpdateContent();

//-- videooverlay
	void UpdateVideo(tjs_int w, tjs_int h, std::function<void(char *dest, int pitch)> updator);
	void ClearVideo();
	void AddVideoOverlay( tTJSNI_VideoOverlay *overlay );
	void DelVideoOverlay( tTJSNI_VideoOverlay *overlay );
	void CheckVideoOverlay();
	void UpdateVideoOverlay();

//-- interface to layer manager
	void TJS_INTF_METHOD NotifySrcResize(); // is called from primary layer

//-- event control
	virtual void PostInputEvent(const ttstr &name, iTJSDispatch2 * params);  // override

//-- interface to layer manager
	void TJS_INTF_METHOD SetDefaultMouseCursor(); // set window mouse cursor to default
	void TJS_INTF_METHOD SetMouseCursor(tjs_int cursor); // set window mouse cursor
	void TJS_INTF_METHOD GetCursorPos(tjs_int &x, tjs_int &y);
	void TJS_INTF_METHOD SetCursorPos(tjs_int x, tjs_int y);
	void TJS_INTF_METHOD WindowReleaseCapture();
	void TJS_INTF_METHOD SetHintText(iTJSDispatch2* sender, const ttstr & text);
	void TJS_INTF_METHOD SetAttentionPoint(tTJSNI_BaseLayer *layer,
		tjs_int l, tjs_int t);
	void TJS_INTF_METHOD DisableAttentionPoint();
	void TJS_INTF_METHOD SetImeMode(tTVPImeMode mode);
	void SetDefaultImeMode(tTVPImeMode mode) {}
	tTVPImeMode GetDefaultImeMode() const { return imDisable; }
	void TJS_INTF_METHOD ResetImeMode();

//-- interface to plugin
	void RegisterWindowMessageReceiver(tTVPWMRRegMode mode,
		void * proc, const void *userdata);

//-- methods
	void Close();
	void OnCloseQueryCalled(bool b);

	void BringToFront();
	void Update(tTVPUpdateType=utNormal);

	void ShowModal();

	void HideMouseCursor();

//-- ビューポート (ゲーム画面の表示画角制御)。Form に設定を保持し、配置は
//   DestRect 再計算、余白色/壁紙は UpdateContent で DrawDevice へ push する。
	void SetViewportFit(tjs_int fit);
	tjs_int GetViewportFit() const;
	void SetViewportZoom(double scale);
	double GetViewportZoom() const;
	void SetViewportAlignX(double v);
	double GetViewportAlignX() const;
	void SetViewportAlignY(double v);
	double GetViewportAlignY() const;
	void SetViewportOffsetX(tjs_int v);
	tjs_int GetViewportOffsetX() const;
	void SetViewportOffsetY(tjs_int v);
	tjs_int GetViewportOffsetY() const;
	void SetViewportBgColor(tjs_uint32 color);
	tjs_uint32 GetViewportBgColor() const;
	// 壁紙: 文字列なら Bitmap を生成してロード、Layer/Bitmap オブジェクトならそのまま
	// 参照保持して Form へ渡す。fit/align は壁紙用。
	void SetViewportWallpaper(const tTJSVariant &image, tjs_int fit, double alignX, double alignY);
	void ClearViewportWallpaper();

//-- properties
	bool GetVisible() const;
	void SetVisible(bool s);

	void GetCaption(ttstr & v) const;
	void SetCaption(const ttstr & v);

	void SetWidth(tjs_int w);
	tjs_int GetWidth() const;
	void SetHeight(tjs_int h);
	tjs_int GetHeight() const;
	void SetSize(tjs_int w, tjs_int h);

	void SetMinWidth(int v);
	int GetMinWidth() const;
	void SetMinHeight(int v);
	int GetMinHeight() const;
	void SetMinSize(int w, int h);

	void SetMaxWidth(int v);
	int GetMaxWidth() const;
	void SetMaxHeight(int v);
	int GetMaxHeight() const;
	void SetMaxSize(int w, int h);

	void SetLeft(tjs_int l);
	tjs_int GetLeft() const;
	void SetTop(tjs_int t);
	tjs_int GetTop() const;
	void SetPosition(tjs_int l, tjs_int t);

	void SetInnerWidth(tjs_int w);
	tjs_int GetInnerWidth() const;
	void SetInnerHeight(tjs_int h);
	tjs_int GetInnerHeight() const;

	void SetInnerSize(tjs_int w, tjs_int h);

	void SetBorderStyle(tTVPBorderStyle st);
	tTVPBorderStyle GetBorderStyle() const;

	void SetStayOnTop(bool b);
	bool GetStayOnTop() const;

	void SetFullScreen(bool b);
	bool GetFullScreen() const;

	void SetUseMouseKey(bool b);
	bool GetUseMouseKey() const;

	void SetTrapKey(bool b);
	bool GetTrapKey() const;

	//void SetMaskRegion(tjs_int threshold);
	//void RemoveMaskRegion();

	void SetMouseCursorState(tTVPMouseCursorState mcs);
    tTVPMouseCursorState GetMouseCursorState() const;

	void SetFocusable(bool b);
	bool GetFocusable();

	void SetZoom(tjs_int numer, tjs_int denom);
	void SetZoomNumer(tjs_int n);
	tjs_int GetZoomNumer() const;
	void SetZoomDenom(tjs_int n);
	tjs_int GetZoomDenom() const;
	
	void SetTouchScaleThreshold( tjs_real threshold );
	tjs_real GetTouchScaleThreshold() const;
	void SetTouchRotateThreshold( tjs_real threshold );
	tjs_real GetTouchRotateThreshold() const;

	tjs_real GetTouchPointStartX( tjs_int index );
	tjs_real GetTouchPointStartY( tjs_int index );
	tjs_real GetTouchPointX( tjs_int index );
	tjs_real GetTouchPointY( tjs_int index );
	tjs_real GetTouchPointID( tjs_int index );
	tjs_int GetTouchPointCount();
	bool GetTouchVelocity( tjs_int id, float& x, float& y, float& speed ) const;
	bool GetMouseVelocity( float& x, float& y, float& speed ) const;
	void ResetMouseVelocity();
	
	void SetHintDelay( tjs_int delay );
	tjs_int GetHintDelay() const;

	void SetEnableTouch( bool b );
	bool GetEnableTouch() const;

	void SetEnableTouchMouse( bool b );
	bool GetEnableTouchMouse() const;

	int GetDisplayOrientation();
	int GetDisplayRotate();
	
	bool WaitForVBlank( tjs_int* in_vblank, tjs_int* delayed );

	void OnTouchUp( tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id );
public: // for iTVPLayerTreeOwner
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(class iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const class BitmapInfomation * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

	virtual void TJS_INTF_METHOD SetMouseCursor(class iTVPLayerManager* manager, tjs_int cursor);
	virtual void TJS_INTF_METHOD GetCursorPos(class iTVPLayerManager* manager, tjs_int &x, tjs_int &y);
	virtual void TJS_INTF_METHOD SetCursorPos(class iTVPLayerManager* manager, tjs_int x, tjs_int y);
	virtual void TJS_INTF_METHOD ReleaseMouseCapture(class iTVPLayerManager* manager);

	virtual void TJS_INTF_METHOD SetHint(class iTVPLayerManager* manager, iTJSDispatch2* sender, const ttstr &hint);

	virtual void TJS_INTF_METHOD NotifyLayerResize(class iTVPLayerManager* manager);
	virtual void TJS_INTF_METHOD NotifyLayerImageChange(class iTVPLayerManager* manager);

	virtual void TJS_INTF_METHOD SetAttentionPoint(class iTVPLayerManager* manager, tTJSNI_BaseLayer *layer, tjs_int x, tjs_int y);
	virtual void TJS_INTF_METHOD DisableAttentionPoint(class iTVPLayerManager* manager);

	virtual void TJS_INTF_METHOD SetImeMode( class iTVPLayerManager* manager, tjs_int mode ); // mode == tTVPImeMode
	virtual void TJS_INTF_METHOD ResetImeMode( class iTVPLayerManager* manager );

};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#endif
