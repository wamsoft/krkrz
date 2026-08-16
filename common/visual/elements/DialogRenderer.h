//---------------------------------------------------------------------------
//!@file Elements ダイアログ描画アダプタ抽象
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_RENDERER_H
#define ELEMENTS_DIALOG_RENDERER_H

#include <cstdint>
#include "tjsTypes.h"   // TJS_EXP_FUNC_DEF (TVPRegisterDialogHost の tp_stub 公開)

class iTVPDrawDevice;   // 登録 API の引数 (前方宣言)

/*[*/
class iTVPDialogRenderer
{
public:
	virtual ~iTVPDialogRenderer() = default;

	// 表示先サーフェイスのサイズ (renderer の logical 座標空間)。
	// 中央配置の計算等に使う。失敗時は 0,0 を返す。
	virtual void GetSurfaceSize(int& w, int& h) = 0;

	// DrawDevice の DestRect (= ゲーム画像が描画される rect、 logical 座標
	// 空間の絶対位置)。マウス座標もこの rect の原点系で来るので、ダイアログの
	// 中央配置およびマウス座標変換の基準としてこちらを使う。
	// 取れない場合は (0, 0, surface_w, surface_h) でフォールバックすること。
	virtual void GetDestRect(int& x, int& y, int& w, int& h) = 0;

	// === レイヤ (= overlay インスタンス) 単位のテクスチャ管理 ===
	// 複数の非モーダル UI を同一フレーム内で重ねて表示できるように、 描画先
	// テクスチャ / ステージングバッファは `layer` キー (overlay インスタンスを
	// 一意に識別する不透明ポインタ) ごとに保持する。 同じ layer で同一サイズが
	// 連続する場合は再確保しないこと。 SDL_RenderTexture 等はテクスチャを
	// 参照キューイングするため、 単一テクスチャを使い回すと先に present した
	// レイヤの内容が壊れる。 layer ごとに別テクスチャを持つ必要がある。

	// Elements が描画する RGBA8888 ピクセルバッファ (layer 固有、 連続 pitch)。
	virtual uint32_t* AcquireBuffer(const void* layer, int w, int h) = 0;
	// 直近 AcquireBuffer した layer のステージングをテクスチャへアップロード。
	virtual void ReleaseBuffer(const void* layer) = 0;

	// layer のバッファ内容を画面の (x, y, w, h) に貼る (surface 座標)。
	// DrawDevice::Show() 終端で、 z-order 奥→手前の順に layer ごとに呼ばれる。
	virtual void PresentOverlay(const void* layer, int x, int y, int w, int h) = 0;

	// overlay インスタンスが閉じられたとき、 その layer のテクスチャ /
	// ステージングを破棄する。 未知の layer は no-op。
	virtual void ReleaseLayer(const void* layer) = 0;

	// ReleaseBuffer の部分転送版: staging のうち (x, y, w, h) だけをテクスチャへ
	// アップロードする (部分再描画時の転送コスト削減)。 staging とテクスチャの
	// 残部には前回フレームの内容が維持されている前提 (AcquireBuffer が同一
	// layer・同一サイズで staging を再利用すること)。 既定実装は全面
	// ReleaseBuffer へのフォールバック (未対応レンダラでも正しさは保たれる)。
	// ※ 既存レンダラ実装の ABI 互換のため vtable 末尾に追加している。
	virtual void ReleaseBufferRect(const void* layer, int x, int y, int w, int h)
	{
		(void)x; (void)y; (void)w; (void)h;
		ReleaseBuffer(layer);
	}
};

//---------------------------------------------------------------------------
//!@brief DrawDevice が実装する「ダイアログ描画アダプタの提供口」。
//
// overlay 動画の iTVPVideoPresenterHost と対になる設計。 各 DrawDevice は自分の
// iTVPDialogRenderer 実装 (SDL_Texture / GLTexture / D3D11 等) を所有し、この
// インターフェース経由で貸し出す。 tTVPElementsDialogManager は具象 renderer 型を
// 知らずに host 経由で renderer を取得する。
//
// DrawDevice はさらに TJS の読み取り専用プロパティ "dialogRendererHost" で自身
// (iTVPDialogRendererHost*) のポインタを tjs_int64 として公開する (videoPresenterHost
// と同じ規約)。 これにより、 プラグイン等の差し替え DrawDevice も同じ構造 —
// iTVPDialogRendererHost を実装 + renderer を所有 + manager へ RegisterDialogHost —
// を実装するだけで overlay ダイアログ描画に参加できる。
//---------------------------------------------------------------------------
class iTVPDialogRendererHost
{
public:
	virtual ~iTVPDialogRendererHost() = default;

	//! このDrawDeviceが所有するダイアログ描画アダプタ。 DrawDevice が所有し続ける
	//! (呼出側は delete しない)。 未対応 DrawDevice / まだ生成前は nullptr を返してよい。
	virtual iTVPDialogRenderer* GetDialogRenderer() = 0;
};
/*]*/

//---------------------------------------------------------------------------
// プラグイン / 差し替え DrawDevice 向け登録 API (tp_stub 公開)
//
// 差し替え DrawDevice は iTVPDialogRendererHost を実装し、自身を host として登録
// する。 これで engine 内蔵の tTVPElementsDialogManager が具象型を知らずに overlay
// ダイアログを描画する。 通常は DrawDevice の生成/破棄 (or context init/done) で呼ぶ。
// (engine 内蔵 DrawDevice は直接 manager を呼ぶのでこの API は使わない。)
//
// KRKRZ_USE_ELEMENTS=OFF ビルドでは実装 (ElementsDialogManager.cpp) がリンクされない
// ため、 stub 生成も KRKRZ_HAS_ELEMENTS でガードする (_ENV 版マクロ)。
//---------------------------------------------------------------------------
TJS_EXP_FUNC_DEF_ENV(KRKRZ_HAS_ELEMENTS, void, TVPRegisterDialogHost, (iTVPDrawDevice* device, iTVPDialogRendererHost* host));
TJS_EXP_FUNC_DEF_ENV(KRKRZ_HAS_ELEMENTS, void, TVPUnregisterDialogHost, (iTVPDrawDevice* device));

#endif
