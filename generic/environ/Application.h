
#ifndef __T_APPLICATION_H__
#define __T_APPLICATION_H__

#include <vector>
#include <map>
#include <stack>
#include <array>
#include <algorithm>
#include <queue>
#include <tuple>
#include <string>
#include <assert.h>
#include <functional>

#include <mutex>
#include <condition_variable>
#include <thread>

#include "tjsCommHead.h"
#include "tjsUtils.h"
#include "tjsNative.h"
#include "RectItf.h"
#include "tjs.h"

#include "tvpinputdefs.h"
#include "WindowFormEvent.h"
#include "PadManager.h"

//---------------------------------------------------------------------------
// memory allocation class
//---------------------------------------------------------------------------
// 用途識別タグ (doc/legacy/MemoryBudgetNegotiation.md §11.1)。
// allocator が単一用途専用でも tag を渡しておけば後で getTagStats で
// 集計できる。プラグインからの拡張要求が出るまでは enum 固定。
enum class TVPAllocTag : uint16_t {
	Unknown        = 0,
	FileCache,        // StorageCache が使う file_malloc 経由
	BitmapBits,       // tTVPBitmapBitsAlloc 経由
	GraphicsLoader,  // 画像デコード作業バッファ (将来)
	Texture,          // OpenGL テクスチャ (将来)
	Sound,            // common/sound/ PCM/リング/DSP/クロスフェード一時 (Phase 1 計装済)
	Movie,            // movie-player 経路 (将来)
	TJS2,             // TJS_malloc 経路 (将来)
	User,             // プラグイン任意用途
	_Count
};

// 容量ネゴシエーション拡張 (capacity / used / available / setPressureCallback)
// は doc/legacy/MemoryBudgetNegotiation.md §3.1 (case A) を参照。既存実装は
// デフォルト (容量不明 = SIZE_MAX) のまま挙動不変。
// テレメトリ (Stats / getStats / TagStats / getTagStats) は §11 を参照。
// デフォルトは未対応値を返す。
class iTVPMemoryAllocator {
public:
	using PressureCallback = std::function<void(float)>;

	struct Stats {
		size_t   current_used     = SIZE_MAX; // 現在使用 (= used())。未対応なら SIZE_MAX
		size_t   peak_used        = SIZE_MAX; // ピーク。未対応なら SIZE_MAX
		uint64_t total_allocated  = 0;        // 累積 alloc バイト数
		uint64_t total_freed      = 0;        // 累積 free バイト数
		uint64_t alloc_count      = 0;
		uint64_t free_count       = 0;
		// L2 サイズビン: <128, <1K, <16K, <256K, <4M, <64M, <1G, ≥1G
		std::array<uint64_t, 8> alloc_size_hist = {};
	};

	// L3 tag 別集計 (doc §11.1)。total_freed は T4 で追加 (sanity check 用)。
	struct TagStats {
		size_t   current_used    = 0;
		uint64_t alloc_count     = 0;
		uint64_t free_count      = 0;
		uint64_t total_allocated = 0;
		uint64_t total_freed     = 0;
	};

	virtual ~iTVPMemoryAllocator() {};
	virtual void* allocate( size_t size ) = 0;
	virtual void free( void* mem ) = 0;

	// タグ付き allocate。デフォルトは tag を捨てて allocate(size) を呼ぶ。
	virtual void* allocate( size_t size, TVPAllocTag /*tag*/ ) { return allocate(size); }

	// size 付き free。free 時に呼び出し側で確保サイズが分かる経路 (例:
	// tTVPBitmapBitsAlloc が record->size を持っている) で使うと、
	// allocator 側に header を置かなくても Sized mode (current_used 集計) を
	// 維持できる。デフォルトは size を捨てて free(mem) を呼ぶ。
	virtual void free( void* mem, size_t /*size*/ ) { free(mem); }

	// realloc 相当。old==nullptr なら allocate と等価、new_size==0 なら free と等価。
	// 既存内容をどれだけコピーするかは getAllocatedSize() の戻り値で決まる
	// (== 0 を返す実装ではコピーされず内容が破棄される — その実装は本メソッドを
	// 必ず override すること)。libogg の _ogg_realloc 等のフック先として使う。
	virtual void* reallocate( void* old, size_t new_size, TVPAllocTag tag );

	// 確保済みポインタの実サイズを返す。0 = 不明 (実装非対応)。
	// デフォルト reallocate の memcpy 量決定に使う。
	virtual size_t getAllocatedSize( void* /*mem*/ ) const { return 0; }

	virtual size_t capacity() const { return SIZE_MAX; }
	virtual size_t used()     const { return SIZE_MAX; }
	virtual size_t available() const {
		size_t c = capacity(), u = used();
		if (c == SIZE_MAX || u == SIZE_MAX) return SIZE_MAX;
		return (c > u) ? (c - u) : 0;
	}
	virtual void setPressureCallback(PressureCallback /*cb*/) {}
	virtual Stats getStats() const { return Stats{}; }
	virtual TagStats getTagStats(TVPAllocTag /*tag*/) const { return TagStats{}; }

	// peak_used を current_used に揃え直す (Sized mode allocator のみ意味あり)。
	// 「ここから先の peak をもう一度測りたい」用途。デフォルトは no-op。
	virtual void resetPeak() {}

	// fallback 経由の統計 (TVPPooledAllocator など pool ベース実装向け)。
	// デフォルトは 0 (fallback なし)。
	virtual uint64_t fallbackAllocCount() const { return 0; }
	virtual uint64_t fallbackBytesInUse() const { return 0; }
};


// 共通MoviePlayer 実装
// Bitmap 描画機能あり
class tTJSNI_VideoOverlay;


class TTVPWindowForm;


// アプリイベント(スレッド間 message 配送)を受信するクラスのインターフェース。
// 実装クラスは ctor で Application->addEventHandler、dtor で removeEventHandler。
// 詳細は doc/AppEvent.md 参照。
class AppEventInterface {
public:
	virtual ~AppEventInterface() {}
	// 自分宛のイベントなら処理して true を返す。判定は message だけで行う。
	virtual bool Dispatch( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) = 0;
};


/**
 * 特殊アプリ構成
 * Windowは一つ
 * 描画内容は DrawDevice を経由せず直接通知がくるのでそれを描画する
 * 
 * WindowForm.h をあわせて参照のこと
 * 
 * パス関係は UTF-16 (tjs_char*) 想定
 * 
 */
class tTVPApplication {

public:
	tTVPApplication();
	virtual ~tTVPApplication();

	TTVPWindowForm *MainWindowForm() const { return windows_.size() > 0 ? windows_[0] : nullptr; }

public:
	// -------------------------------------------------------------------
	// アプリシステム側からの制御
	// -------------------------------------------------------------------

	// 引数設定用
	void InitArgs(int argc, char **argv);
	void InitArgs(int argc, tjs_char **argv);

	// アプリ初期化
	bool InitializeApplication();

	// スクリプト処理開始指示
	void Startup();

	// 画面サイズ変更通知
	void ResizeScreen();

	// 全ウインドウの再描画要請
	void RequestUpdate();

	// 処理実行
	void Dispatch();

	// ジョイパッドイベント処理
	// GetPadState() の結果を処理する
	void SendPadEvent();

protected:
	void OnInitialize(tTJS *tjs){}

	// ゲームパッド論理管理 (0=最後に操作したパッドの別名 / 1..N=実パッド)。
	// 物理バックエンドは各プラットフォームの App が iTVPPhysicalPadProvider として
	// 供給し、コンストラクタ等で PadManager_.SetProvider() する。
	tTVPPadManager PadManager_;

public:
	// -------------------------------------------------------------------
	// スクリプトからの呼び出し
	// -------------------------------------------------------------------

	// 環境別実装。SDL は即 SDL_Event に詰めて送信し、失敗したら false を返す。
	virtual bool _SendAppEvent( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) = 0;

	// 任意スレッドから呼べる。_SendAppEvent し、失敗時だけ retry_que_ に積む(要ロック)。
	void SendAppEvent( tjs_int message, tjs_int64 wparam, tjs_int64 lparam );

	// メインスレッドでの呼び返し。app 独自イベントを処理し、未消費なら
	// addEventHandler 済みの全ハンドラへ配る。いずれかが処理したら true。
	bool DispatchAppEvent( tjs_int message, tjs_int64 wparam, tjs_int64 lparam );

	// 起動スクリプト (AM_STARTUP_SCRIPT → TVPInitializeStartupScript) の実行が
	// 完了した直後に呼ばれる。ホスト側のローディング/スプラッシュ表示を
	// 「初回スクリプトロード完了」まで維持して閉じる用途 (wasm は HTML
	// オーバーレイ、nx 等は splash)。既定は何もしない
	virtual void OnStartupScriptDone() {}

	void addEventHandler( AppEventInterface* handler );
	void removeEventHandler( AppEventInterface* handler );

	/**
	 * タイトルの設定
	 */
	tjs_string GetTitle() const { return title_; }

	void SetTitle( const tjs_string& caption ) { 
		title_ = caption;
	}

	// -------------------------------------------------------------

	// Bitmap用のAllocatorを返す
	virtual iTVPMemoryAllocator *CreateBitmapAllocator();

	// File読み込みバッファ用のAllocatorを返す
	virtual iTVPMemoryAllocator *CreateFileAllocator();

	// サウンド用バッファのAllocatorを返す
	virtual iTVPMemoryAllocator *CreateSoundAllocator();

	// GlobalAllocStats (operator new / TJS_malloc) 用のAllocatorを返す
	// KRKRZ_ENABLE_ALLOC_STATS=ON 時に GlobalAllocStats::Initialize() から呼ばれる
	virtual iTVPMemoryAllocator *CreateKrkrzAllocator();

	// システムアロケータ情報を返す (組込みプラットフォーム固有実装用)
	// デフォルトは一般 OS 向けの実装を返す。
	// doc/MemoryDesign.md 参照。
	virtual class iTVPSystemAllocatorInfo *GetSystemAllocatorInfo();

	/**
	 * 画像の非同期読込み要求
	 */
	void LoadImageRequest( class iTJSDispatch2 *owner, class tTJSNI_Bitmap* bmp, const ttstr &name );

	/**
	 * 画像 decode prefetch 要求 (owner なし、cache 登録のみ)
	 */
	void LoadImagePrefetchRequest( const ttstr &name );

	/**
	 * 内部用: AsyncImageLoader インスタンス取得 (NULL あり)
	 */
	class tTVPAsyncImageLoader* GetImageLoadThread() { return image_load_thread_; }

	void CacheFileRequest( const ttstr &name, bool fast=false, tjs_uint64 minSize=0 );
	void CacheFileClear( const ttstr &name );
	void CacheFileClearOld(int keepTime, bool force);
	void CacheFileSetMaxSize( int maxSize);
	bool CacheIsLoading(bool fast=false) const;

	// -------------------------------------------------------------

	void AddWindow( TTVPWindowForm* window );
	void DelWindow( TTVPWindowForm* window );

	void SendMessage( void *window, tjs_int message, tjs_int64 wparam, tjs_int64 lparam );
	void SendTouchMessage( void *window, tjs_int type, float x, float y, float c, int id, tjs_uint64 tick );
	void SendMouseMessage( void *window, tjs_int type, int button, int shift, int x, int y);

	// ----------------------------------------------------------------------------
	// 環境依存機能群
	// ----------------------------------------------------------------------------

	/**
	 * メッセージポンプを回す
	 * 全メッセージを処理
	 */
	virtual void ProcessMessages() {};

	/**
	 * メッセージ処理を一回回す
	 */
	virtual void HandleMessage() {};

	// ----------------------------------------------------------------------
	// システム諸元取得
	// ----------------------------------------------------------------------

	virtual const tjs_string& InitDataPath() = 0; //< データ書き出し先のパスを初期化して返す

	virtual const tjs_string& ExePath() const = 0; //< 実行ファイルのパス
	virtual const tjs_string& AppPath() const = 0; //< 実行ファイルのある場所のパス
	virtual const tjs_string& ResourcePath() const = 0; //< リソースフォルダのパス
	virtual const tjs_string& PluginPath() const = 0; //< プラグインフォルダのパス
	virtual const tjs_string& TempPath() const = 0; //< テンポラリ領域のパス
	virtual const tjs_string& ProjectPath() const = 0; //< プロジェクトデータのパス
	virtual const tjs_string& LogPath() const = 0; //< ログデータのパス

	virtual const std::string& getLanguage() const = 0; //< 言語名取得
	virtual const std::string& getCountry() const = 0; //< 国名取得

	virtual const tjs_string& getPlatformName() const = 0;
	virtual const tjs_string& getOsName() const = 0;

	// 全体引数
	tjs_int GetArgumentCount() const { return _args.size(); }
	const tjs_char* GetArgument(tjs_uint no) { return no < _args.size() ? _args[no].c_str() : TJS_W(""); }

	// オプション以外の引数
	tjs_int GetNormalArgumentCount() const { return _nargs.size(); }
	const tjs_char* GetNormalArgument(tjs_uint no) { return no < _nargs.size() ? _nargs[no].c_str() : TJS_W(""); }

	virtual tjs_int DrawThreadNum() { return 0; }

	// ----------------------------------------------------------------------

	// スクリーンサイズを返す
	virtual tjs_int ScreenWidth() const = 0;
	virtual tjs_int ScreenHeight() const = 0;

	// デスクトップ (作業領域) の矩形を返す (TJS: System.desktop*)。
	// win32 版 krkrz の System.desktopLeft/Top/Width/Height 互換で、
	// ウィンドウ位置の画面内クランプ等に使う。 既定はスクリーン全体
	// (原点 0,0)。 タスクバー等を除いた作業領域を返せるホストは override。
	virtual tjs_int DesktopLeft() const { return 0; }
	virtual tjs_int DesktopTop() const { return 0; }
	virtual tjs_int DesktopWidth() const { return ScreenWidth(); }
	virtual tjs_int DesktopHeight() const { return ScreenHeight(); }

	// アクティブかどうか
	virtual bool GetActivating() const = 0;
	virtual bool GetNotMinimizing() const = 0;

	// アプリ処理用の標準の DrawDevice実装を返す
	virtual tTJSNativeClass* GetDefaultDrawDevice() = 0;

	// アプリ処理用の WindowFOrm 実装を返す
	virtual TTVPWindowForm *CreateWindowForm(class tTJSNI_Window *win) = 0;


	// for exception showing
	virtual void MessageDlg( const tjs_string& string, const tjs_string& caption, int type, int button ) = 0;

	// Yes/No モーダル確認 (System.confirm)。Yes なら true。既定は MessageDlg を
	// 出して true を返すだけのフォールバック。SDL3Application 等が override する。
	virtual bool ConfirmYesNo( const tjs_string& string, const tjs_string& caption ) {
		MessageDlg( string, caption, 0, 0 );
		return true;
	}

	// テキスト入力モーダル (System.inputString)。OK なら true (result に入力文字列)、
	// キャンセル/未対応なら false。既定は未対応 (false=キャンセル扱い)。
	// WINVER は win32/base の TVPInputString を直接使う。SDL は将来 Elements で実装。
	virtual bool InputString( const tjs_string& caption, const tjs_string& prompt,
		const tjs_string& def, tjs_string& result ) { return false; }

	// 未処理スクリプト例外の表示と後処理を行う (プラットフォーム別ポリシー)。
	//   message : 表示用エラー本文 / trace : スタックトレース / dlgType : ダイアログ種別
	//   戻り値  : true  = 呼び出し側 (TVPShowScriptException) が既定の後処理
	//                     (exceptionexe 等 + TVPTerminateSync) を続行する
	//             false = このメソッドが表示・後処理を担い、アプリは終了しない
	// 既定はデスクトップ挙動: MessageDlg を出して終了 (true) を返す。
	// SDL3Application (wasm) や コンソール版はこれを override して独自挙動にする。
	virtual bool OnUnhandledScriptException( const tjs_string& message, const tjs_string& trace, int dlgType ) {
		MessageDlg( message, GetTitle(), dlgType, 0 /*mbOK*/ );
		return true;
	}

	// 終了開始
	virtual void Terminate(int code=0) = 0; //< 終了要求
	virtual void Exit(int code) = 0; //< 強制終了処理（そのままシステム終了）

	// DLL処理
	virtual void* LoadLibrary( const tjs_char* path ) = 0;
	virtual void* GetProcAddress( void* handle, const char* func_name) = 0;
	virtual void  FreeLibrary( void* handle ) = 0;

	/// 物理メモリ量取得
	virtual tjs_uint64 GetTotalPhysMemory() = 0;

	// シェル実行
	virtual bool ShellExecute(const tjs_char *target, const tjs_char *param) {
		return false; // デフォルトは実装しない
	};

	// ファイル選択ダイアログ (標準 Storages.selectFile 用)。params は吉里吉里互換の
	// 辞書 (filter / title / name / initialDir / save / filterIndex)。成功で true を返し、
	// params.name に選択パス (正規化ストレージ名) を書き戻す。既定は非対応 (false)。
	// SDL3 版は SDL_ShowOpenFileDialog で実装。WINVER は win32/base の TVPSelectFile
	// (GetOpenFileName) を直接使うためこの経路は通らない。
	virtual bool SelectFile( class iTJSDispatch2 *params ) { return false; }

	//< フォルダ選択ダイアログ (Storages.selectDirectory)。SDL3 版は
	//< SDL_ShowOpenFolderDialog で実装。WINVER は win32/base の TVPSelectDirectory
	//< (IFileOpenDialog) を直接使うためこの経路は通らない。
	virtual bool SelectDirectory( class iTJSDispatch2 *params ) { return false; }

	// アプリロック取得
	virtual bool CreateAppLock(const ttstr &lockname) { 
		return true; // デフォルトはロックしない
	}

	// 乱数初期化用
	virtual void InitRandomGenerator();

	// ----------------------------------------------------------------------
    // ファイル処理系
	// ----------------------------------------------------------------------

	//< システムフォント一覧取得
	virtual void GetSystemFontList(std::vector<tjs_string>& fontFiles) {}; 

public:
	// 解像度情報
	virtual tjs_int GetDensity() const;

	// キー押し下げ状態取得
	virtual bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent);

	virtual tjs_uint32 GetPadState(int no) { return PadManager_.GetPadState(no); }

	// パッド軸 ID (doc/Gamepad.md §3 参照)。値は SDL3 の SDL_GamepadAxis と同値で、
	// SDL3 実装では axisId をそのまま SDL に渡せる。新規 ID は末尾に追加すること。
	enum {
		TVP_PAD_AXIS_LEFTX         = 0, //< 左スティック X (-1 = 左,  +1 = 右)
		TVP_PAD_AXIS_LEFTY         = 1, //< 左スティック Y (-1 = 上,  +1 = 下)
		TVP_PAD_AXIS_RIGHTX        = 2, //< 右スティック X
		TVP_PAD_AXIS_RIGHTY        = 3, //< 右スティック Y
		TVP_PAD_AXIS_LEFT_TRIGGER  = 4, //< L2 アナログ ( 0 〜 +1)
		TVP_PAD_AXIS_RIGHT_TRIGGER = 5, //< R2 アナログ ( 0 〜 +1)
		TVP_PAD_AXIS_COUNT         = 6,
	};

	// 指定パッドの指定軸の現在値を返す。
	// スティック軸: -1.0f 〜 +1.0f (中立 0.0f)
	// トリガ軸    :  0.0f 〜 +1.0f (未押下 0.0f)
	// 未接続パッド・無効 axisId は 0.0f を返す。デッドゾーンは適用しない (呼び元責務)。
	virtual float GetPadAxis(int no, int axisId) { return PadManager_.GetPadAxis(no, axisId); }

	// SystemControl から移管
	// イベント処理からのコールバック
	void BeginContinuousEvent();
	void EndContinuousEvent();

	virtual tjs_string GetJoypadType(int no) { return PadManager_.GetJoypadType(no); } //< joypadの種別（環境依存値）
	//< joypad のボタン表記の系統 ("xbox" / "ps" / "switch"。 判らなければ空)
	virtual tjs_string GetJoypadStyle(int no) { return PadManager_.GetJoypadStyle(no); }
	virtual tjs_int GetJoypadCount() { return PadManager_.GetJoypadCount(); } //< 接続されているjoypadの数
	virtual bool HasJoypad(int no) { return PadManager_.HasJoypad(no); } //< 指定番号のjoypadが有効か

	// 振動機能
	virtual bool RumbleGamepad(int no, int low, int high, int duration_ms) { return PadManager_.Rumble(no, low, high, duration_ms); }
	virtual bool StopRumbleGamepad(int no) { return PadManager_.StopRumble(no); }

	// パッド機能の有効/無効 (System.padEnabled)。CLI -joypad より優先。
	void SetJoypadEnabled(bool b) { PadManager_.SetEnabled(b); }
	bool GetJoypadEnabled() { return PadManager_.IsEnabled(); }

	// VK_PAD1..4 (A/B/X/Y) をボタンの刻印で割り当てるか (System.padButtonMapping)。
	// true = 刻印基準 (既定。任天堂系は SOUTH=B / EAST=A なので位置とは異なる)
	// false = 位置基準 (SDL の SOUTH/EAST/WEST/NORTH をそのまま A/B/X/Y と読む)
	// パッドを持たないバックエンドでは常に位置基準扱い。
	virtual void SetPadButtonMappingByLabel(bool /*by_label*/) {}
	virtual bool GetPadButtonMappingByLabel() { return false; }

	// プラットフォームタグ (System.platformTag / config_<tag>.cf に使う)。
	// getPlatformName() が SDL_GetPlatform() の生文字列 ("Nintendo Switch" /
	// "PlayStation 5" 等。 空白入り・表記ゆれあり) なのに対し、 こちらは
	// 小文字・空白無しに正規化した短いタグ ("switch" / "ps5" / "windows" ...)。
	// 一般的なものから具体的なものへ並べる (Switch2 = {"switch", "switch2"})。
	// コンパイル時に決まる値なので、 初期化のごく初期 (config.cf 読み込み時)
	// から参照できる。
	virtual const std::vector<tjs_string>& GetPlatformTags() const {
		static const std::vector<tjs_string> empty;
		return empty;
	}

	// 本体 (OS / ハード) の表示言語設定 (System.systemLanguage)。
	// 戻り値は BCP-47 の言語タグ ("ja" / "en-US" / "zh-Hant" / "zh-Hans" ...)。
	// 取得できない環境は空文字を返す (呼び出し側でゲーム既定へフォールバック)。
	//
	// ★ 旧 getLangName プラグインの置き換え。 プラグイン版は Win が英語の
	//    言語名 ("Japanese")、 NX がコード ("ja"/"cn"/"tw") と戻り値が
	//    不揃いで、 PS5 は実装が無く "ja" 固定だった。 こちらは全機種で
	//    BCP-47 に統一する。
	virtual std::string GetSystemLanguage() const { return std::string(); }

	// ----------------------------------------------------------------------
    // 動画関係処理
	// ----------------------------------------------------------------------

protected:
	void UpdateVideoOverlay();

	// タイトル
	tjs_string title_;

	// 引数記録用
	std::vector<tjs_string> _args;
	std::vector<tjs_string> _nargs;

	void ShowException( const tjs_char* e );

	std::vector<TTVPWindowForm *> windows_;

private:

	std::vector<AppEventInterface*>	event_handlers_;
	// _SendAppEvent 失敗時のリトライ用。{ message, wparam, lparam }。
	std::queue<std::tuple<tjs_int,tjs_int64,tjs_int64>>	retry_que_;

	class tTVPAsyncImageLoader* image_load_thread_;
	class tTVPStorageCacheThread* file_cache_thread_;

	std::mutex event_handlers_mutex_;
	std::mutex retry_que_mutex_;

	void DeliverEvents();

	bool ContinuousEventCalling;

};

extern class tTVPApplication* Application;

// プール初期確保量 (バイト)。0 = pool 無効 (raw malloc フォールバック)。
// CLI: -bitmappoolsize=N (MB)、none/off/0 で無効化。
size_t TVPGetBitmapAllocatorPoolSize();
// CLI: -filepoolsize=N (MB)、none/off/0 で無効化。 (FileAllocator.cpp 定義)
size_t TVPGetFileAllocatorPoolSize();

#endif // __T_APPLICATION_H__
