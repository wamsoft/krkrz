
#ifndef __T_APPLICATION_H__
#define __T_APPLICATION_H__

#include <vector>
#include <map>
#include <stack>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "PadManager.h"
#include "XInputPad.h"

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


tjs_string ExePath();

// 見通しのよい方法に変更した方が良い
extern int _argc;
extern tjs_char ** _wargv;

enum {
	mrOk,
	mrAbort,
	mrCancel,
};

enum {
  mtWarning = MB_ICONWARNING,
  mtError = MB_ICONERROR,
  mtInformation = MB_ICONINFORMATION,
  mtConfirmation = MB_ICONQUESTION,
  mtStop = MB_ICONSTOP,
  mtCustom = 0
};
enum {
	mbOK = MB_OK,
};

class AcceleratorKey {
	HACCEL hAccel_;
	ACCEL* keys_;
	int key_count_;

public:
	AcceleratorKey();
	~AcceleratorKey();
	void AddKey( WORD id, WORD key, BYTE virt );
	void DelKey( WORD id );
	HACCEL GetHandle() { return hAccel_; }
};
class AcceleratorKeyTable {
	std::map<HWND,AcceleratorKey*> keys_;
	HACCEL hAccel_;

public:
	AcceleratorKeyTable();
	~AcceleratorKeyTable();
	void AddKey( HWND hWnd, WORD id, WORD key, BYTE virt );
	void DelKey( HWND hWnd, WORD id );
	void DelTable( HWND hWnd );
	HACCEL GetHandle(HWND hWnd) {
		std::map<HWND,AcceleratorKey*>::iterator i = keys_.find(hWnd);
		if( i != keys_.end() ) {
			return i->second->GetHandle();
		}
		return hAccel_;
	}
};
class tTVPApplication {
	std::vector<class TTVPWindowForm*> windows_list_;
	tjs_string title_;

	bool is_attach_console_;
	tjs_string console_title_;
	AcceleratorKeyTable accel_key_;
	bool tarminate_;
	bool application_activating_;
	bool has_map_report_process_;

	class tTVPAsyncImageLoader* image_load_thread_;
	class tTVPStorageCacheThread* file_cache_thread_;

	std::stack<class tTVPWindow*> modal_window_stack_;
	std::vector<char> console_cache_;

private:
	void CheckConsole();
	void CloseConsole();
	void CheckDigitizer();
	void ShowException( const tjs_char* e );
	void Initialize() {}
	void Run();

public:
	tTVPApplication();
	~tTVPApplication();
	bool StartApplication( int argc, tjs_char* argv[] );

	tjs_string ExePath();
	tjs_string PluginPath();

	bool IsAttachConsole() { return is_attach_console_; }

	bool IsTarminate() const { return tarminate_; }

	HWND GetHandle();
	bool IsIconic() {
		HWND hWnd = GetHandle();
		if( hWnd != INVALID_HANDLE_VALUE ) {
			return 0 != ::IsIconic(hWnd);
		}
		return true; // そもそもウィンドウがない
	}
	void Minimize();
	void Restore();
	void BringToFront();

	void AddWindow( class TTVPWindowForm* win ) {
		windows_list_.push_back( win );
	}
	void RemoveWindow( class TTVPWindowForm* win );
	unsigned int GetWindowCount() const {
		return (unsigned int)windows_list_.size();
	}
	// 先頭ウィンドウ (= メインウィンドウ) の form。generic 版 tTVPApplication と
	// 対称の accessor (Agent 入力注入 seam 等が共通コードから参照する)。
	class TTVPWindowForm* MainWindowForm() const {
		return windows_list_.empty() ? nullptr : windows_list_[0];
	}

	bool ProcessMessage( MSG &msg );
	void ProcessMessages();
	void HandleMessage();
	void HandleIdle(MSG &msg);

	tjs_string GetTitle() const { return title_; }
	void SetTitle( const tjs_string& caption );

	static inline int MessageDlg( const tjs_string& string, const tjs_string& caption, int type, int button ) {
		return ::MessageBox( nullptr, (const wchar_t*)string.c_str(), (const wchar_t*)caption.c_str(), type|button  );
	}

	// 未処理スクリプト例外時の表示と後処理 (WINVER デスクトップ既定: ダイアログ +
	// 終了)。generic 版 (Application.h) と同シグネチャで、TVPShowScriptException
	// が Application-> 経由で呼ぶ。REPL 動作時は共通コード側で別分岐となり本メソッド
	// は呼ばれない。
	bool OnUnhandledScriptException( const tjs_string& message, const tjs_string& trace, int dlgType ) {
		MessageDlg( message, GetTitle(), dlgType, 0 /*MB_OK*/ );
		return true; // 終了する
	}
	void Terminate() {
		::PostQuitMessage(0);
	}
	void SetHintHidePause( int v ) {}
	void SetShowHint( bool b ) {}
	void SetShowMainForm( bool b ) {}


	HWND GetMainWindowHandle() const;

	int ArgC;
	tjs_char ** ArgV;

	void PostMessageToMainWindow(UINT message, WPARAM wParam, LPARAM lParam);


	void ModalStarted( class tTVPWindow* form ) {
		modal_window_stack_.push(form);
	}
	void ModalFinished();
	void OnActiveAnyWindow();
	void DisableWindows();
	void EnableWindows( const std::vector<class TTVPWindowForm*>& win );
	void GetDisableWindowList( std::vector<class TTVPWindowForm*>& win );
	void GetEnableWindowList( std::vector<class TTVPWindowForm*>& win, class TTVPWindowForm* activeWindow );

	
	void RegisterAcceleratorKey(HWND hWnd, char virt, short key, short cmd);
	void UnregisterAcceleratorKey(HWND hWnd, short cmd);
	void DeleteAcceleratorKeyTable( HWND hWnd );

	void OnActivate( HWND hWnd );
	void OnDeactivate( HWND hWnd );
	bool GetActivating() const { return application_activating_; }
	bool GetNotMinimizing() const;

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

	// -------------------------------------------------------------

	void CacheFileRequest( const ttstr &name, bool fast=false, tjs_uint64 minSize=0 );
	void CacheFileClear( const ttstr &name);
	void CacheFileClearOld(int keepTime, bool force);
	void CacheFileSetMaxSize( int maxSize);
	bool CacheIsLoading(bool fast=false) const;

	// -------------------------------------------------------------

	tjs_int GetDensity() const;

	// キー押し下げ状態取得
	bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent);

	// -------------------------------------------------------------
	// ゲームパッド (XInput バックエンド。0=最後に操作したパッドの別名 / 1..N=実パッド)
	// -------------------------------------------------------------

	// パッド軸 ID (doc/Gamepad.md §3。値は SDL_GamepadAxis と同値)
	enum {
		TVP_PAD_AXIS_LEFTX         = 0,
		TVP_PAD_AXIS_LEFTY         = 1,
		TVP_PAD_AXIS_RIGHTX        = 2,
		TVP_PAD_AXIS_RIGHTY        = 3,
		TVP_PAD_AXIS_LEFT_TRIGGER  = 4,
		TVP_PAD_AXIS_RIGHT_TRIGGER = 5,
		TVP_PAD_AXIS_COUNT         = 6,
	};

	tjs_uint32 GetPadState(int no) { return PadManager_.GetPadState(no); }
	float GetPadAxis(int no, int axisId) { return PadManager_.GetPadAxis(no, axisId); }
	tjs_string GetJoypadType(int no) { return PadManager_.GetJoypadType(no); }
	tjs_int GetJoypadCount() { return PadManager_.GetJoypadCount(); }
	bool HasJoypad(int no) { return PadManager_.HasJoypad(no); }
	bool RumbleGamepad(int no, int low, int high, int duration_ms) { return PadManager_.Rumble(no, low, high, duration_ms); }
	bool StopRumbleGamepad(int no) { return PadManager_.StopRumble(no); }

	// パッド機能の有効/無効 (System.padEnabled)。CLI -joypad より優先。
	void SetJoypadEnabled(bool b) { PadManager_.SetEnabled(b); }
	bool GetJoypadEnabled() { return PadManager_.IsEnabled(); }

	// パッド状態を取り込みキーイベント (VK_PAD*) を生成する。毎フレーム呼ぶ。
	// windowActive=false の間は全キー解放扱い。
	void PadPoll(bool windowActive);
	const std::vector<int>& PadUppedKeys()  const { return PadManager_.GetUppedKeys(); }
	const std::vector<int>& PadDownedKeys() const { return PadManager_.GetDownedKeys(); }
	const std::vector<int>& PadRepeatKeys() const { return PadManager_.GetRepeatKeys(); }
	// パッドキーの押下状態 (論理 0 = 最後に操作したパッド基準)
	bool GetPadKeyAsyncState(tjs_uint keycode) { return PadManager_.GetAsyncKeyState(keycode); }

private:
	tTVPPadManager PadManager_;
	tTVPXInputPadProvider PadProvider_;
};
std::vector<std::string>* LoadLinesFromFile( const tjs_string& path );

inline HINSTANCE GetHInstance() { return ((HINSTANCE)GetModuleHandle(0)); }
extern class tTVPApplication* Application;


#endif // __T_APPLICATION_H__
