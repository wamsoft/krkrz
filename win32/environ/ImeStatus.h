#ifndef __IME_STATUS_H__
#define __IME_STATUS_H__

#include "tjsTypes.h"

/**
 * IME 関連の状態スナップショット (診断用。 Agent.imeStatus() が返す)。
 *
 * 「入力欄にキャレットは出ているのに日本語が打てない」の切り分け用。
 * Window.imeMode の getter は既定値 (defaultImeMode) しか返さず、 実際に
 * 適用しているモードや入力コンテキストの有無は見えないため、 ここで一式返す。
 *
 * 依存を持たせないよう TTVPWindowForm の外に置いてある (WindowImpl.h は
 * TTVPWindowForm を前方宣言しか持たない)。
 */
struct tTVPImeStatus {
	bool visible;           //!< ウィンドウが表示されているか
	bool hasFocus;          //!< ::GetFocus() == この窓 (AcquireImeControl のガード)
	bool trapKeys;          //!< Window.trapKey
	bool keyTrapperIsSelf;  //!< GetKeyTrapperWindow() == this (偽なら他窓のモードが適用される)
	bool attentionPoint;    //!< 注視ポイントが有効か
	bool controlImeState;   //!< -controlime=no で切られていないか
	bool contextAttached;   //!< 入力コンテキストが結び付いているか (偽なら IME は完全に無効)
	bool disabledBySelf;    //!< 本体側 (ImeControl::Disable) で切っているか
	bool imeAvailable;      //!< 現在のキーボードレイアウトが IME か
	bool open;              //!< IME が開いているか
	bool overrideActive;    //!< オーバレイ UI (Elements のテキスト欄) が握っているか
	bool contextForced;     //!< 上書きのために入力コンテキストを結び直したか
	tjs_int lastSetImeMode; //!< 実際に適用しているモード
	tjs_int defaultImeMode; //!< Window.imeMode (既定値)
	tjs_int savedImeMode;   //!< 上書き中に保留しているモード
	tjs_uint32 conversion;  //!< IME 変換モード (0 = コンテキスト無し)
	tjs_uint32 sentence;    //!< IME 文節モード
};

#endif // __IME_STATUS_H__
