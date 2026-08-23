//---------------------------------------------------------------------------
/*
	ビューポート余白塗り (ゲーム画面が surface 全面を覆わないときの周囲) の
	登録口インターフェース。

	iTVPDrawDevice 本体には載せず、対応している描画デバイスだけが
	TJS プロパティ "viewportBackgroundHost" でこのインターフェースへの
	ポインタを公開する、という規約にしている (WINVER の videoPresenterHost /
	dialogRendererHost と同じ方式)。 プロパティが無い / 0 を返す描画デバイス
	(NullDrawDevice やプラグイン製のカスタムデバイス等) では、Window 側が
	余白設定の反映をスキップするだけで何も壊れない。

	この方式にしているのは、iTVPDrawDevice の vtable を増やさないため。
	インターフェースに仮想関数を足すと、既存プラグインが実装した描画デバイスの
	vtable と食い違って壊れる。

	詳細 = src/core/doc/WindowGeometry.md
*/
//---------------------------------------------------------------------------
#ifndef ViewportBackgroundH
#define ViewportBackgroundH

#include "ViewportConfig.h"

/*[*/
//---------------------------------------------------------------------------
//! @brief	ビューポート余白 (背景色 + 壁紙) の設定を受け取る登録口
//---------------------------------------------------------------------------
class iTVPViewportBackgroundHost
{
public:
	//! @brief		余白の背景色を設定する
	//! @param		color	0xAARRGGBB
	virtual void SetViewportBackgroundColor(tjs_uint32 color) = 0;

	//! @brief		余白の壁紙を設定する
	//! @param		image	壁紙となる Layer / Bitmap オブジェクトを保持する Variant。
	//!						void / null でクリア。tTJSVariant が参照を保持するので
	//!						イメージデータは維持される。描画デバイス (プラグイン可) は
	//!						imageWidth/imageHeight/mainImageBuffer/mainImageBufferPitch
	//!						プロパティから画像イメージを取得する (内部型は渡さない)。
	//! @param		fit		壁紙のフィット方式
	//! @param		alignX	水平配置 0..1
	//! @param		alignY	垂直配置 0..1
	virtual void SetViewportWallpaper(const tTJSVariant &image,
		tTVPViewportFit fit, double alignX, double alignY) = 0;

	//! @brief		余白の壁紙をクリアする
	virtual void ClearViewportWallpaper() = 0;
};
//---------------------------------------------------------------------------
/*]*/

//---------------------------------------------------------------------------
//! @brief	描画デバイスの TJS オブジェクトから余白塗りの登録口を得る
//! @param	ddobj	Window が持つ DrawDevice の TJS オブジェクト
//! @return	対応していれば登録口、非対応なら NULL
//! @note	規定プロパティ "viewportBackgroundHost" を読む。プロパティが無い /
//!			0 の描画デバイスは非対応 (余白は各デバイスの既定の塗りつぶしのまま)。
//---------------------------------------------------------------------------
iTVPViewportBackgroundHost * TVPQueryViewportBackgroundHost(const tTJSVariant &ddobj);

#endif
