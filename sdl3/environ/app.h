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
	enum tTVPBorderStyle mBorderStyle;	//< SDL には枠スタイルの概念が無いので値を保持する
	int mMinWidth, mMinHeight, mMaxWidth, mMaxHeight;

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

	// モーダル表示 (Window.showModal)。ネストしたイベントループを回す。
	virtual void ShowWindowAsModal() override;

	// --- ウィンドウの位置 / サイズ / 装飾 -------------------------------
	// generic の既定は「位置もサイズも取れない空実装」なので、ウィンドウ装飾を
	// 持つデスクトップ SDL3 では実際の SDL_Window を操作する。
	virtual void SetLeft(int l);
	virtual int  GetLeft() const;
	virtual void SetTop(int t);
	virtual int  GetTop() const;
	virtual void SetPosition(int l, int t);
	virtual void SetWidth(int w);
	virtual int  GetWidth() const;
	virtual void SetHeight(int h);
	virtual int  GetHeight() const;
	virtual void SetSize(int w, int h);

	virtual void SetMinWidth(int v);
	virtual void SetMinHeight(int v);
	virtual void SetMinSize(int w, int h);
	virtual void SetMaxWidth(int v);
	virtual void SetMaxHeight(int v);
	virtual void SetMaxSize(int w, int h);

	virtual void SetBorderStyle(enum tTVPBorderStyle st);
	virtual enum tTVPBorderStyle GetBorderStyle() const;
	virtual void SetStayOnTop(bool b);
	virtual bool GetStayOnTop() const;
	virtual void BringToFront();
	virtual void SetFullScreenMode(bool b);
	virtual bool GetFullScreenMode() const;

	virtual void GetCursorPos(tjs_int &x, tjs_int &y);
	virtual void SetCursorPos(tjs_int x, tjs_int y);

	// マウスカーソル表示/非表示 (SetMouseCursorState mcsTempHidden/mcsHidden の
	// 実体)。 基底は空実装なので SDL 側で SDL_HideCursor/ShowCursor に接続する。
	virtual void SetCursorVisible(bool visible);

	virtual void SetEnableTouch( bool b );
	virtual bool GetEnableTouch() const;

	virtual void SetEnableTouchMouse( bool b );
	virtual bool GetEnableTouchMouse() const;

	//< SDLイベント処理
	bool AppEvent(const SDL_Event& event);
};


class SDL3Application : public tTVPApplication, public iTVPPhysicalPadProvider {

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

	// 起動スクリプト実行完了通知 (tTVPApplication)。wasm ではページ側フック
	// (krkrzOnStartupScriptDone) を呼ぶ。プラットフォーム側で splash 等を閉じる用途
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

	// Yes/No モーダル確認 (System.confirm)。SDL_ShowMessageBox で同期表示。
	virtual bool ConfirmYesNo(const tjs_string& string, const tjs_string& caption) override;

	// テキスト入力 (System.inputString)。既定は Elements 実装。OS のソフトウェア
	// キーボード等へ差し替えたいプラットフォームはこのメソッドを override する。
	virtual bool InputString(const tjs_string& caption, const tjs_string& prompt,
		const tjs_string& def, tjs_string& result) override;

#ifdef __EMSCRIPTEN__
	// wasm: ネイティブモーダルダイアログ (SDL_ShowMessageBox) はメインスレッドを
	// ブロックして音がぶつれ、TVPTerminateSync でアプリごと固まるため使わない。
	// HTML オーバーレイにエラーを出し、音を止めてアプリは終了させない (継続)。
	// 実際の UI とリロードは JS 側 (web/pre.js の krkrzOnScriptError) が担う。
	virtual bool OnUnhandledScriptException(const tjs_string& message, const tjs_string& trace, int dlgType) override;
#endif

	virtual bool GetAsyncKeyState(tjs_uint keycode, bool getcurrent) override;

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

	//< ファイル選択ダイアログ (Storages.selectFile)。SDL_ShowOpenFileDialog で実装。
	virtual bool SelectFile( class iTJSDispatch2 *params ) override;

	//< フォルダ選択ダイアログ (Storages.selectDirectory)。SDL_ShowOpenFolderDialog で実装。
	virtual bool SelectDirectory( class iTJSDispatch2 *params ) override;

	//< シェル実行 (System.shellExecute)。SDL_OpenURL で URL/ファイルを既定ハンドラで開く。
	virtual bool ShellExecute(const tjs_char *target, const tjs_char *param) override;

	// -----------------------------------------------------------------------

	// --- iTVPPhysicalPadProvider (物理パッドアクセス) ---
	// 物理 index は接続順 (g_open_gamepads の並び)。論理層は tTVPPadManager が担う。
	virtual int GetPhysicalPadCount() override;
	virtual tjs_uint32 GetPhysicalPadState(int phys) override;
	virtual float GetPhysicalPadAxis(int phys, int axisId) override;
	virtual tjs_string GetPhysicalPadName(int phys) override;
	virtual bool RumblePhysical(int phys, int low, int high, int duration_ms) override;
	virtual bool StopRumblePhysical(int phys) override;

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