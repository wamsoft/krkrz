//---------------------------------------------------------------------------
/**
 * @file VirtualCursor.h
 * @brief ウィンドウごとの「仮想カーソル位置」
 *
 * hover 判定や `Layer.cursorX/cursorY` が見るカーソル位置を、OS の実カーソル
 * ではなく**エンジンが持つ位置**にする。書き手は 3 つ:
 *
 *   1. 実マウスの移動 … `OnMouseMove` から。ウィンドウ内の移動しか来ないので、
 *      「ウィンドウ外の実マウスは無視して現状維持」が自動的に成り立つ。
 *   2. キー / パッドのナビ … Elements の cursor-warp 等。**実カーソルは触らず**
 *      仮想位置だけ動かす。
 *   3. `Layer.setCursorPos` … 従来どおり実カーソルも動かし、仮想位置も揃える。
 *
 * こうすると「この mouse move は自分が動かしたカーソルの折返しか？」という
 * 推測が要らなくなる (OS を一往復しないため)。設計と経緯は
 * doc/VirtualCursor.md を参照。
 */
//---------------------------------------------------------------------------
#ifndef VirtualCursorH
#define VirtualCursorH

#include "tjsCommHead.h"

//---------------------------------------------------------------------------
//  実マウス入力の無視 (動作テスト用)
//---------------------------------------------------------------------------
//! @brief 真のとき、**実マウスの入力を捨てる** (Agent の注入だけを通す)。
//!
//! 自動テストで「入力は全部 Agent が出す」前提を作るためのもの。人が
//! うっかりポインタを動かしても測定が汚れない。cursor-warp まわりの計測が
//! 実マウスの移動で汚染され、誤った不具合起票までした経験から入れた
//! (doc/VirtualCursor.md / doc/ElementsAudit.md §1-b)。
//!
//! 起動オプション `-ignoremouse=yes` で有効。実行中は `Agent.ignoreRealMouse`
//! で切り替えられる。仮想カーソル位置は Agent 注入で更新されるので、
//! hover もフォーカスも従来どおり動く。
inline bool TVPIgnoreRealMouse = false;

//! @brief Agent がマウス入力を注入している最中かどうか (内部用)。
//!        `TVPIgnoreRealMouse` の判定で「実入力」と「注入」を分けるために使う。
inline bool TVPAgentMouseInjecting = false;

//! @brief `TVPAgentMouseInjecting` を立てるスコープガード (内部用)。
struct tTVPAgentMouseInjectScope
{
	tTVPAgentMouseInjectScope()  { TVPAgentMouseInjecting = true; }
	~tTVPAgentMouseInjectScope() { TVPAgentMouseInjecting = false; }
};

//! @brief この実マウス入力を捨てるべきか。
inline bool TVPShouldDropRealMouse()
{
	return TVPIgnoreRealMouse && !TVPAgentMouseInjecting;
}

//---------------------------------------------------------------------------
//! @brief 仮想カーソル位置 (描画矩形内の座標)
//!
//! 未設定のうちは「無効」で、読み出し側は実カーソルへフォールバックする
//! (起動直後にポインタがどこにあるか分からない状態)。一度でも実マウスが
//! ウィンドウ内で動くか、ホストが明示的に置けば有効になる。
//---------------------------------------------------------------------------
class tTVPVirtualCursor
{
	tjs_int X_ = 0;
	tjs_int Y_ = 0;
	bool    Valid_ = false;

public:
	//! @brief 位置を取得する。
	//! @return 有効なら true (x/y へ格納)。無効なら false (x/y は不変)。
	bool Get(tjs_int &x, tjs_int &y) const
	{
		if (!Valid_) return false;
		x = X_;
		y = Y_;
		return true;
	}

	//! @brief 位置を設定する (以後 Get が有効になる)。
	void Set(tjs_int x, tjs_int y) { X_ = x; Y_ = y; Valid_ = true; }

	//! @brief 無効化する (以後は実カーソルへフォールバック)。
	void Invalidate() { Valid_ = false; }

	bool IsValid() const { return Valid_; }
};
//---------------------------------------------------------------------------
#endif
