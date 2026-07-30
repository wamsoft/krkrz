/****************************************************************************/
/*! @file
@brief overlay 動画用の D3D11 YUV レンダラ (子ウィンドウ最前面 present)

Track V の overlay 対応。EVR が扱えない webm/mpg の overlay 再生用に、デコーダの
YUV (I420) プレーンを D3D11 テクスチャへ上げ、ピクセルシェーダで YUV→RGB 変換
しながらフルスクリーンquadで描画してスワップチェーンへ present する。
CPU での YUV→RGB 変換や GDI 拡大を避け、変換/拡大を GPU に任せる (高性能・高品質)。
子ウィンドウ方式なので DrawDevice に依存しない (自前 DrawDevice でも動く)。
将来 V-E (vomD3D11) のデコードテクスチャ直 present の土台にも流用できる。
*****************************************************************************/
#ifndef __D3D11_OVERLAY_WINDOW_H__
#define __D3D11_OVERLAY_WINDOW_H__

#include <windows.h>
#include <d3d11.h>
#include <mutex>

class tTVPD3D11OverlayWindow
{
public:
	tTVPD3D11OverlayWindow();
	~tTVPD3D11OverlayWindow();

	//! 親 (game) ウィンドウの子として overlay ウィンドウ + D3D11 を用意する。
	bool Create( HWND owner );
	void Destroy();

	//! 表示矩形 (親クライアント座標) を設定。
	void SetRect( const RECT &rect );
	//! 表示/非表示。
	void SetVisible( bool visible );
	//! マウスメッセージの転送先 (通常 game window)。
	void SetMessageDrainWindow( HWND wnd ) { MessageDrainWindow = wnd; }

	//! I420 (YUV420 planar) の 1 フレームを present する。
	//! y/u/v: 各プレーン先頭、*Stride: 行バイト数、w/h: 輝度サイズ
	//! (色差は w/2 x h/2)。スレッド安全 (内部でロック)。
	void PresentI420( const uint8_t *y, int yStride,
	                  const uint8_t *u, int uStride,
	                  const uint8_t *v, int vStride,
	                  int w, int h );

	//! packed BGRA (B8G8R8A8, top-down) の 1 フレームを present する。
	//! data: 先頭行ポインタ、stride: 行バイト数、w/h: サイズ。スレッド安全。
	//! (MF SourceReader の RGB32 出力 = wmv/mp4/avi overlay 用)
	void PresentBGRA( const uint8_t *data, int stride, int w, int h );

	//! 直近 present したフレームを PNG 保存 (デバッグ/自己検証用)。成功で true。
	bool DebugSaveLastFrame( const wchar_t *path );

	HWND GetHWND() const { return ChildWnd; }

private:
	static LRESULT CALLBACK WndProcThunk( HWND, UINT, WPARAM, LPARAM );
	LRESULT WndProc( HWND, UINT, WPARAM, LPARAM );

	bool CreateDevice();
	bool CreateSwapChain( UINT w, UINT h );
	bool CreatePipeline();
	bool EnsurePlaneTextures( int w, int h );
	void ReleasePlaneTextures();
	bool EnsureBGRATexture( int w, int h );
	void ReleaseBGRATexture();
	bool EnsureBackBufferRTV();
	void ResizeToClient();

	HWND ChildWnd;
	HWND OwnerWnd;
	HWND MessageDrainWindow;
	RECT DesiredRect;
	bool Visible;
	static ATOM ClassAtom;

	std::mutex Mtx;

	ID3D11Device*           Dev;
	ID3D11DeviceContext*    Ctx;
	IDXGISwapChain*         Swap;
	ID3D11RenderTargetView* RTV;
	UINT SwapW, SwapH;

	ID3D11VertexShader* VS;
	ID3D11PixelShader*  PS;      //!< I420→RGB (YUV) 用
	ID3D11PixelShader*  PSRgba;  //!< packed BGRA パススルー用
	ID3D11InputLayout*  IL;
	ID3D11Buffer*       VB;
	ID3D11SamplerState* Samp;

	// I420 の 3 プレーン (R8_UNORM)
	ID3D11Texture2D*          TexY;
	ID3D11Texture2D*          TexU;
	ID3D11Texture2D*          TexV;
	ID3D11ShaderResourceView* SrvY;
	ID3D11ShaderResourceView* SrvU;
	ID3D11ShaderResourceView* SrvV;
	int PlaneW, PlaneH; //!< 現在確保している輝度プレーンサイズ

	// packed BGRA (B8G8R8A8_UNORM) 1 枚 (wmv/mp4/avi overlay 用)
	ID3D11Texture2D*          TexBGRA;
	ID3D11ShaderResourceView* SrvBGRA;
	int BgraW, BgraH;   //!< 現在確保している BGRA テクスチャサイズ

	int LastW, LastH;   //!< 直近フレームの映像サイズ
};

#endif // __D3D11_OVERLAY_WINDOW_H__
