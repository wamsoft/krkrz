/****************************************************************************/
/*! @file
@brief 動画オーバレイの「フレーム供給 + DrawDevice への pull 登録」を担う中立インターフェース

WINVER (BasicDrawDevice) が overlay 動画を pull 型で合成するのと同じ構造を、SDL / OGL 等の
generic 系 DrawDevice にも広げるための土台。generic 版 VideoOverlay
(generic/visual/VideoOvlImpl.cpp、SDL / OGL / NX / PS5 で共有) は、この中立 IF 越しに
「フレームを渡す」「DrawDevice へ登録する」を行い、実体 (SDL_Renderer / GL へ描く presenter) は
各環境が factory 経由で供給する。これにより generic 側は特定の描画 API に依存しない。

環境別の描画コールバック IF (SDL=iTVPSDLVideoPresenter、GL=iTVPGLVideoPresenter) は各環境の
ヘッダで定義し、その実体がこの中立 IF も実装して factory から返る。DrawDevice が対応 host を
公開しなければ RegisterWith() は false を返し、呼び側 (VideoOverlay) は従来の push 経路
(DrawDevice::UpdateVideo) にフォールバックする。
*****************************************************************************/
#ifndef __VIDEO_OVERLAY_PRESENTER_H__
#define __VIDEO_OVERLAY_PRESENTER_H__

#include "tjsCommHead.h"
#include "tjsVariant.h"
#include "MoviePlayer.h"   // iTVPMoviePlayer::VideoPlaneFrame (YUV)
#include "ComplexRect.h"   // tTVPRect
#include <functional>

//! @brief フレーム供給 + DrawDevice 登録を担う環境別 presenter の中立 IF
class iTVPVideoOverlayPresenter
{
public:
	virtual ~iTVPVideoOverlayPresenter() {}

	//! @brief 現在の DrawDevice (TJS オブジェクト) が公開する presenter host を引いて束縛する。
	//!        pull はまだ開始しない (Activate で開始)。対応 host が無ければ false。
	virtual bool Bind(const tTJSVariant &drawDeviceObj) = 0;

	//! @brief pull を開始する (host に自身を登録)。Bind 済みであること。
	virtual void Activate() = 0;
	//! @brief pull を停止する (host から自身を登録解除)。object は保持 (replay 可)。
	virtual void Deactivate() = 0;

	//! @brief overlay の表示可否を設定する (WINVER の VideoOverlay.visible と同仕様。既定 false)。
	//!        false の間、DrawDevice は presenter を pull せずゲーム画面を描く。再生ライフサイクル
	//!        (Activate/Deactivate) とは独立で、再生中でも visible を切り替えると表示が切り替わる。
	//!        WINVER は RenderVideoFrame 内で Visible を毎フレーム判定するのと等価。
	virtual void SetVisible(bool visible) {}

	//! @brief この presenter が YUV plane 経路 (UpdateFrameYUV + GPU 側 YUV→RGB) に対応するか。
	//!        false の場合、呼び側は ARGB 経路 (UpdateFrame) を使う。
	virtual bool SupportsYUV() const { return false; }

	//! @brief decode スレッドから最新フレーム (ARGB) を内部バッファへ取り込む。
	//! @param updater  presenter が用意した dest バッファを ARGB8888 で埋めるコールバック
	virtual void UpdateFrame(int w, int h, std::function<void(char *dest, int pitch)> updater) = 0;

	//! @brief decode スレッドから最新フレーム (YUV plane) を内部バッファへ取り込む。
	//!        SupportsYUV()==true の presenter のみ呼ばれる。plane data はコールバック内で copy 済み。
	virtual void UpdateFrameYUV(const iTVPMoviePlayer::VideoPlaneFrame &frame) {}

	//! @brief 保持中のフレーム / テクスチャを破棄 (再生停止・Close 時)。
	virtual void ClearFrame() = 0;

	//! @brief mixer 追加画像を設定する (動画の上へ α 合成で重ねる。setMixingLayer の後継)。
	//!        bgra は ARGB8888 (メモリ上 B,G,R,A) top-down。primaryRect はプライマリレイヤ座標での
	//!        配置矩形 (presenter が game 画面のマッピングに合わせて描画先へ変換する)。データは
	//!        presenter が内部へ copy する。未対応 presenter は既定 no-op。
	virtual void SetMixerImage(const void *bgra, int w, int h, int pitch,
	                           const tTVPRect &primaryRect, float alpha) {}
	//! @brief mixer 追加画像の全体アルファのみ更新する。
	virtual void SetMixerAlpha(float alpha) {}
	//! @brief mixer 追加画像を消す。
	virtual void ClearMixerImage() {}
};

//! @brief presenter factory の型 (各環境が起動時に自分の factory を登録する)。
typedef iTVPVideoOverlayPresenter * (*tTVPVideoOverlayPresenterFactory)();

//! @brief 環境別 presenter factory を登録する (SDL / GL 等が起動時に呼ぶ、冪等)。
//!        複数登録可 (SDL と GL が両方登録され、現行デバイスに合う方が採用される)。
void TVPRegisterVideoOverlayPresenterFactory(tTVPVideoOverlayPresenterFactory f);

//! @brief 登録済み factory を順に試し、指定 DrawDevice に RegisterWith が成功した
//!        presenter を返す (登録済みで返る)。どれも対応しなければ nullptr
//!        (= pull 非対応デバイス。VideoOverlay は push 経路を使う)。
iTVPVideoOverlayPresenter * TVPCreateBoundVideoOverlayPresenter(const tTJSVariant &drawDeviceObj);

#endif // __VIDEO_OVERLAY_PRESENTER_H__
