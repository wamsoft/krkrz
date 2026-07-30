/****************************************************************************/
/*! @file
@brief overlay 動画の DrawDevice presenter インターフェース (Track V-E)

overlay 動画を、子ウィンドウの独立 D3D11 デバイスではなく、エンジン (BasicDrawDevice)
の単一 D3D11 デバイス上でバックバッファへ合成 (pull 型) するための 2 インターフェース。

  - iTVPVideoPresenter      … overlay 動画側 (tTJSNI_VideoOverlay) が実装する描画コールバック。
                              DrawDevice の Show() (描画スレッド) から毎フレーム呼ばれ、現在の
                              動画フレーム + mixer 追加画像を engine の dev/ctx で RTV へ描く。
  - iTVPVideoPresenterHost  … DrawDevice 側 (BasicDrawDevice) が実装する登録口。
                              DrawDevice の TJS オブジェクトの読み取り専用プロパティ
                              "videoPresenterHost" が host ポインタを tjs_int64 で返す。未対応の
                              描画デバイスは 0 を返し、overlay は自前の子ウィンドウ present
                              (tTVPD3D11OverlayWindow) にフォールバックする。

設計の要 (スレッド境界): BasicDrawDevice は単一 D3D11 デバイスで ImmediateContext は描画
スレッド専用。デコードスレッドから触ると壊れるので「push」でなく「描画スレッド(Show())が
pull」する。VideoOverlay は host に登録するだけで、実際のフレーム取り出し + アップロード +
描画は Show() 内 (描画スレッド) の RenderVideoFrame で行う。

このインターフェースは tp_stub 公開されている (WINVER 専用)。D3D11 の型は前方宣言のみを
使い、tp_stub / プラグインへ d3d11.h を持ち込まない (ポインタとしてのみ扱う)。custom
DrawDevice / custom 動画プラグインが参加する場合はこのヘッダを直接 include する。
*****************************************************************************/
#ifndef __VIDEO_PRESENTER_H__
#define __VIDEO_PRESENTER_H__

#include "tjsCommHead.h"   // tjs_int / tjs_uint / TJS_INTF_METHOD
#include "ComplexRect.h"   // tTVPRect

// D3D11 の型は前方宣言のみ (d3d11.h を include しない)。ポインタとしてのみ扱う。実際に
// 描画する側 (BasicDrawDevice / VideoPresenterD3D) は別途 d3d11.h を include する。
// tp_stub 側では同等の前方宣言をプリアンブル (名前空間外) に置く (makestub.pl)。
// この前方宣言は tp_stub 抽出マーカーの外に置く。マーカー内 (krkrz_plugin 名前空間) に
// 入れるとプラグインが include する d3d11.h の ::ID3D11Device と衝突するため。
#ifndef __d3d11_h__
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
#endif

/*[*/
#ifdef __WINVER__
//---------------------------------------------------------------------------
// overlay 動画 presenter インターフェース (Track V-E, WINVER 専用)
//---------------------------------------------------------------------------
//! @brief presenter が 1 フレーム描くのに必要なコンテキスト (DrawDevice が Show() で構築)
struct tTVPVideoPresenterContext
{
	ID3D11Device*           Device;        //!< エンジンの D3D11 デバイス
	ID3D11DeviceContext*    Context;       //!< ImmediateContext (描画スレッド専用)
	ID3D11RenderTargetView* RenderTarget;  //!< バックバッファ RTV
	tjs_uint TargetWidth;   //!< バックバッファ (=クライアント) 幅 px
	tjs_uint TargetHeight;  //!< バックバッファ (=クライアント) 高さ px
	tTVPRect DestRect;      //!< ゲーム画面 (プライマリレイヤ) がクライアントに配置される矩形 px
	tTVPRect ClipRect;      //!< クリップ矩形 px
	tjs_int  SrcWidth;      //!< プライマリレイヤ幅 (DestRect にスケールされる元寸)
	tjs_int  SrcHeight;     //!< プライマリレイヤ高さ
};

//! @brief overlay 動画側が実装する描画コールバックインターフェース
class iTVPVideoPresenter
{
public:
	//! @brief 現在の動画フレーム (+ mixer 追加画像) を ctx.RenderTarget へ描画する。
	//! @return 何か描いたら true。まだフレームが無い等で描かなければ false。
	virtual bool TJS_INTF_METHOD RenderVideoFrame( const tTVPVideoPresenterContext & ctx ) = 0;
};

//! @brief DrawDevice 側が実装する presenter 登録インターフェース
class iTVPVideoPresenterHost
{
public:
	//! @brief presenter を登録する (以後 Show() で RenderVideoFrame が毎フレーム呼ばれる)。
	virtual void TJS_INTF_METHOD AddVideoPresenter( iTVPVideoPresenter * presenter ) = 0;
	//! @brief presenter を登録解除する (再生停止・Close・破棄時に必ず呼ぶこと)。
	virtual void TJS_INTF_METHOD RemoveVideoPresenter( iTVPVideoPresenter * presenter ) = 0;
};
#endif // __WINVER__
/*]*/

#endif // __VIDEO_PRESENTER_H__
