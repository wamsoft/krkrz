//---------------------------------------------------------------------------
//!@file D3D11 用 Elements ダイアログ描画アダプタ (WINVER / BasicDrawDevice)
//!
//! BasicDrawDevice の D3D11 デバイス上で、Elements が描いた overlay バッファ
//! (layer ごと) を engine のバックバッファへアルファ合成する iTVPDialogRenderer
//! 実装。ステージング CPU バッファ → DYNAMIC テクスチャ (B8G8R8A8) → クアッド
//! α 合成描画、という流れ。描画本体は overlay 動画と同じ tTVPVideoPresenterD3D
//! (CPU BGRA → RTV ブリッタ) を layer ごとに 1 個持って流用する。
//!
//! Elements の overlay バッファは uint32 = 0xAARRGGBB (little-endian でメモリ上
//! B,G,R,A バイト順) で、DXGI_FORMAT_B8G8R8A8_UNORM とそのまま一致するため
//! swizzle 不要 (SDL 版が SDL_PIXELFORMAT_ARGB8888 を使うのと同じ理由)。
//!
//! Host DrawDevice (BasicDrawDevice) は iTVPD3D11DialogHost を実装し、描画スレッド
//! (Show()) で有効な D3D11 リソース (device/context/backbuffer RTV/サイズ) と
//! DestRect を貸し出す。
//---------------------------------------------------------------------------
#ifndef D3D11_DIALOG_RENDERER_H
#define D3D11_DIALOG_RENDERER_H

#include "elements/DialogRenderer.h"
#include <cstdint>
#include <vector>
#include <map>

// D3D11 の型は前方宣言のみ (このヘッダに d3d11.h を持ち込まない)。BasicDrawDevice.h
// は別途 d3d11.h を include するが、前方宣言と共存できる。
#ifndef __d3d11_h__
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
#endif

//! @brief D3D11DialogRenderer がホスト DrawDevice から D3D11 リソース / DestRect を
//!        借用するための小さな抽象。BasicDrawDevice が実装する (多重継承)。
class iTVPD3D11DialogHost
{
public:
	virtual ~iTVPD3D11DialogHost() = default;

	//! @brief 描画に必要な D3D11 リソースを取得する。描画スレッド (Show()) からのみ
	//!        有効。まだ device 未生成等で描けないときは false を返す。
	//! @param targetW,targetH  バックバッファ (=クライアント) の px サイズ。
	virtual bool DialogHost_GetD3D(ID3D11Device *& dev, ID3D11DeviceContext *& ctx,
	                               ID3D11RenderTargetView *& rtv,
	                               int & targetW, int & targetH) = 0;

	//! @brief DestRect (= ゲーム画像が描画される画面領域、px)。ダイアログの中央
	//!        配置とマウス座標変換の基準。未確定なら 0,0,0,0 を返す。
	virtual void DialogHost_GetDestRect(int & x, int & y, int & w, int & h) = 0;
};

class tTVPVideoPresenterD3D;   // per-layer BGRA ブリッタ (実装流用)

//! @brief D3D11 版 dialog overlay レンダラ (BasicDrawDevice 用)。
//!
//!  - AcquireBuffer() : layer 固有の CPU 連続バッファ (w*h*4, 連続 pitch) を返す。
//!  - ReleaseBuffer() : no-op (アップロードは PresentOverlay の Render 内で行う)。
//!  - PresentOverlay(): staging を DYNAMIC tex へ上げ、DestRect 同等の px 矩形へ
//!                      α 合成でクアッド描画。RTV/viewport は self-contained に設定。
//!
//! ライフサイクル: host DrawDevice が生成/破棄時に Register/UnregisterDialogHost する。
//! layer ごとに別テクスチャ (tTVPVideoPresenterD3D) を持つ (present がテクスチャを
//! 参照キューイングするため使い回すと壊れる、の D3D11 版)。
class tTVPD3D11DialogRenderer : public iTVPDialogRenderer
{
public:
	explicit tTVPD3D11DialogRenderer(iTVPD3D11DialogHost * host);
	~tTVPD3D11DialogRenderer() override;

	void GetSurfaceSize(int & w, int & h) override;
	void GetDestRect(int & x, int & y, int & w, int & h) override;
	std::uint32_t * AcquireBuffer(const void* layer, int w, int h) override;
	void ReleaseBuffer(const void* layer) override;
	void ReleaseBufferRect(const void* layer, int x, int y, int w, int h) override;
	void PresentOverlay(const void* layer, int x, int y, int w, int h) override;
	void ReleaseLayer(const void* layer) override;

private:
	iTVPD3D11DialogHost * _host;   //!< 借用 (所有しない)

	//! @brief overlay インスタンス (layer) ごとの staging + GPU テクスチャ + ブリッタ。
	//!
	//! テクスチャは presenter 内蔵の DYNAMIC 版ではなく、ここで持つ DEFAULT 版を
	//! 使う。DYNAMIC + Map(WRITE_DISCARD) はリソース全体を捨てる契約なので部分更新
	//! ができず、変化が数十 px でも毎フレーム全面を転送することになるため。
	//! DEFAULT + UpdateSubresource(box) なら矩形だけ差し替えられる。
	struct Layer
	{
		std::vector<std::uint32_t> staging;
		int w = 0;
		int h = 0;
		tTVPVideoPresenterD3D * presenter = nullptr;  //!< BGRA→RTV ブリッタ (所有)
		ID3D11Texture2D * tex = nullptr;              //!< DEFAULT usage (所有)
		ID3D11ShaderResourceView * srv = nullptr;     //!< tex の SRV (所有)
		ID3D11Device * texDev = nullptr;              //!< tex を作ったデバイス (照合用・借用)
		bool uploaded = false;                        //!< 一度でも全面を上げたか
	};
	std::map<const void*, Layer> _layers;

	void DestroyLayer(Layer & layer);
	void DestroyTexture(Layer & layer);
	//! @brief L.tex を (dev, L.w, L.h) 用に用意する。デバイス/サイズ変化で作り直す。
	bool EnsureTexture(Layer & L, ID3D11Device * dev);
	//! @brief staging の矩形を tex へ転送する (w<=0 なら全面)。
	void UploadRect(const void* layer, int x, int y, int w, int h);
};

#endif // D3D11_DIALOG_RENDERER_H
