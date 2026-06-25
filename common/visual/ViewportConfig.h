//---------------------------------------------------------------------------
/*
	ゲーム画面 (内側プライマリレイヤ) を外側 surface (= ウインドウ client /
	innerWidth・innerHeight) の中へどう配置・スケールするかを表すビューポート設定。

	- 外側 surface サイズ = innerWidth/innerHeight (WINVER/SDL とも物理 client)
	- 内側ゲームサイズ    = プライマリレイヤサイズ (layerW/layerH)
	- この設定が両者の橋渡し (fit / zoom / align / offset) を決める。

	従来 SDL の CalcDestRect は contain + center 固定だったが、これを一般化する。
	WINVER 側はいったん据え置き (client が zoom ロックのため非適用)。
*/
//---------------------------------------------------------------------------
#ifndef ViewportConfigH
#define ViewportConfigH

#include "ComplexRect.h"
#include <algorithm>
#include <cmath>

/*[*/
//---------------------------------------------------------------------------
//! @brief	ゲーム画面のフィット方式 (CSS object-fit + ピクセルパーフェクト系)
//---------------------------------------------------------------------------
enum tTVPViewportFit {
	vfContain = 0, //!< アスペクト維持で収まる最大 (従来デフォルト / letterbox)
	vfCover,       //!< アスペクト維持で埋める最小 (はみ出しは clip)
	vfFill,        //!< アスペクト無視で surface 全面へ引き伸ばし
	vfNone,        //!< 原寸 (scale = 1.0)
	vfInteger,     //!< 収まる範囲で最大の整数倍 (最低 1 倍、ドット等倍維持)
	vfCustom,      //!< 明示倍率 (customScale) を使用
};
/*]*/

//---------------------------------------------------------------------------
//! @brief	ビューポート配置設定 (配置・スケールのみ。余白色/壁紙は別管理)
//---------------------------------------------------------------------------
struct tTVPViewportConfig {
	tTVPViewportFit fit;     //!< フィット方式
	double customScale;      //!< vfCustom 時の倍率 (1.8 = 180%)
	double alignX;           //!< 水平配置 0=左 0.5=中央 1=右
	double alignY;           //!< 垂直配置 0=上 0.5=中央 1=下
	tjs_int offsetX;         //!< 水平オフセット (px, surface 座標, align 後加算)
	tjs_int offsetY;         //!< 垂直オフセット (px, surface 座標, align 後加算)

	tTVPViewportConfig()
		: fit(vfContain), customScale(1.0)
		, alignX(0.5), alignY(0.5)
		, offsetX(0), offsetY(0) {}
};

//---------------------------------------------------------------------------
//! @brief	設定から描画先矩形 (DestRect) を計算する
//! @param	cfg		ビューポート設定
//! @param	sw,sh	外側 surface サイズ
//! @param	lw,lh	内側 (ゲーム/壁紙) サイズ
//! @return	surface 座標での描画先矩形。cover/custom 等で surface をはみ出す
//!			こともある (描画側が clip する想定)。
//---------------------------------------------------------------------------
inline tTVPRect TVPCalcViewportDestRect(const tTVPViewportConfig &cfg,
	tjs_int sw, tjs_int sh, tjs_int lw, tjs_int lh)
{
	if (sw <= 0 || sh <= 0 || lw <= 0 || lh <= 0) {
		return tTVPRect(0, 0, sw > 0 ? sw : 1, sh > 0 ? sh : 1);
	}

	double sx, sy;
	switch (cfg.fit) {
	case vfContain: {
		double s = std::min((double)sw / lw, (double)sh / lh);
		sx = sy = s;
		break;
	}
	case vfCover: {
		double s = std::max((double)sw / lw, (double)sh / lh);
		sx = sy = s;
		break;
	}
	case vfFill:
		sx = (double)sw / lw;
		sy = (double)sh / lh;
		break;
	case vfNone:
		sx = sy = 1.0;
		break;
	case vfInteger: {
		double s = std::min((double)sw / lw, (double)sh / lh);
		int n = (int)std::floor(s);
		if (n < 1) n = 1;
		sx = sy = (double)n;
		break;
	}
	case vfCustom:
		sx = sy = (cfg.customScale > 0.0) ? cfg.customScale : 1.0;
		break;
	default:
		sx = sy = 1.0;
		break;
	}

	tjs_int nw = (tjs_int)(lw * sx + 0.5);
	tjs_int nh = (tjs_int)(lh * sy + 0.5);
	if (nw < 1) nw = 1;
	if (nh < 1) nh = 1;

	tjs_int offx = (tjs_int)((sw - nw) * cfg.alignX + 0.5) + cfg.offsetX;
	tjs_int offy = (tjs_int)((sh - nh) * cfg.alignY + 0.5) + cfg.offsetY;

	return tTVPRect(offx, offy, offx + nw, offy + nh);
}

//---------------------------------------------------------------------------
#endif
