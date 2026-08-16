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

//! @brief overlay 描画パイプラインの区間計測 (TJS: Dialog.renderStats)。
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
	//!        他人のインスタンスを巻き込まないために、 TJS Dialog の close()
	//!        等はこちらを使う。
	void Close(iTVPDialogEventHandler* handler);

	//! @brief ダイアログを即座に強制 teardown (Window close 時の cleanup 用)。
	//!        通常 view 破棄は次フレーム PaintOverlay で行うが、ウィンドウを
	//!        閉じる経路では PaintOverlay が呼ばれないので同期的に破棄する。
	//!        view callback 内から呼ぶと use-after-free になるので注意。
	void ForceClose();

	//! @brief 何らかのインスタンスがアクティブか (DrawDevice 入力インターセプトの
	//!        ゲート用)。 非モーダルを含めて 1 つでも表示中なら true。
	bool IsModalActive() const;

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

	//! @brief 指定 handler が所有するインスタンスが今アクティブか。 TJS Dialog の
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
	//!        経路 / Dialog.showModalJson の overlay モード) が後で取得できる。
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

	//! @brief handler (TJS native インスタンス等) の破棄時に呼び、 該当 handler を
	//!        参照する全インスタンスから参照を切って teardown を予約する。
	//!        モーダル終了後の遅延 teardown が解放済み handler へ OnClosed を
	//!        発火する use-after-free の防止。 破棄経路では Close() でなくこちらを
	//!        使う (active でないインスタンスにも効く)。
	void DetachHandler(iTVPDialogEventHandler* handler);

	//! @brief overlay の描画密度モード (TJS: Dialog.renderScale)。
	//!        0 = auto (既定): 最終 present サイズで直接ラスタライズする。
	//!        >0 = authored 論理サイズ×倍率で描き、 present 時に拡縮する
	//!        (1.0 = 原寸レンダ→拡縮表示、 2.0 = 旧 supersampling 相当)。
	//!        次回の RenderInstance から反映される (表示中の画面にも効く)。
	void SetRenderScale(float scale);
	float GetRenderScale() const;

	//! @brief overlay の再ラスタライズ抑止 (TJS: Dialog.renderCache)。
	//!        true (既定): 変化が無いフレームは ThorVG の再ラスタライズ +
	//!        テクスチャ再アップロードを省略し、 レンダラが保持する前回の
	//!        描画結果をそのまま提示する (アイドル時 CPU 負荷の削減)。
	//!        false: 従来どおり毎フレーム再描画 (負荷 A/B 比較・切り分け用)。
	void SetRenderCache(bool enable);
	bool GetRenderCache() const;

	//! @brief overlay の部分再描画 (TJS: Dialog.partialRedraw)。
	//!        true (既定): ダーティが矩形で特定できる変化 (テキスト欄キャレット
	//!        点滅等) は、 その矩形だけをクリア + クリップ付き再ラスタライズし、
	//!        テクスチャへも部分転送する。 renderCache 有効時のみ機能する
	//!        (staging に前回フレームが残っていることが前提)。
	//!        false: 変化フレームは常に全面再描画 (A/B 比較・切り分け用)。
	void SetPartialRedraw(bool enable);
	bool GetPartialRedraw() const;

	//! @brief 実際にラスタライズ (render_to_buffer) した累計回数。 アイドル時に
	//!        増えないことの確認・負荷比較用 (TJS: Dialog.renderCount 読取専用)。
	tjs_uint64 GetRenderCount() const;

	//! @brief overlay 描画パイプラインの区間計測を取得する
	//!        (TJS: Dialog.renderStats 読取専用。 累積値)。
	void GetRenderStats(tTVPElementsRenderStats& out) const;
	//! @brief 計測カウンタを 0 クリアする (TJS: Dialog.renderStatsReset())。
	void ResetRenderStats();

	//! @brief 全インスタンスへ明示的な再描画を要求する。 セッションから観測
	//!        できない外部変化 (registerImage による mem:// 画像バイト差替等)
	//!        の反映に使う。
	void InvalidateOverlays();

private:
	tTVPElementsDialogManager();
	~tTVPElementsDialogManager();
	tTVPElementsDialogManager(const tTVPElementsDialogManager&) = delete;
	tTVPElementsDialogManager& operator=(const tTVPElementsDialogManager&) = delete;

	// フロー開始時の host DrawDevice 解決 (未指定なら登録済みの先頭)。
	iTVPDrawDevice* ResolveHostDeviceForFlow(iTVPDrawDevice* requested);

	struct Impl;
	std::unique_ptr<Impl> _impl;
};

//! @brief 登録済み DrawDevice のいずれかでテストダイアログを表示する補助関数。
//!        Phase 3 MVP のデバッグ用 (例: F12 キーから呼ぶ)。
void TVPShowElementsTestDialog();

#endif
