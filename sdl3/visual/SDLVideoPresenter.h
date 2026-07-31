/****************************************************************************/
/*! @file
@brief overlay 動画の SDL DrawDevice presenter インターフェース

WINVER の iTVPVideoPresenter / iTVPVideoPresenterHost (D3D11) に対応する SDL 版。
overlay 動画を、SDLDrawDevice の SDL_Renderer 上でバックバッファへ合成 (pull 型) する。

  - iTVPSDLVideoPresenter      … 動画側が実装する描画コールバック。SDLDrawDevice の Show()
                                  (描画スレッド) から毎フレーム呼ばれ、現在の動画フレームを
                                  ctx.Renderer へ描く。
  - iTVPSDLVideoPresenterHost  … SDLDrawDevice が実装する登録口。DrawDevice の TJS オブジェクトの
                                  読み取り専用プロパティ "sdlVideoPresenterHost" が host ポインタを
                                  tjs_int64 で返す。未対応デバイスは 0 を返す。

WINVER と同じスレッド境界 (デコードスレッドから触らず、描画スレッドが pull) を守る。
VideoOverlay は host に登録するだけで、フレームのアップロード + 描画は Show() 内 (描画スレッド)
の RenderVideoFrame で行う。
*****************************************************************************/
#ifndef __SDL_VIDEO_PRESENTER_H__
#define __SDL_VIDEO_PRESENTER_H__

#include "tjsCommHead.h"
#include "ComplexRect.h"

struct SDL_Renderer;

//! @brief presenter が 1 フレーム描くのに必要なコンテキスト (SDLDrawDevice が Show() で構築)
struct tTVPSDLVideoPresenterContext
{
	SDL_Renderer* Renderer;      //!< エンジンの SDL_Renderer (描画スレッド専用)
	tjs_int TargetWidth;         //!< 描画先の論理サイズ幅 (= SetRenderLogicalPresentation の値)
	tjs_int TargetHeight;        //!< 描画先の論理サイズ高さ
	tTVPRect DestRect;           //!< ゲーム画面 (プライマリレイヤ) が論理座標に配置される矩形
	tjs_int SrcWidth;            //!< プライマリレイヤ幅 (mixer 画像のプライマリ座標→描画先変換用)
	tjs_int SrcHeight;           //!< プライマリレイヤ高さ
};

//! @brief overlay 動画側が実装する描画コールバックインターフェース (SDL)
class iTVPSDLVideoPresenter
{
public:
	//! @brief 現在の動画フレームを ctx.Renderer へ描画する。
	//! @return 何か描いたら true。まだフレームが無い等で描かなければ false。
	virtual bool TJS_INTF_METHOD RenderVideoFrame( const tTVPSDLVideoPresenterContext & ctx ) = 0;
};

//! @brief SDLDrawDevice 側が実装する presenter 登録インターフェース
class iTVPSDLVideoPresenterHost
{
public:
	//! @brief presenter を登録する (以後 Show() で RenderVideoFrame が毎フレーム呼ばれる)。
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPSDLVideoPresenter * presenter ) = 0;
	//! @brief presenter を登録解除する (再生停止・Close・破棄時に必ず呼ぶこと)。
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPSDLVideoPresenter * presenter ) = 0;
};

//! @brief SDL 版 presenter factory を presenter レジストリへ登録する (起動時に一度呼ぶ)。
void TVPRegisterSDLVideoOverlayPresenterFactory();

#endif // __SDL_VIDEO_PRESENTER_H__
