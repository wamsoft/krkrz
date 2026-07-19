#ifndef __SDL3Application_h
#define __SDL3Application_h

#include "Application.h"
#include "WindowForm.h"
#include "SDL3KirikiriStorage.h"
#include "SDL3KirikiriIOStream.h"

#include <SDL3/SDL.h>

class SDL3WindowForm : public TTVPWindowForm 
{
	friend class SDL3Application;

protected:
	SDL3WindowForm(class tTJSNI_Window* win);
	~SDL3WindowForm();

	SDL_Window *mWindow;
	bool mVisible;
	bool mEnableTouch;

	bool checkTouchDevice();

public:
	virtual void *NativeWindowHandle() const {
		return mWindow;
	}
	virtual void DestroyNativeWindow();

	virtual void GetSurfaceSize(int &w, int &h) const;
	virtual void ResizeWindow(int w, int h);

	// メインウインドウのキャプション設定
	virtual void SetCaption(const tjs_string& caption);

	virtual void OnCloseCancel();

	// 表示制御
	virtual bool GetVisible() const;
	virtual void SetVisible(bool b);

	virtual void GetCursorPos(tjs_int &x, tjs_int &y);
	virtual void SetCursorPos(tjs_int x, tjs_int y);

	virtual void SetEnableTouch( bool b );
	virtual bool GetEnableTouch() const;

	virtual void SetEnableTouchMouse( bool b );
	virtual bool GetEnableTouchMouse() const;

	//< SDLイベント処理
	bool AppEvent(const SDL_Event& event);
};


class SDL3Application : public tTVPApplication {

public:
    SDL3Application();
	virtual ~SDL3Application();

	virtual void AppInit() {}
	virtual void AppInitDone() {}
	virtual void AppIterate() {}

	//< SDLイベント処理
	virtual SDL_AppResult AppEvent(const SDL_Event& event);

	// アプリイベント送信 (任意スレッド)。SDL_PushEvent で即送信、失敗で false。
	// doc/AppEvent.md 参照。
	virtual bool _SendAppEvent(tjs_int message, tjs_int64 wparam, tjs_int64 lparam) override;

	virtual void AppQuit(){}

	// システムパス初期化
	virtual bool InitPath() = 0;

#ifdef USE_SPLASHWINDOW
	// スプラッシュ画面関連
	void CreateSplashWindow(const char *imagePath);
	void DestroySplashWindow();
#endif

	// 起動スクリプト実行完了通知 (tTVPApplication)。splash が残っていれば
	// 閉じ、wasm ではページ側フック (krkrzOnStartupScriptDone) を呼ぶ
	virtual void OnStartupScriptDone() override;
	// SDL3 Kirikiri Storage関連
	SDL_Storage* GetKirikiriStorage();

	// SDL3 Kirikiri IOStream関連
	SDL_IOStream* CreateIOStreamFromPath(const tjs_string& path, tjs_uint32 flags = 0);
	SDL_IOStream* CreateIOStreamFromBinaryStream(iTJSBinaryStream* stream, bool ownsStream = true);

	// ----------------------------------------------------------------------
	// システム諸元取得
	// ----------------------------------------------------------------------

	virtual const tjs_string& InitDataPath() override; //< セーブデータ用のパスを初期化して返す

	virtual const tjs_string& ExePath() const override { return _ExePath; }//< 実行ファイルのパス
	virtual const tjs_string& AppPath() const override { return _AppPath; } //< 既定のパス
	virtual const tjs_string& ResourcePath() const override {return _ResourcePath; } //< リソースフォルダのパス
	virtual const tjs_string& PluginPath() const override {return _PluginPath; } //< プラグインフォルダのパス
	virtual const tjs_string& ProjectPath() const override { return _ProjectPath; } //< 実行対象データのパス
	virtual const tjs_string& LogPath() const override { return _DataPath; }; //< ログデータのパス

	virtual const std::string& getLanguage() const override { return _language; }; //< 言語名取得
	virtual const std::string& getCountry() const override { return _country; }; //< 国名取得

	virtual const tjs_string& getPlatformName() const override { return _platformName; }
	virtual const tjs_string& getOsName() const override { return _osName; }

	// アプリ処理用の WindowForm 実装を返す
	virtual TTVPWindowForm *CreateWindowForm(class tTJSNI_Window *win) override;

	// スクリーンサイズを返す
	virtual tjs_int ScreenWidth() const override;
	virtual tjs_int ScreenHeight() const override;

	// アクティブかどうか
	virtual bool GetActivating() const override;
	virtual bool GetNotMinimizing() const override;

	// for exception showing
	virtual void MessageDlg(const tjs_string& string, const tjs_string& caption, int type, int button) override;

#ifdef __EMSCRIPTEN__
	// wasm: ネイティブモーダルダイアログ (SDL_ShowMessageBox) はメインスレッドを
	// ブロックして音がぶつれ、TVPTerminateSync でアプリごと固まるため使わない。
	// HTML オーバーレイにエラーを出し、音を止めてアプリは終了させない (継続)。
	// 実際の UI とリロードは JS 側 (web/pre.js の krkrzOnScriptError) が担う。
	virtual bool OnUnhandledScriptException(const tjs_string& message, const tjs_string& trace, int dlgType) override;
#endif

	virtual bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent) override;

	virtual tjs_uint32 GetPadState(int no) override;

	/// @brief  パッド軸アナログ値取得 (-1.0〜+1.0、トリガは 0.0〜+1.0)
	virtual float GetPadAxis(int no, int axisId) override;

	// 解像度情報
	virtual tjs_int GetDensity() const override;

	// アプリ処理用の DrawDevice実装を返す
	virtual tTJSNativeClass* GetDefaultDrawDevice() override;

	virtual void Terminate(int code) override;
	virtual void Exit(int code) override;

	// DLL処理
	virtual void* LoadLibrary( const tjs_char* path ) override;
	virtual void* GetProcAddress( void* handle, const char* func_name) override;
	virtual void  FreeLibrary( void* handle ) override;

	/// 物理メモリ量取得
	virtual tjs_uint64 GetTotalPhysMemory() override;

	// -----------------------------------------------------------------------

	bool IsTerminated() const { return _Terminated; }
	int TerminateCode() const { return _TerminateCode; }

	/// @brief  バックグラウンド状態かどうか
	bool IsInBackground() const { return _InBackground; }
	void SetInBackground(bool b) { _InBackground = b; }

	// ----------------------------------------------------------------------
    // ファイル処理系
	// ※パス系は全て UTF-16 で渡ってくるので注意
	// ----------------------------------------------------------------------

	//< システムフォント一覧取得
	virtual void GetSystemFontList(std::vector<tjs_string>& fontFiles) override; 

	// -----------------------------------------------------------------------

	/// @brief  ジョイパッドの種別
	virtual tjs_string GetJoypadType(int no) override;
	/// @brief  接続されているゲームパッドの数
	virtual tjs_int GetJoypadCount() override;
	/// @brief  指定番号のゲームパッドが有効か
	virtual bool HasJoypad(int no) override;

	/// @brief  振動開始
	virtual bool RumbleGamepad(int no, int low, int high, int duration_ms) override;
	/// @brief  振動停止
	virtual bool StopRumbleGamepad(int no) override;

	/**
	 * アプリ終了通知の開始と終了
	 */
	virtual void OnTerminatingStart() {}
	virtual void OnTerminatingEnd() {}

protected:
	tjs_string _ExePath;    //< 実行ファイルのパス
	tjs_string _PluginPath; //< プラグインフォルダのパス
	tjs_string _ProjectPath;   //< プロジェクトデータのパス
	tjs_string _DataPath; //< セーブデータのパス
	tjs_string _AppPath;    //< 実行ファイルのある場所のパス
	tjs_string _ResourcePath; //< リソースデータのパス
	std::string _language; //< 言語名
	std::string _country; //< 国名

	tjs_string _platformName; //< プラットフォーム名
	tjs_string _osName; //< OS名

	bool _Terminated;
	int _TerminateCode;
	bool _InBackground; //< バックグラウンド状態

#ifdef USE_SPLASHWINDOW
	// スプラッシュ画面用メンバー変数
	SDL_Window* mSplashWindow;
	SDL_Renderer* mSplashRenderer;
	SDL_Texture* mSplashTexture;
#endif

	// SDL3 Kirikiri Storage用メンバー変数
	SDL_Storage* mKirikiriStorage;

	// アプリイベント用に SDL_RegisterEvents で確保した単一のユーザイベント型。
	// 全ての AM_* / TVP_EV_* はこの type の user.code に message を載せて運ぶ。
	Uint32 mAppEventType;

	virtual void OnInitialize(tTJS* scriptEngine);
};

// 生成用
extern SDL3Application *GetSDL3Application();

// for iTVPLocalFileSystem
extern bool SDL_NormalizeStorageName(tjs_string &name);
extern void SDL_GetLocallyAccessibleName(tjs_string &name);

// リスト処理ファイル情報取得が極端に遅いアーキテクチャがあるので分離…
bool SDL_GetListAt(const tjs_char *name, std::function<void(const tjs_char *, bool isDir)> lister, bool withdir);
extern bool SDL_CommitSavedata();
extern bool SDL_RollbackSavedata();

// ファイルオープンのっとり用
extern iTJSBinaryStream* SDL_OpenStream(const char *path, const tjs_uint32 flags);

#endif