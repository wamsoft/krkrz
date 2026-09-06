//---------------------------------------------------------------------------
//!@file Elements の画面をホストのレイヤへ描くパネル
//
// overlay ダイアログ (`tTVPElementsDialogManager`) は DrawDevice::Show() の
// 終端で画面へ直接貼るので **常に最前面**で、 ゲームのレイヤツリーのどこにも
// 属さない。 そのため
//
//   - 本文の上に窓の絵が被る (テキストを出す画面を Elements で組めない)
//   - `piledCopy` に写らない (ホストのスクリーンショットに入らない)
//   - 表 / 裏のどちらでもないので `[trans]` に乗らない
//   - 下のレイヤの入力を食べる
//
// という制約がある。 このパネルは **`render_to_buffer` の書き込み先を
// ホストのレイヤのビットマップにする**ことでその 4 つを一度に外す。
// z 順・トランジション・スクリーンショット・入力の帰属は、 すべて吉里吉里の
// レイヤの仕組みがそのまま面倒を見る。
//
// **overlay ダイアログとは完全に別枠**で、 manager の
// `Impl::instances` には入らない。 したがって
//
//   - `IsModalActive()` (DrawDevice の入力インターセプトのゲート) を立てない
//   - `HasModalInstance()` (ウィンドウクローズ抑止) に影響しない
//   - `Agent.dialogs()` / `closeAllDialogs()` に現れない
//
// 共有するのは «プロセス全体のもの» だけ: ThorVG / フォントの初期化、 テーマ、
// 表示言語、 `registerImage` の mem:// store、 そして通知キュー
// (manager の `Dispatch*` 経由。 ダイアログとの通知順序を保つため)。
//
// 駆動は継続イベントフック (`TVPAddContinuousEventHook`)。 window update の
// 外なので、 ここから TJS を走らせても再入の問題が無い。 吉里吉里の
// アニメーションレイヤと同じ «毎フレーム描いて `update()`» の作り。
//
// 内部ヘッダ。 elements を使う翻訳単位からのみ include すること。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_LAYER_PANEL_H
#define ELEMENTS_LAYER_PANEL_H

#include "tjsCommHead.h"
#include "tjsVariant.h"
#include "EventIntf.h"        // tTVPContinuousEventCallbackIntf
#include "tvpinputdefs.h"     // tTVPMouseButton / TVP_SS_*

#include <memory>
#include <string>
#include <vector>

namespace elements_modal { class overlay_session; }
class iTVPDialogEventHandler;
class tTJSNI_BaseLayer;

//---------------------------------------------------------------------------
//! @brief Elements の画面 1 枚をホストのレイヤへ描く。
//!
//! 1 パネル = 1 画面。 画面の切替は `Open` を呼び直す (navigator フローは
//! 持たない。 クロスフェードはレイヤを 2 枚にして `[trans]` に任せる)。
class tTVPElementsLayerPanel : public tTVPContinuousEventCallbackIntf
{
public:
	//! @param layer   描画先。 パネルはこのレイヤの**画像バッファ**へ描く
	//!                (寿命はホストが保証する。 パネルは弱参照)
	//! @param handler 通知先 (OnAction / OnDrag / OnVar / WantsVarNotify)
	tTVPElementsLayerPanel(tTJSNI_BaseLayer* layer,
	                       iTVPDialogEventHandler* handler);
	// 基底 (tTVPContinuousEventCallbackIntf) に仮想デストラクタは無いので
	// override は付けない。 パネルは常に実型で持つ。
	~tTVPElementsLayerPanel();

	tTVPElementsLayerPanel(const tTVPElementsLayerPanel&) = delete;
	tTVPElementsLayerPanel& operator=(const tTVPElementsLayerPanel&) = delete;

	//! @brief 画面 JSON (utf-8) を開く。 既に開いていれば閉じてから開き直す。
	//! @param resource_base_utf8 画面 JSON 内の相対資材パスの解決基準 (空可)
	//! @return 成功したか (JSON が通らない / レイヤが無い / 画像サイズ 0 で false)
	bool Open(const std::string& json_utf8,
	          const std::string& resource_base_utf8);

	//! @brief 閉じる。 開いていなければ何もしない。 `OnClosed` は呼び出し側
	//!        (TJS の口) が発火させる (native 側は通知しない)。
	void Close();

	//! @brief 開いているか。
	bool IsOpen() const { return Session != nullptr; }

	//! @brief レイヤの画像サイズが変わったときに呼ぶ。 バッファを作り直し、
	//!        view にも新しい寸法を伝える。 サイズが同じなら何もしない。
	void NotifyLayerResized();

	// --- 入力 (座標は **レイヤ local**。 変換不要) ---------------------
	// 吉里吉里はレイヤのマウスハンドラへ「レイヤ左上原点」の座標を渡すので、
	// そのまま session へ流せる (render_to_buffer に surface=0,0 を渡して
	// 内部アンカーを無効化しているため view local 座標と一致する)。
	void MouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags);
	void MouseUp  (tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags);
	void MouseMove(tjs_int x, tjs_int y, tjs_uint32 flags);
	void MouseWheel(tjs_int delta, tjs_int x, tjs_int y, tjs_uint32 flags);
	void MouseLeave();

	// キー / パッド / テキストは **呼ばれたときだけ**流す。 パネルは既定では
	// キーボードフォーカスを取らない (複数パネルの調停を持たないため)。
	bool KeyDown(tjs_uint key, tjs_uint32 shift);
	bool KeyUp  (tjs_uint key, tjs_uint32 shift);
	void TextInput(const char* utf8_text);

	// --- 状態 ---------------------------------------------------------
	bool SetVar(const ttstr& name, const ttstr& value);
	bool GetVar(const ttstr& name, ttstr& out) const;

	//! @brief 画面が使っている変数の一覧 (名前 / 現在値 / 参照元)。
	struct VarInfo
	{
		ttstr name;
		ttstr value;
		std::vector<std::pair<ttstr, ttstr>> used_by;   // {要素 id, 参照の種類}
	};
	std::vector<VarInfo> DescribeVars() const;

	//! @brief 変数観測 (`OnVar`) を張り直す (`watchVars` を変えたとき)。
	void RefreshVarWatch();

	//! @brief 表示言語を変える (画面 JSON の `"strings"` を引く言語)。
	void SetLanguage(const std::string& lang);

	//! @brief 明示的な再描画要求 (`registerImage` で mem:// 画像を差し替えた後)。
	void Invalidate();

	bool FocusById(const ttstr& id);
	bool ActivateById(const ttstr& id);

	//! @brief 現在フォーカス中の widget id (無ければ空)。
	ttstr FocusedId() const;

	//! @brief 画面が自分で終了したか (`close_on_click` / Esc 相当)。
	//!        呼び出し側はこれを見て後始末する。
	bool IsFinished() const;

	// --- 駆動 ---------------------------------------------------------
	void TJS_INTF_METHOD OnContinuousCallback(tjs_uint64 tick) override;

	//! @brief 開いている全パネルへ再描画を要求する (mem:// 画像の差替反映)。
	static void InvalidateAll();
	//! @brief 開いている全パネルの表示言語を変える。
	static void SetLanguageAll(const std::string& lang);

private:
	void ReleaseBuffer();
	bool EnsureBuffer();
	//! staging の矩形をレイヤの画像バッファへ転送する。
	void BlitToLayer(int x, int y, int w, int h);

	tTJSNI_BaseLayer*       Layer   = nullptr;   // 弱参照
	iTVPDialogEventHandler* Handler = nullptr;   // 弱参照

	std::unique_ptr<elements_modal::overlay_session> Session;

	// 描画先 staging (ARGB8888 / 連続 pitch)。 レイヤのビットマップは pitch が
	// w*4 と一致しないことがあるので直接は描かない。 加えて
	// `render_to_buffer_partial` は «前回描画がバッファに残っている» ことを
	// 前提にするので、 パネル側で保持する必要がある。
	std::vector<tjs_uint32> Staging;
	int BufW = 0;
	int BufH = 0;

	std::string ResourceBase;
	bool  HookRegistered = false;
	tjs_uint64 LastTick  = 0;

	// session->update() の呼び出しスタック上か。 その最中に届いた通知を
	// 即時配送すると、 コールバックから session を触り直せてしまうので
	// manager のキューへ積ませる (manager の paint_depth と同じ役割)。
	bool InUpdate = false;
};

#endif
