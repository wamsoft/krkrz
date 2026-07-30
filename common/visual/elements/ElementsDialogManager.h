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

	// === 描画アダプタ登録 (DrawDevice 生成時に渡す) ===
	void RegisterRenderer(iTVPDrawDevice* device, std::unique_ptr<iTVPDialogRenderer> renderer);
	void UnregisterRenderer(iTVPDrawDevice* device);

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

	// === DrawDevice::Show() 終端から呼ばれる ===
	void PaintOverlay(iTVPDrawDevice* device);

	//! @brief Elements ランタイム (ThorVG + フォント) を確実に初期化する。
	//!        独立 window 経路 (SDLElementsModalRunner 等) からも共有できるよう
	//!        公開している。 ShowFrom* 系を呼ぶ前に内部で自動呼出されるため、
	//!        外から呼ぶ必要があるのは「JSON パース前にフォントを揃えたい」
	//!        ケース。 [[feedback_elements_font_init_order]] 参照。
	void EnsureRuntimeInitialized();

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
