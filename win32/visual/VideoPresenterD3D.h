/****************************************************************************/
/*! @file
@brief overlay 動画 presenter 用 BGRA→RTV ブリッタ (Track V-E)

エンジン (BasicDrawDevice) の D3D11 デバイス上で、CPU デコード済みの BGRA フレームを
1 枚テクスチャへ上げ、バックバッファ RTV の指定矩形へアルファ合成で描く軽量ブリッタ。
tTJSNI_VideoOverlay が「動画本体用」「mixer 追加画像用」に各 1 インスタンス保持する。

自前 D3D11 デバイスを持つ子ウィンドウ (tTVPD3D11OverlayWindow) と違い、描画先は engine の
共有デバイス。描画は必ず描画スレッド (DrawDevice::Show()) から呼ばれる。デバイスや
フレームサイズが変わったら遅延で作り直す (デバイスロスト対応)。
*****************************************************************************/
#ifndef __VIDEO_PRESENTER_D3D_H__
#define __VIDEO_PRESENTER_D3D_H__

#include <d3d11.h>
#include "VideoPresenter.h"   // tTVPVideoPresenterContext / tTVPRect

class tTVPVideoPresenterD3D
{
public:
	tTVPVideoPresenterD3D();
	~tTVPVideoPresenterD3D();

	//! @brief BGRA (B8G8R8A8, top-down) の 1 フレームを destClientPx へ alpha 合成で描く。
	//! @param ctx          DrawDevice が与える描画コンテキスト (device/context/target サイズ)
	//! @param topRow       視覚的な先頭行 (top) のピクセル先頭
	//! @param pitch        次の行への符号付きバイト差 (ボトムアップ格納なら負値可)
	//! @param w,h          フレーム画素サイズ
	//! @param destClientPx 描画先矩形 (クライアント px)
	//! @param alpha        全体アルファ (0..1)。フレームのアルファにさらに乗算される。
	//! @return 描画できたら true
	bool Render( const tTVPVideoPresenterContext & ctx,
	             const void * topRow, int pitch, int w, int h,
	             const tTVPRect & destClientPx, float alpha );

	//! @brief 既に GPU 上にある BGRA テクスチャ (SRV) を destClientPx へ alpha 合成で描く。
	//! CPU アップロードは行わない (HW 動画 = MediaEngine の TransferVideoFrame 出力の present 用)。
	//! srv は ctx.Device 上のテクスチャの SRV であること。
	bool RenderSRV( const tTVPVideoPresenterContext & ctx,
	                ID3D11ShaderResourceView * srv, int w, int h,
	                const tTVPRect & destClientPx, float alpha );

	//! @brief I420 (planar YUV420, BT.601 limited range) の 1 フレームを destClientPx へ描く。
	//! Y/U/V の 3 プレーンを R8 テクスチャへ上げ、シェーダで YUV→RGB 変換して alpha 合成する
	//! (CPU での色変換を省き GPU で行う)。u/v プレーンは w/2 × h/2。
	//! @return 描画できたら true
	bool RenderI420( const tTVPVideoPresenterContext & ctx,
	                 const void * y, int yStride, const void * u, int uStride,
	                 const void * v, int vStride, int w, int h,
	                 const tTVPRect & destClientPx, float alpha );

	//! GPU リソースを解放する (Close / 破棄時)。
	void Release();

private:
	bool EnsurePipeline( ID3D11Device * dev );
	bool EnsureTexture( ID3D11Device * dev, int w, int h );
	bool EnsureYUVShader( ID3D11Device * dev );
	bool EnsurePlaneTextures( ID3D11Device * dev, int w, int h );
	//! パイプライン設定 + クアッド描画。srv[0..count-1] を samp で dst へ描く (ps 指定)。
	void DrawQuad( ID3D11DeviceContext * ictx, ID3D11ShaderResourceView * const * srv, int srvCount,
	               ID3D11PixelShader * ps, const tTVPVideoPresenterContext & ctx,
	               const tTVPRect & dst, float alpha );

	ID3D11Device*             Dev;   //!< 現在リソースが属するデバイス (変化検出用、非所有)
	ID3D11VertexShader*       VS;
	ID3D11PixelShader*        PS;
	ID3D11InputLayout*        IL;
	ID3D11Buffer*             VB;    //!< 4 頂点 (TRIANGLESTRIP) DYNAMIC
	ID3D11Buffer*             CB;    //!< 定数バッファ (全体アルファ)
	ID3D11SamplerState*       Samp;
	ID3D11BlendState*         Blend;
	ID3D11Texture2D*          Tex;   //!< BGRA DYNAMIC
	ID3D11ShaderResourceView* Srv;
	int TexW, TexH;

	// --- I420 (YUV) 経路 ---
	ID3D11PixelShader*        PSYUV; //!< I420→RGB(BT.601) PS。VS は BGRA と共用。
	ID3D11Texture2D*          TexY;  //!< R8 DYNAMIC (Y full / U,V half)
	ID3D11Texture2D*          TexU;
	ID3D11Texture2D*          TexV;
	ID3D11ShaderResourceView* SrvY;
	ID3D11ShaderResourceView* SrvU;
	ID3D11ShaderResourceView* SrvV;
	int PlaneW, PlaneH;
};

#endif // __VIDEO_PRESENTER_D3D_H__
