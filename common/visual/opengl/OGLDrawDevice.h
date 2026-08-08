#ifndef OGL_DRAW_DEVICE_H
#define OGL_DRAW_DEVICE_H

#include "DrawDevice.h"
#include "OpenGLHeader.h"
#include "TextureUpdateRect.h"
#include "GLTexture.h"
#include "GLVideoPresenter.h"   // iTVPGLVideoPresenter / iTVPGLVideoPresenterHost
#include "EventIntf.h"   // tTVPRepeatedExceptionGuard
#ifdef __GENERIC__
#include "OGLViewportBackground.h"
#endif
#include <functional>
#include <mutex>
#include <memory>

#ifdef KRKRZ_HAS_ELEMENTS
#include "OGLDialogRenderer.h"
#endif

//---------------------------------------------------------------------------
//! @brief	OpelGL のテクスチャに描画する想定の DrawDevice
//---------------------------------------------------------------------------
class tTVPOGLDrawDevice : public tTVPDrawDevice
#ifdef __GENERIC__
	, public iTVPGLVideoPresenterHost   // overlay 動画を pull 型で受ける登録口 (GENERIC のみ)
#endif
#ifdef KRKRZ_HAS_ELEMENTS
	, public iTVPGLDialogHost         // renderer→DrawDevice: GL リソース借用
	, public iTVPDialogRendererHost   // manager→DrawDevice: renderer 提供
#endif
{
	typedef tTVPDrawDevice inherited;

	iTJSDispatch2 *Owner;
	iTJSDispatch2 *Self;
	tTJSVariant WindowObject;

	class iTVPGLContext *GLContext;
	// SetWindowInterface (InitGLES) 時に取得した context の保持用。
	// tTVPEGLContext は参照が尽きると即破棄されるため、保持しないと
	// SetTargetWindow 前にスクリプトが Texture 等を生成した時点で
	// カレントコンテキストが消えていて GL 呼び出しが全て失敗する
	class iTVPGLContext *InitGLESContext;
	GLTextureDrawer TextureDrawer;

	// primaryLayer 描画用テクスチャ
	tTJSNativeClass *TextureClass;
	tTJSVariant TextureObject;
	class tTJSNI_Texture *TextureInstance;
	TextureUpdateRect mTextureUpdateRect;

	// primaryLayer 描画用座標値
	tTJSVariant MatrixObject;
	class tTJSNI_Matrix32 *MatrixInstance;

	GLfloat _position[8];
	int SurfaceWidth;
	int SurfaceHeight;

	~tTVPOGLDrawDevice(); //!< デストラクタ

	void CreateTexture();
	void DestroyTexture();
	void UpdateTexture(int x, int y, int w, int h, std::function<void(char *dest, int pitch)> updator);

	void InitPosition();
	void InitMatrix();
	void InitUV();

	void InitContext(void *nativeWindow);
	void DoneContext();

	// -----------------------------------------------------
	// Canvas Interface
	// -----------------------------------------------------

	bool DoCreateCanvas;

	void CreateCanvas();
	void DestroyCanvas();
	
	tTJSVariant CanvasObject; //!< Current Canvas TJS2 Object
	class tTJSNI_Canvas* CanvasInstance;
	void SetCanvasObject(const tTJSVariant & val);

	// onDraw ハンドラの連続例外ガード (EventIntf.h 参照)。
	// 上限到達で onDraw の発火を停止して WARNING を出す (画面クリアは継続)。
	// resumeOnDraw() (TJS) で再開。
	tTVPRepeatedExceptionGuard OnDrawExceptionGuard;
	bool OnDrawDisabled = false;

public:
	void RequestCreateCanvas();
	void ResumeOnDraw() { OnDrawDisabled = false; OnDrawExceptionGuard.Reset(); }
	const tTJSVariant & GetCanvasObject() const { return CanvasObject; }
	const tTJSVariant & GetWindowObject() const { return WindowObject; }
	const tTJSVariant & GetTextureObject() const { return TextureObject; }
	const tTJSVariant & GetMatrixObject() const { return MatrixObject; }

public:
	tTVPOGLDrawDevice(iTJSDispatch2 *tjs_obj); //!< コンストラクタ

	//---- オブジェクト生存期間制御
	virtual void TJS_INTF_METHOD Destruct();

	//---- window interface 関連
	virtual void TJS_INTF_METHOD SetWindowInterface(iTVPWindow * window);

    //---- LayerManager の管理関連
	virtual void TJS_INTF_METHOD AddLayerManager(iTVPLayerManager * manager);

    //---- 描画位置・サイズ関連
	virtual void TJS_INTF_METHOD SetTargetWindow(HWND wnd, bool is_main);
	virtual void TJS_INTF_METHOD SetDestRectangle(const tTVPRect & rect);
	virtual void TJS_INTF_METHOD NotifyLayerResize(iTVPLayerManager * manager);

	//----- 表示処理
	virtual void TJS_INTF_METHOD Show();

    //---- LayerManager からの画像受け渡し関連
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

#ifdef __GENERIC__	

public:
	// overlay 動画は pull 型 presenter host (WINVER/SDL と統一)。push 型
	// (UpdateVideo/ClearVideo + 自前 mVideoBuffer/_video_texture) は廃止し、base の
	// UpdateVideo/ClearVideo no-op を継承。
	//---- iTVPGLVideoPresenterHost
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPGLVideoPresenter * presenter );
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPGLVideoPresenter * presenter );
	//! 現在 presenter が稼働中か (稼働中はレイヤ描画を省き動画のみ描く)。
	bool HasActiveVideoPresenter() const { return VideoPresenter != nullptr; }

	virtual void SetWaitVSync(bool enable);

private:
#ifdef __GENERIC__
	// ビューポート余白の壁紙テクスチャキャッシュ (背景色は base が保持)
	tTVPGLWallpaperCache mWallpaperCache;
#endif

	// overlay 動画 presenter (単一スロット、最後に登録した 1 つを pull する)。
	// フレームバッファ / GLTexture は presenter 側 (GLVideoPresenter.cpp) が持つ。
	iTVPGLVideoPresenter *VideoPresenter;
	bool ShowVideo();

#endif

#ifdef KRKRZ_HAS_ELEMENTS
public:
	// iTVPGLDialogHost — Elements ダイアログ overlay 用に GL リソース / DestRect
	// を借用させるための accessor。GLContext は InitContext 前 / DoneContext 後は
	// nullptr を返す。
	iTVPGLContext * DialogHost_GetGLContext() override { return GLContext; }
	GLTextureDrawer * DialogHost_GetTextureDrawer() override { return &TextureDrawer; }
	void DialogHost_GetDestRect(int & x, int & y, int & w, int & h) override
	{
		x = DestRect.left;
		y = DestRect.top;
		w = DestRect.get_width();
		h = DestRect.get_height();
	}

	// iTVPDialogRendererHost — manager が具象型を知らずに renderer を取得する口。
	// GL context 生存中だけ renderer が存在する (DoneContext 後は nullptr)。
	iTVPDialogRenderer * GetDialogRenderer() override { return DialogRenderer.get(); }
private:
	//! この DrawDevice が所有する OGL dialog renderer (host = this)。InitContext で
	//! 生成、DoneContext で破棄 (GL context 寿命に一致)。
	std::unique_ptr<tTVPOGLDialogRenderer> DialogRenderer;
public:
#endif
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_OGLDrawDevice
//---------------------------------------------------------------------------
class tTJSNI_OGLDrawDevice :
	public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPOGLDrawDevice * Device;

public:
	tTJSNI_OGLDrawDevice();
	~tTJSNI_OGLDrawDevice();
	tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

public:
	tTVPOGLDrawDevice * GetDevice() const { return Device; }

};
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_OGLDrawDevice
//---------------------------------------------------------------------------
class tTJSNC_OGLDrawDevice : public tTJSNativeClass
{
public:
	tTJSNC_OGLDrawDevice();

	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------


#endif // OGL_DRAW_DEVICE_H