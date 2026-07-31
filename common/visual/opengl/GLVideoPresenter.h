/****************************************************************************/
/*! @file
@brief overlay 動画の OpenGL DrawDevice presenter インターフェース

WINVER の iTVPVideoPresenter (D3D11) / SDL の iTVPSDLVideoPresenter に対応する GL 版。
overlay 動画を、OGLDrawDevice の GL コンテキスト上でバックバッファへ合成 (pull 型) する。

  - iTVPGLVideoPresenter      … 動画側が実装する描画コールバック。OGLDrawDevice の Show()
                                (= GL コンテキストが current な描画スレッド) から毎フレーム呼ばれ、
                                現在の動画フレームを ctx.TextureDrawer で描く。
  - iTVPGLVideoPresenterHost  … OGLDrawDevice が実装する登録口。DrawDevice の TJS オブジェクトの
                                読み取り専用プロパティ "glVideoPresenterHost" が host ポインタを
                                tjs_int64 で返す。未対応デバイスは 0 を返す。

GL 操作 (テクスチャ生成/更新/削除、描画) はすべて RenderVideoFrame (描画スレッド、context current)
で行う。フレームの CPU バッファ充填のみデコードスレッドから来る。
*****************************************************************************/
#ifndef __GL_VIDEO_PRESENTER_H__
#define __GL_VIDEO_PRESENTER_H__

#include "tjsCommHead.h"
#include "ComplexRect.h"   // tTVPRect

class GLTextureDrawer;

//! @brief presenter が 1 フレーム描くのに必要なコンテキスト (OGLDrawDevice が Show() で構築)
struct tTVPGLVideoPresenterContext
{
	GLTextureDrawer* TextureDrawer;   //!< エンジンのテクスチャ描画器 (描画スレッド専用)
	tjs_int TargetWidth;              //!< 描画先サーフェス幅 px
	tjs_int TargetHeight;             //!< 描画先サーフェス高さ px
	tTVPRect DestRect;                //!< ゲーム画面 (プライマリレイヤ) がサーフェスに配置される矩形
	tjs_int SrcWidth;                 //!< プライマリレイヤ幅 (mixer 画像のプライマリ座標→描画先変換用)
	tjs_int SrcHeight;                //!< プライマリレイヤ高さ
};

//! @brief overlay 動画側が実装する描画コールバックインターフェース (GL)
class iTVPGLVideoPresenter
{
public:
	//! @brief 現在の動画フレームを ctx.TextureDrawer で描画する。
	//! @return 何か描いたら true。まだフレームが無い等で描かなければ false。
	virtual bool TJS_INTF_METHOD RenderVideoFrame( const tTVPGLVideoPresenterContext & ctx ) = 0;
};

//! @brief OGLDrawDevice 側が実装する presenter 登録インターフェース
class iTVPGLVideoPresenterHost
{
public:
	//! @brief presenter を登録する (以後 Show() で RenderVideoFrame が毎フレーム呼ばれる)。
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPGLVideoPresenter * presenter ) = 0;
	//! @brief presenter を登録解除する (再生停止・Close・破棄時に必ず呼ぶこと)。
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPGLVideoPresenter * presenter ) = 0;
};

//! @brief GL 版 presenter factory を presenter レジストリへ登録する (起動時に一度呼ぶ)。
void TVPRegisterGLVideoOverlayPresenterFactory();

#endif // __GL_VIDEO_PRESENTER_H__
