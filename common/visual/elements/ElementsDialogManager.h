//---------------------------------------------------------------------------
//!@file Elements ベース汎用ダイアログ管理 (Phase 3: SDL3 アダプタで MVP)
//
// 設計詳細は doc/ElementsDialog.md 参照。
// Elements (ThorVG ベース) のヘッダは .cpp 側でのみ include。
// 公開ヘッダは TJS / DrawDevice 系の型のみ依存。
//---------------------------------------------------------------------------
#ifndef ELEMENTS_DIALOG_MANAGER_H
#define ELEMENTS_DIALOG_MANAGER_H

#include "tjsCommHead.h"
#include "tjsVariant.h"
#include "DrawDevice.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class iTVPDialogEventHandler;
class iTVPDialogRenderer;
class iTVPDialogRendererHost;

//! @brief overlay 描画パイプラインの区間計測 (TJS: ElementsDialog.renderStats)。
//!        すべて累積値 (renderStatsReset() で 0 クリア)。 時間は microsecond。
//!        呼出側は 2 回読んで差分を取り、 経過実時間に対する割合や
//!        1 フレームあたりの平均を計算する (負荷比較・NX 実測用)。
struct tTVPElementsRenderStats
{
	tjs_uint64 frames = 0;          //!< PaintOverlay 呼出回数 (提示フレーム数)
	tjs_uint64 updates = 0;         //!< session->update() 実行回数 (インスタンス毎)
	tjs_uint64 rasters = 0;         //!< render_to_buffer 実行回数 (= renderCount と同じ契機)
	tjs_uint64 partials = 0;        //!< うち部分再描画 (ダーティ矩形限定) だった回数
	tjs_uint64 cachedPresents = 0;  //!< renderCache による提示のみ (ラスタ省略) の回数
	tjs_uint64 presents = 0;        //!< PresentOverlay 呼出回数
	tjs_uint64 totalUs = 0;         //!< PaintOverlay 全体の所要時間
	tjs_uint64 updateUs = 0;        //!< update() (poll / anim tick / dirty 判定)
	tjs_uint64 rasterUs = 0;        //!< render_to_buffer (ThorVG CPU ラスタ + クリア)
	tjs_uint64 acquireUs = 0;       //!< AcquireBuffer (staging 確保 / テクスチャ lock 待ち)
	tjs_uint64 uploadUs = 0;        //!< ReleaseBuffer (テクスチャ転送)
	tjs_uint64 presentUs = 0;       //!< PresentOverlay (提示)
};

class tTVPElementsDialogManager
{
public:
	static tTVPElementsDialogManager& Instance();

	// Phase 3 MVP: ハードコードされたテストダイアログを表示。
	// Phase 6 で JSON レイアウトと iTVPDialogEventHandler を受ける版に拡張。
	void ShowTestDialog(iTVPDrawDevice* hostDevice);
	// 登録済みのいずれかの DrawDevice を選んで表示 (デバッグ用)
	void ShowTestDialog();

	// === Phase 6a: JSON レイアウト経由で表示 ===
	//! @brief JSON 文字列から構築したダイアログを表示する。
	//! @param hostDevice 描画先 (nullptr なら登録済み中の最初を選ぶ)
	//! @param modal      true: モーダル (下のインスタンス / ゲームへ入力を通さず
	//!                   全入力を独占)。 false: 非モーダル (ヒットしない入力は
	//!                   下 / ゲームへ素通し、 複数同時表示可能)。
	//! @return 起動成功なら true。 複数インスタンス共存可。 同一 handler が既に
	//!         アクティブな場合のみ false。
	//! @param grabFocus 起動時にキーボード/パッドのフォーカスを取得するか。
	//!        true: このダイアログがキー入力を受ける (メニュー等の操作対象)。
	//!        false: キー入力を取らず、 未フォーカスのままゲームへキーを通す
	//!        (常駐 HUD 等)。 modal=true の場合は常に強制取得 (この引数は無視)。
	bool ShowFromJsonString(const std::string& json,
		iTVPDialogEventHandler* handler,
		iTVPDrawDevice* hostDevice = nullptr,
		bool modal = true,
		bool grabFocus = true);

	//! @brief JSON ファイル (resource://./foo.json 等) から構築。
	bool ShowFromJsonFile(const ttstr& path,
		iTVPDialogEventHandler* handler,
		iTVPDrawDevice* hostDevice = nullptr,
		bool modal = true,
		bool grabFocus = true);

	// === navigator フロー: 複数画面の遷移ダイアログ ===
	//! @brief app.jsonc マニフェスト (Storages 経由) から複数画面フローを開始。
	//!        entry 画面を表示し、 各画面 JSON の "transitions" に従って
	//!        push/pop/replace/exit を navigator が解決する。 画面ファイルは
	//!        マニフェストと同じディレクトリ起点で解決し、 各画面の相対資材
	//!        パスはその画面ファイルのディレクトリを resource_base として解決
	//!        する。 フロー終了 (スタック空) で IsModalActive()=false になる。
	//! @return entry 画面の起動に成功したら true
	bool StartFlowFromManifest(const ttstr& manifestPath,
		iTVPDialogEventHandler* handler,
		iTVPDrawDevice* hostDevice = nullptr,
		bool modal = false,
		bool grabFocus = true);

	//! @brief インライン画面マップ (画面名 → JSON utf-8) から複数画面フローを開始。
	//!        ファイル I/O を介さず、 遷移先は各画面 JSON の "transitions" の
	//!        target (画面名) で inline マップを引く。 資材参照は各画面 JSON 内の
	//!        パスをそのまま Storages 解決する (resource_base なし)。
	//! @param screens 画面名 (utf-8) → JSON (utf-8)
	//! @param entry   起点画面名 (utf-8)
	bool StartFlowFromScreens(const std::map<std::string, std::string>& screens,
		const std::string& entry,
		iTVPDialogEventHandler* handler,
		iTVPDrawDevice* hostDevice = nullptr,
		bool modal = false,
		bool grabFocus = true);

	// 最前面 (topmost) のインスタンスを閉じる (デバッグ / QUIT 経路用)。
	void Close();

	//! @brief 指定 handler が所有するインスタンスを閉じる。 複数共存時に
	//!        他人のインスタンスを巻き込まないために、 TJS ElementsDialog の close()
	//!        等はこちらを使う。
	void Close(iTVPDialogEventHandler* handler);

	//! @brief ダイアログを即座に強制 teardown (Window close 時の cleanup 用)。
	//!        通常 view 破棄は次フレーム PaintOverlay で行うが、ウィンドウを
	//!        閉じる経路では PaintOverlay が呼ばれないので同期的に破棄する。
	//!        view callback 内から呼ぶと use-after-free になるので注意。
	void ForceClose();

	//! @brief 何らかのインスタンスがアクティブか。 非モーダルを含めて 1 つでも
	//!        表示中なら true。 「どこかに UI が出ているか」の全体判定用
	//!        (毎フレームの present 要否など)。 入力インターセプトのゲートには
	//!        ウィンドウを跨いで入力を奪ってしまうので使わない
	//!        (→ IsActiveOnDevice)。
	bool IsModalActive() const;

	//! @brief 指定 DrawDevice (= そのウィンドウ) 上にアクティブなインスタンスが
	//!        あるか。 入力インターセプトのゲート用。 overlay を載せていない
	//!        別ウィンドウ (サブウィンドウのダイアログ等) の入力まで奪わない
	//!        ように、 ゲートは device 単位で判定する。
	bool IsActiveOnDevice(iTVPDrawDevice* device) const;

	//! @brief modal=true なインスタンスが 1 つでもアクティブか。 ウィンドウ
	//!        クローズ抑止 (modal 中は閉じさせない) の判定に使う。 非モーダルの
	//!        常駐 UI だけならウィンドウは閉じてよいので false。
	bool HasModalInstance() const;

	// === ホストホットキー (Elements バイパス) ===
	// 登録されたキー / マウスボタン / パッドボタンは、 モーダルが居ない限り
	// Elements ダイアログへ転送せず (Forward* が false を返し) 通常のゲーム入力
	// 経路 (Window.onKeyDown / onMouseDown 等) へ直行する。 配送優先順位:
	//   モーダル (全消費) > ホストホットキー (バイパス) >
	//   フォーカスパネル (handled 素通し) > ゲーム
	// vk はキーの他、 マウスボタン (VK_LBUTTON/VK_RBUTTON/VK_MBUTTON/
	// VK_XBUTTON1/VK_XBUTTON2) と パッド (VK_PAD*) も同じ空間で受ける。
	// mods は TVP_SS_SHIFT|ALT|CTRL の組合せで down は完全一致、 up は vk のみ
	// 一致 (押下中の修飾変化で up がパネルへ漏れない)。
	// duringTextInput=false (既定) はテキスト入力ウィジェット focus 中
	// (focus_consumes_text) は抑止 = 入力欄と衝突するキーを奪わない。
	void RegisterHostHotkey(tjs_uint vk, tjs_uint32 mods, bool duringTextInput);
	void UnregisterHostHotkey(tjs_uint vk, tjs_uint32 mods);
	void ClearHostHotkeys();

	//! @brief 指定 handler が所有するインスタンスが今アクティブか。 TJS ElementsDialog の
	//!        active / close / Invalidate 判定、 ブロッキングモーダルの pump ループ
	//!        終了判定 (自分のインスタンスが閉じたか) に使う。
	bool IsHandlerActive(iTVPDialogEventHandler* handler) const;

	//! @brief 最前面 (topmost) アクティブインスタンスの event handler を返す
	//!        (なければ nullptr)。
	iTVPDialogEventHandler* ActiveHandler() const;

	//! @brief エージェント / デバッグ向けのインスタンス記述 (z-order 順、
	//!        先頭=最背面)。 Agent.dialogs() 等から UI の状態を観測するのに使う。
	struct InstanceInfo
	{
		bool modal = false;
		bool active = false;
		ttstr screen;     //!< フローの現画面名 (単発ダイアログは空)
		ttstr focused;    //!< 現在フォーカス中の widget id (無ければ空)
		int x = 0, y = 0, w = 0, h = 0;   //!< 直近描画矩形 (surface logical)
	};
	std::vector<InstanceInfo> DescribeInstances() const;

	//! @brief index 番目のインスタンスの id 付き widget を列挙 (UI ツリー dump)。
	//!        Agent.dialogTree() から「どの widget が居るか / 現在値」を観測する。
	struct WidgetInfo
	{
		ttstr id;
		ttstr type;        //!< JSON の "type"
		tTJSVariant value; //!< 現在値 (state widget のみ。 無ければ void)
		bool has_value = false;
	};
	std::vector<WidgetInfo> DescribeWidgets(int index) const;

	//! @brief 指定 handler のインスタンスの変数 store へ書き込む。 JSON で
	//!        "text_var": name を指定した label が次フレームで自動更新される
	//!        (elements_modal の VariableStore / overlay_session::set_var)。
	//!        ソフトウェアキーボードの入力文字列表示等、 ホスト状態 → label の
	//!        動的反映に使う。 handler のインスタンスが非アクティブなら false。
	bool SetVar(iTVPDialogEventHandler* handler,
	            const ttstr& name, const ttstr& value);

	//! @brief 指定 handler のインスタンスの変数 store から 1 件読む。
	//!        SetVar で書いた値だけでなく、 hover / focus 連動 (vars_on_hover /
	//!        vars_on_focus)、 スライダの value_var、 ドラッグの drag_at_var など
	//!        画面内で書かれた値も同じ store から読める。
	//!        未知の変数名 / 非アクティブなら false (out は触らない)。
	bool GetVar(iTVPDialogEventHandler* handler,
	            const ttstr& name, ttstr& out) const;

	//! @brief 指定 handler のインスタンスで id の widget へフォーカスを移す
	//!        (FocusWidgetById の handler 版 = ElementsDialog.focus(id))。
	//!        非アクティブ / インスタンス無しなら false。
	bool FocusWidget(iTVPDialogEventHandler* handler, const ttstr& id);

	//! @brief 指定 handler のインスタンスで id の widget を実行する (focus +
	//!        Enter 相当。 ActivateWidgetById の handler 版 =
	//!        ElementsDialog.activate(id))。 非アクティブ / インスタンス無し /
	//!        id 不明なら false。
	bool ActivateWidget(iTVPDialogEventHandler* handler, const ttstr& id);

	//! @brief 画面が使っている変数 1 件の記述 (DescribeVars の要素)。
	struct VarInfo
	{
		ttstr name;    //!< 変数名
		ttstr value;   //!< 現在値 (一度も書かれていなければ空)
		//! この変数を参照している {要素 id, 参照の種類 ("text_var" 等)}。
		//! id は「いちばん近い祖先の id」。 空 = どこからも参照されていない
		//! (ホストが SetVar で作っただけの変数)。
		std::vector<std::pair<ttstr, ttstr>> used_by;
	};

	//! @brief 指定 handler のインスタンスが使っている変数の一覧 (名前順)。
	//!        参照だけあって未書込のもの、 参照は無いがホストが書いたもの、
	//!        どちらも載る。 デバッグパネル / 検証ツール向け。
	std::vector<VarInfo> DescribeVars(iTVPDialogEventHandler* handler) const;

	//! @brief 変数観測の有効/無効を張り直す (ElementsDialog.watchVars の変更時)。
	//!        handler の WantsVarNotify() を問い直し、 現在表示中のインスタンス
	//!        へ即座に反映する。 画面開始時は BeginScreen が自動で行うので、
	//!        表示中に観測対象を変えたときだけ呼べばよい。
	void RefreshVarWatch(iTVPDialogEventHandler* handler);

	//! @brief i18n の表示言語を設定する (画面 JSON の "strings" を引く言語)。
	//!        表示中の全インスタンスへ即時適用し、 以後に開く画面の既定にもなる。
	//!        text_id / text_list_id / options_id を持つ widget が再解決されて
	//!        その場で表示が切り替わる (画面の作り直しは不要)。
	//!        "strings" を持たない画面では何も起きない。
	void SetLanguage(const ttstr& lang);

	//! @brief 現在の表示言語 (SetLanguage で設定した値。 既定は空 = 画面 JSON の
	//!        "lang" 任せ)。
	ttstr GetLanguage() const;

	//! @brief 内蔵仮想キーボードの動作モード。 テキスト欄に focus が入ったとき、
	//!        OS のソフトキーボード (NX swkbd / PS5 IME) の代わりに Elements
	//!        自身の英数キーボードを出すかどうか。
	//!        - "auto"   … 既定。 物理キーボードが無いときだけ出す
	//!        - "always" … 物理キーボードがあっても常に出す (テスト用)
	//!        - "never"  … 出さない (OS 側に任せる)。 表示中なら閉じる
	//!        初期値は環境変数 KRKRZ_FORCE_VIRTUAL_KEYBOARD=1 なら "always"。
	void SetVirtualKeyboardMode(const ttstr& mode);

	//! @brief 現在の仮想キーボード動作モード ("auto" / "always" / "never")。
	ttstr GetVirtualKeyboardMode() const;

	//! @brief 物理 (ハードウェア) キーボードが接続されているか。
	//!        SDL_HasKeyboard() の値。 デスクトップは常に true。
	//!        ゲーム側が独自ソフトキーボードを出すか判断するのに使う。
	bool HasPhysicalKeyboard() const;

	//! @brief index 番目のインスタンスの widget を id 指定でフォーカス + 起動
	//!        (Enter 相当)。 座標を当てずにボタン押下 / トグルできる。
	//!        index が範囲外 / id が見つからなければ false。
	bool ActivateWidgetById(int index, const ttstr& id);
	//! @brief index 番目のインスタンスの widget へフォーカスを移す。
	bool FocusWidgetById(int index, const ttstr& id);

	//! @brief 直近のモーダルダイアログの結果 (action + state widget 値マップ)。
	//!        button に JSON で "close_on_click": true を付けた要素が click
	//!        されたとき、 overlay_session が自動 finish + result を確定する。
	//!        その内容を保存して、 modal 呼出側 (SDLElementsModalRunner overlay
	//!        経路 / ElementsDialog.showModalJson の overlay モード) が後で取得できる。
	//!
	//!        action は閉じた button の id (Esc / × は空)。 values は
	//!        checkbox / toggle / slide_switch / input_box 等の最終値マップ。
	//!
	//!        TakeLastModalResult() は呼出すと内部状態をクリアする (一度きり)。
	bool HasLastModalResult() const;
	//! @brief 指定 handler のインスタンスが finish したときに保存された結果を
	//!        取り出す (一度きり、 取り出すと消える)。 ブロッキングモーダルの
	//!        pump ループが、 自分のインスタンスの結果を回収するのに使う。
	bool TakeLastModalResult(iTVPDialogEventHandler* handler,
	                         ttstr& out_action,
	                         std::map<ttstr, tTJSVariant>& out_values);

	//! @brief handler への OnAction 配送口 (bridge callback が使用)。
	//!        paint (PaintOverlay → session update) の最中に発火した action は
	//!        直接配送せずキューに積み、 continuous イベントフック (window
	//!        update の外) から配送する。 コールバック内で開かれるブロッキング
	//!        モーダルが window update の再入禁止で描画不能になるのを防ぐ。
	//!        paint 外 (入力経路等) から呼ばれた場合は従来どおり即時配送。
	void DispatchAction(iTVPDialogEventHandler* handler,
	                    const ttstr& id, const tTJSVariant& payload);

	//! @brief ドラッグ通知を handler へ配送する (DispatchAction と同じ経路)。
	//!        coalesce=true (move) のとき、 キュー末尾が同じ handler の move なら
	//!        差し替える (paint 中に溜まった移動を最新 1 件へ畳む)。
	void DispatchDrag(iTVPDialogEventHandler* handler,
	                  const tTJSVariant& payload, bool coalesce);

	//! @brief 変数変化通知を handler へ配送する (DispatchAction と同じキュー)。
	//!        変数は «状態» なので必ずキューへ積み (即時配送しない)、 同じ
	//!        handler + 同じ変数名が既に積まれていれば値を差し替える
	//!        (1 フレームぶんの連続変化は最新値だけ配れば足りる)。
	void DispatchVar(iTVPDialogEventHandler* handler,
	                 const ttstr& name, const ttstr& value);

	//! @brief close 予約済み (close_requested) のインスタンスを直ちに破棄する
	//!        (OnClosed 発火まで含む)。 ブロッキングモーダルの pump 脱出直後に
	//!        呼ぶこと。 finish → teardown は通常次フレームの PaintOverlay へ
	//!        遅延されるが、 pump はその前に抜けるため、 呼出側の (スタック上の
	//!        短命な) handler が解放された後に OnClosed が飛んで use-after-free
	//!        になる。 pump 脱出時にここで同期的に破棄して防ぐ。
	//!        update() が呼び出しスタック上にあるインスタンスには触らない。
	void FlushPendingTeardowns();

	// === 描画アダプタ提供口の登録 (DrawDevice が自身を host として登録) ===
	// DrawDevice は iTVPDialogRendererHost を実装し (renderer は DrawDevice が所有)、
	// 自身を host として登録する。 manager は具象 renderer 型を知らず、host 経由で
	// renderer を取得する (overlay 動画の presenter host と同じ設計)。 差し替え /
	// プラグイン DrawDevice も同じ登録で overlay ダイアログ描画に参加できる。
	void RegisterDialogHost(iTVPDrawDevice* device, iTVPDialogRendererHost* host);
	void UnregisterDialogHost(iTVPDrawDevice* device);

	// === DrawDevice からの入力フォワード ===
	// 戻り値 = イベントを Elements 側で消費したか。 false なら呼出側 (DrawDevice /
	// Window) は通常のゲーム入力処理を続行する。 最前面が modal なら全消費、
	// 非モーダルのみならマウスはヒット時のみ消費 (素通し可)、 キー / パッド /
	// テキストは最前面インスタンスへ送って消費する。
	bool ForwardMouseDown(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags);
	bool ForwardMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb, tjs_uint32 flags);
	bool ForwardMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags);
	bool ForwardMouseWheel(tjs_uint32 shift, tjs_int delta, tjs_int x, tjs_int y);
	bool ForwardClick(tjs_int x, tjs_int y);
	bool ForwardDoubleClick(tjs_int x, tjs_int y);
	bool ForwardReleaseCapture();
	bool ForwardMouseOutOfWindow();

	bool ForwardKeyDown(tjs_uint key, tjs_uint32 shift);
	bool ForwardKeyUp(tjs_uint key, tjs_uint32 shift);
	bool ForwardKeyPress(tjs_char key);

	//! @brief SDL_EVENT_TEXT_INPUT 由来のテキスト入力 (UTF-8) を流す。
	//!        krkrz の通常 input event 系列は SDL_EVENT_TEXT_INPUT を扱わない
	//!        ため、 form.cpp の AppEvent から dialog active 時のみ直接ここに
	//!        流す。 IME 経由の入力もここに来る。
	bool ForwardText(const char* utf8_text);

	bool ForwardTouchDown(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id);
	bool ForwardTouchUp(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id);
	bool ForwardTouchMove(tjs_real x, tjs_real y, tjs_real cx, tjs_real cy, tjs_uint32 id);

	//! @brief 入力を転送してきた window を記録する (TVP_DIALOG_INTERCEPT 経由)。
	//!        cursor-warp ナビ ("input":{"cursor_warp":true}) でフォーカス移動先へ
	//!        実マウスカーソルを SetCursorPos するのに使う。 window 側の入力
	//!        ハンドラ (tTJSNI_BaseWindow::On*) が this を渡す。
	void NoteInputWindow(class iTVPWindow* window);

	// === DrawDevice::Show() 終端から呼ばれる ===
	void PaintOverlay(iTVPDrawDevice* device);

	//! @brief Elements ランタイム (ThorVG + フォント) を確実に初期化する。
	//!        独立 window 経路 (SDLElementsModalRunner 等) からも共有できるよう
	//!        公開している。 ShowFrom* 系を呼ぶ前に内部で自動呼出されるため、
	//!        外から呼ぶ必要があるのは「JSON パース前にフォントを揃えたい」
	//!        ケース。 [[feedback_elements_font_init_order]] 参照。
	void EnsureRuntimeInitialized();

	//! @brief ElementsDialog.focusRing がスクリプトから明示設定されたことを記録する。
	//!        EnsureRuntimeInitialized が適用するテーマ既定 (フォーカスリング OFF)
	//!        が、 初回画面表示より前の明示設定を上書きしないようにするための
	//!        フラグ (setter 側から呼ぶ)。
	void NoteFocusRingUserSet() { FocusRingUserSet = true; }

	//! @brief handler (TJS native インスタンス等) の破棄時に呼び、 該当 handler を
	//!        参照する全インスタンスから参照を切って teardown を予約する。
	//!        モーダル終了後の遅延 teardown が解放済み handler へ OnClosed を
	//!        発火する use-after-free の防止。 破棄経路では Close() でなくこちらを
	//!        使う (active でないインスタンスにも効く)。
	void DetachHandler(iTVPDialogEventHandler* handler);

	//! @brief overlay の描画密度モード (TJS: ElementsDialog.renderScale)。
	//!        0 = auto (既定): 最終 present サイズで直接ラスタライズする。
	//!        >0 = authored 論理サイズ×倍率で描き、 present 時に拡縮する
	//!        (1.0 = 原寸レンダ→拡縮表示、 2.0 = 旧 supersampling 相当)。
	//!        次回の RenderInstance から反映される (表示中の画面にも効く)。
	void SetRenderScale(float scale);
	float GetRenderScale() const;

	//! @brief UI の author 基準面サイズ (TJS: ElementsDialog.baseSize)。
	//!        overlay の提示拡縮率 (fit) の分母。 0,0 (既定) はゲームの基準面
	//!        (primary layer サイズ) を使う。 UI をゲーム画面と別の解像度で
	//!        author しているタイトルはこれを設定する (例: 1920,1080)。
	//!        次回の RenderInstance から反映される (表示中の画面にも効く)。
	void SetBaseSize(int w, int h);
	void GetBaseSize(int& w, int& h) const;

	//! @brief overlay の再ラスタライズ抑止 (TJS: ElementsDialog.renderCache)。
	//!        true (既定): 変化が無いフレームは ThorVG の再ラスタライズ +
	//!        テクスチャ再アップロードを省略し、 レンダラが保持する前回の
	//!        描画結果をそのまま提示する (アイドル時 CPU 負荷の削減)。
	//!        false: 従来どおり毎フレーム再描画 (負荷 A/B 比較・切り分け用)。
	void SetRenderCache(bool enable);
	bool GetRenderCache() const;

	//! @brief overlay の部分再描画 (TJS: ElementsDialog.partialRedraw)。
	//!        true (既定): ダーティが矩形で特定できる変化 (テキスト欄キャレット
	//!        点滅等) は、 その矩形だけをクリア + クリップ付き再ラスタライズし、
	//!        テクスチャへも部分転送する。 renderCache 有効時のみ機能する
	//!        (staging に前回フレームが残っていることが前提)。
	//!        false: 変化フレームは常に全面再描画 (A/B 比較・切り分け用)。
	// pad_icon のテーマを接続パッドから自動で決めるか (setPadTheme("auto"))。
	// 有効にすると画面を開くたびに決め直すので、 途中でコントローラを
	// 替えても次に開いた画面から絵が追従する。
	void SetPadThemeAuto(bool enable);
	bool GetPadThemeAuto() const;
	// auto のときだけ、 今つながっているパッドを見てテーマを設定し直す。
	void ResolveAutoPadTheme();

	void SetPartialRedraw(bool enable);
	bool GetPartialRedraw() const;

	//! @brief 実際にラスタライズ (render_to_buffer) した累計回数。 アイドル時に
	//!        増えないことの確認・負荷比較用 (TJS: ElementsDialog.renderCount 読取専用)。
	tjs_uint64 GetRenderCount() const;

	//! @brief overlay 描画パイプラインの区間計測を取得する
	//!        (TJS: ElementsDialog.renderStats 読取専用。 累積値)。
	void GetRenderStats(tTVPElementsRenderStats& out) const;
	//! @brief 計測カウンタを 0 クリアする (TJS: ElementsDialog.renderStatsReset())。
	void ResetRenderStats();

	//! @brief 全インスタンスへ明示的な再描画を要求する。 セッションから観測
	//!        できない外部変化 (registerImage による mem:// 画像バイト差替等)
	//!        の反映に使う。
	void InvalidateOverlays();

	// === 別枠の出力先 (ホストのレイヤへ描くパネル) との共用 ===
	// 通知キュー (pending_actions) は **overlay ダイアログとパネルで 1 本**に
	// する。 別々のキューにすると、 同じフレームに両方から出た通知の相対順序
	// が入れ替わりうるため。 パネルは自身の handler をここへ登録して、
	// 配送前の生存確認を通るようにする。

	//! @brief 「overlay インスタンスを持たないが生きている handler」を登録する。
	//!        配送直前の生存確認 (インスタンスが消えた handler の通知は捨てる)
	//!        で、 パネルの handler が巻き添えにならないようにするため。
	void RegisterExternalHandler(iTVPDialogEventHandler* handler);
	//! @brief 上の登録解除。 キューに残っている通知も捨てる。
	void UnregisterExternalHandler(iTVPDialogEventHandler* handler);

	//! @brief 「いま session を触っている最中なので通知を即時配送しないでほしい」
	//!        区間の開始 / 終了。 overlay の PaintOverlay が内部で使っている
	//!        paint 深度と同じもので、 パネルは自分の update() の周りで挟む。
	//!        入れ子可。
	void PushDeferScope();
	void PopDeferScope();

	//! @brief エンジン終了時の後始末 (ThorVG を畳む)。 表示中のインスタンスを
	//!        全て閉じてから elements_modal::shutdown() を呼ぶ。 呼ばないと
	//!        ThorVG のフォントローダが静的リストに残り、 CRT の atexit で
	//!        «先に死んだフォントマネージャ» を触って間欠クラッシュになる
	//!        (詳細は実装側のコメント / doc/ElementsDialog.md)。
	//!        TVPSystemUninit の AtExit (PRI_SHUTDOWN) から自動で呼ばれるので、
	//!        通常ホストが直接呼ぶ必要は無い。 二度目以降は no-op。
	void ShutdownRuntime();

private:
	tTVPElementsDialogManager();
	~tTVPElementsDialogManager();
	tTVPElementsDialogManager(const tTVPElementsDialogManager&) = delete;
	tTVPElementsDialogManager& operator=(const tTVPElementsDialogManager&) = delete;

	// フロー開始時の host DrawDevice 解決 (未指定なら登録済みの先頭)。
	iTVPDrawDevice* ResolveHostDeviceForFlow(iTVPDrawDevice* requested);

	struct Impl;
	std::unique_ptr<Impl> _impl;

	// ElementsDialog.focusRing の明示設定済みフラグ (NoteFocusRingUserSet 参照)
	bool FocusRingUserSet = false;
};

//! @brief 登録済み DrawDevice のいずれかでテストダイアログを表示する補助関数。
//!        Phase 3 MVP のデバッグ用 (例: F12 キーから呼ぶ)。
void TVPShowElementsTestDialog();

#endif
