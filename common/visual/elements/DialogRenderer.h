//---------------------------------------------------------------------------
//!@file Elements ダイアログ描画アダプタ抽象
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_RENDERER_H
#define ELEMENTS_DIALOG_RENDERER_H

#include <cstdint>

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
};

#endif
