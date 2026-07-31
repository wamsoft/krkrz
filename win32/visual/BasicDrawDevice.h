
#ifndef BASIC_DRAW_DEVICE_H
#define BASIC_DRAW_DEVICE_H

#include "DrawDevice.h"
#include "VideoPresenter.h"   // iTVPVideoPresenterHost / iTVPVideoPresenter
#ifdef KRKRZ_HAS_ELEMENTS
#include "D3D11DialogRenderer.h"   // iTVPD3D11DialogHost
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>
#include <memory>

//---------------------------------------------------------------------------
//! @brief		「Basic」デバイス(もっとも基本的な描画を行うのみのデバイス)
//! @note		D3D11/DXGI ベース実装。レイヤ合成済みフレームを動的テクスチャへ
//!				アップロードし、クライアントサイズの flip swapchain バックバッファへ
//!				スケール付きクアッドとして描画→ Present する。
//!				(旧実装は Direct3D9 固定機能パイプライン。D3D11Migration.md 参照)
//---------------------------------------------------------------------------
class tTVPBasicDrawDevice : public tTVPDrawDevice, public iTVPVideoPresenterHost
#ifdef KRKRZ_HAS_ELEMENTS
	, public iTVPD3D11DialogHost      // renderer→DrawDevice: D3D11 リソース借用
	, public iTVPDialogRendererHost   // manager→DrawDevice: renderer 提供
#endif
{
	typedef tTVPDrawDevice inherited;

	HWND TargetWindow;
	bool IsMainWindow;
	bool DrawUpdateRectangle;

	//-- D3D11 / DXGI オブジェクト
	ID3D11Device*			D3DDevice;
	ID3D11DeviceContext*	D3DContext;
	IDXGISwapChain1*		SwapChain;
	IDXGIOutput*			DXGIOutput;			//!< WaitForVBlank 用 (swapchain から取得)
	ID3D11RenderTargetView*	BackBufferRTV;		//!< バックバッファ RTV

	//-- 提示用パイプライン
	ID3D11VertexShader*		VertexShader;
	ID3D11PixelShader*		PixelShader;
	ID3D11InputLayout*		InputLayout;
	ID3D11Buffer*			VertexBuffer;		//!< 4 頂点 (TRIANGLESTRIP)、DYNAMIC
	ID3D11SamplerState*		SamplerLinear;
	ID3D11SamplerState*		SamplerPoint;

	//-- 合成フレームを受け取る動的テクスチャ
	ID3D11Texture2D*			Texture;
	ID3D11ShaderResourceView*	TextureSRV;
	void*	TextureBuffer;	//!< Map 中のピクセルポインタ (未 Map 時 NULL)
	long	TexturePitch;	//!< Map された行バイト数

	tjs_uint TextureWidth;	//!< テクスチャ(=元画像)の横幅
	tjs_uint TextureHeight;	//!< テクスチャ(=元画像)の縦幅

	UINT	SwapWidth;		//!< 現 swapchain 幅 (=クライアント幅)
	UINT	SwapHeight;		//!< 現 swapchain 高さ (=クライアント高さ)

	bool ShouldShow;		//!< show で実際に画面に画像を転送すべきか

	tjs_uint VsyncInterval;

	//-- Track V-E: overlay 動画 presenter (pull 型・単一スロット=最後に登録した 1 つ)。
	//   登録中は Show() で毎フレーム RenderVideoFrame が呼ばれ、バックバッファへ動画を
	//   全画面 present する (動画が画面を覆う前提)。
	iTVPVideoPresenter * VideoPresenter;

public:
	tTVPBasicDrawDevice(); //!< コンストラクタ

private:
	~tTVPBasicDrawDevice(); //!< デストラクタ

	void InvalidateAll();

	bool IsTargetWindowActive() const;

	//-- クライアント矩形サイズ取得
	bool GetClientSize( UINT &w, UINT &h ) const;

	//-- D3D11 リソース生成/破棄
	bool CreateD3DDevice();
	void DestroyD3DDevice();
	bool CreatePresentPipeline();	//!< shader/inputlayout/sampler/vbuffer
	void DestroyPresentPipeline();
	bool CreateSwapChain( UINT w, UINT h );
	void DestroySwapChain();
	bool EnsureBackBufferRTV();
	bool ResizeSwapChain( UINT w, UINT h );

	bool CreateTexture();
	void DestroyTexture();

	//-- 合成済みレイヤフレーム (TextureBuffer) をバックバッファ RTV へ描画する。
	//   EndBitmapCompletion (レイヤ更新時) と Show() (動画 present 時) の双方から呼ぶ。
	void DrawCompositedFrame();
	//-- 登録された overlay 動画 presenter をバックバッファ RTV の上へ重ねる (Show() から)。
	void RenderVideoPresenters();
	//-- 稼働中の overlay 動画 presenter があるか (毎フレーム present を強制するため)。
	bool HasActiveVideoPresenter() const { return VideoPresenter != 0; }

	void HandleDeviceLost();
	void ErrorToLog( HRESULT hr );

#ifdef KRKRZ_USE_REPL
	//-- 画面キャプチャ (エージェント/テスト用)。Present 直前にバックバッファを
	//   ステージングテクスチャへ読み戻して PNG 保存する。ScreenCapture.h の
	//   受け渡し層 (TVPRequestScreenCapture / TVPHasPendingScreenCapture) を消費。
	void FulfillScreenCapture();
	//-- overlay 動画 presenter 稼働時は CPU シャドウに動画が映らないので、描画直後
	//   (Present 直前) にバックバッファを読み戻してキャプチャする。
	void FulfillScreenCaptureFromBackBuffer();
#endif

public:
	void SetToRecreateDrawer() { DestroyD3DDevice(); }

	//! Track V-E: HW 動画 (IMFMediaEngine) が共有する engine の D3D11 デバイス。
	ID3D11Device* GetD3DDevice() const { return D3DDevice; }

public:
	void EnsureDevice();

//---- LayerManager の管理関連
	virtual void TJS_INTF_METHOD AddLayerManager(iTVPLayerManager * manager);

//---- 描画位置・サイズ関連
	virtual void TJS_INTF_METHOD SetTargetWindow(HWND wnd, bool is_main);
	virtual void TJS_INTF_METHOD SetDestRectangle(const tTVPRect & rect);
	virtual void TJS_INTF_METHOD NotifyLayerResize(iTVPLayerManager * manager);

//---- 再描画関連
	virtual void TJS_INTF_METHOD Show();
	virtual bool TJS_INTF_METHOD WaitForVBlank( tjs_int* in_vblank, tjs_int* delayed );

//---- LayerManager からの画像受け渡し関連
	virtual void TJS_INTF_METHOD StartBitmapCompletion(iTVPLayerManager * manager);
	virtual void TJS_INTF_METHOD NotifyBitmapCompleted(iTVPLayerManager * manager,
		tjs_int x, tjs_int y, const void * bits, const BITMAPINFO * bitmapinfo,
		const tTVPRect &cliprect, tTVPLayerType type, tjs_int opacity);
	virtual void TJS_INTF_METHOD EndBitmapCompletion(iTVPLayerManager * manager);

//---- デバッグ支援
	virtual void TJS_INTF_METHOD SetShowUpdateRect(bool b);

//---- フルスクリーン
	virtual bool TJS_INTF_METHOD SwitchToFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color, bool changeresolution );
	virtual void TJS_INTF_METHOD RevertFromFullScreen( HWND window, tjs_uint w, tjs_uint h, tjs_uint bpp, tjs_uint color );

//---- iTVPVideoPresenterHost (Track V-E: overlay 動画の presenter 登録口)
	//! presenter を登録する (以後 Show() で RenderVideoFrame が毎フレーム呼ばれる)。
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPVideoPresenter * presenter );
	//! presenter を登録解除する。
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPVideoPresenter * presenter );

#ifdef KRKRZ_HAS_ELEMENTS
//---- iTVPD3D11DialogHost (Elements ダイアログの D3D11 描画リソース貸出口)
	virtual bool DialogHost_GetD3D( ID3D11Device *& dev, ID3D11DeviceContext *& ctx,
	                                ID3D11RenderTargetView *& rtv,
	                                int & targetW, int & targetH );
	virtual void DialogHost_GetDestRect( int & x, int & y, int & w, int & h );

//---- iTVPDialogRendererHost (manager が具象型を知らずに renderer を取得する口)
	virtual iTVPDialogRenderer * GetDialogRenderer() override { return DialogRenderer.get(); }

private:
	//! この DrawDevice が所有する D3D11 dialog renderer (host = this)。
	std::unique_ptr<tTVPD3D11DialogRenderer> DialogRenderer;
public:
#endif

};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNI_BasicDrawDevice
//---------------------------------------------------------------------------
class tTJSNI_BasicDrawDevice :
	public tTJSNativeInstance
{
	typedef tTJSNativeInstance inherited;

	tTVPBasicDrawDevice * Device;

public:
	tTJSNI_BasicDrawDevice();
	~tTJSNI_BasicDrawDevice();
	tjs_error TJS_INTF_METHOD
		Construct(tjs_int numparams, tTJSVariant **param,
			iTJSDispatch2 *tjs_obj);
	void TJS_INTF_METHOD Invalidate();

public:
	tTVPBasicDrawDevice * GetDevice() const { return Device; }

};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// tTJSNC_BasicDrawDevice
//---------------------------------------------------------------------------
class tTJSNC_BasicDrawDevice : public tTJSNativeClass
{
public:
	tTJSNC_BasicDrawDevice();

	static tjs_uint32 ClassID;

private:
	iTJSNativeInstance *CreateNativeInstance();
};
//---------------------------------------------------------------------------


#endif // BASIC_DRAW_DEVICE_H
