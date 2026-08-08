/**
 * メモ
 */
#include "tjsCommHead.h"

#include "tjsError.h"
#include "tjsDebug.h"

#include <errno.h>
#include <string>
#include <stdio.h>
#include <climits>
#include "Application.h"
#include "MemoryAllocatorStats.h"
#include "PooledAllocator.h"
#include "SystemAllocatorInfo.h"
#include "ThreadIntf.h"  // KRKRZ_RENDER_STATS_SCOPE / TVPRenderStatsAddFrameDispatch

#include "ScriptMgnIntf.h"
#include "SystemIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "TickCount.h"
#include "CharacterSet.h"
#include "WindowForm.h"
#include "SysInitImpl.h"
#include "MsgImpl.h"
#include "FontSystem.h"
#include "GraphicsLoadThread.h"
#include "MsgLoad.h"
#include "Random.h"
#include "Exception.h"
#include "StorageImpl.h"
#include "StorageCache.h"
#include "StorageIntf.h"
#include "ScriptMgnIntf.h"

#ifdef KRKRZ_HAS_ELEMENTS
#include "elements/ElementsUserConfig.h"
#endif

#include <picojson/picojson.h>
#include <algorithm>

tTVPApplication* Application;

/**
 * コンストラクタ
 */
tTVPApplication::tTVPApplication()
: image_load_thread_(nullptr)
, file_cache_thread_(NULL)
, ContinuousEventCalling(false)
{
	Application = this;
}

/**
 * デストラクタ
 */
tTVPApplication::~tTVPApplication() {
	if(image_load_thread_) {
		try {
			// image_load_thread_->ExitRequest();
			delete image_load_thread_;
			image_load_thread_ = nullptr;
		} catch(...) {
			// ignore errors
		}
	}
	if(file_cache_thread_) {
		try {
			// image_load_thread_->ExitRequest();
			delete file_cache_thread_;
			file_cache_thread_ = nullptr;
		} catch(...) {
			// ignore errors
		}
	}
}

// -------------------------------------------------------------------
// アプリシステム側からの制御
// -------------------------------------------------------------------

void tTVPApplication::ShowException( const tjs_char* e )
{
	ttstr error_message(e);
	TVPLOG_CRITICAL("{}", error_message);
	// REPL 駆動中はネイティブのブロッキング message box を出さない。 内容は
	// 上の TVPLOG_CRITICAL で REPL コンソールに出ているので、 アプリを止めずに
	// 進められる。
	if (TVPReplActive) return;
	tjs_string caption = (const tjs_char*)TVPFatalError;
	MessageDlg( e, caption, 0, 0 );
}

// 引数処理・UTF-8用
void 
tTVPApplication::InitArgs(int argc, char **argv)
{
	int args_done = false;
	for (int i=0;i<argc;i++) {
		std::string arg = argv[i];
		tjs_string warg;
		TVPUtf8ToUtf16( warg, arg);
		_args.push_back(warg);

		// オプション以外の引数を　_nargs に記録する
		if (!args_done) {
			if (warg[0] == TJS_W('-')) {
				// -- があるとそこで本体用の引数は終了
				if (warg[1] == TJS_W('-') && warg[2] == TJS_W('\0')) {
					args_done = true;
				}
			} else {
				_nargs.push_back(warg);
			}
		}
	}
}

// 引数処理・UTF-16用
void
tTVPApplication::InitArgs(int argc, tjs_char **argv)
{
	int args_done = false;
	for (int i=0;i<argc;i++) {
		tjs_string warg = argv[i];
		_args.push_back(warg);

		// オプション以外の引数を　_nargs に記録する
		if (!args_done) {
			if (warg[0] == TJS_W('-')) {
				// -- があるとそこで本体用の引数は終了
				if (warg[1] == TJS_W('-') && warg[2] == TJS_W('\0')) {
					args_done = true;
				}
			} else {
				_nargs.push_back(warg);
			}
		}
	}
}

extern void TVPLoadStaticPluigins(void);

bool tTVPApplication::InitializeApplication() 
{
	try {
		try {
			tjs_string path = Application->ResourcePath() + TJS_W("messages.json");
			tjs_uint64 flen;
			auto buf = TVPReadStream(path.c_str(), &flen);
			if (buf.get() == nullptr ) {
				TVPAddImportantLog( TJS_W("failed to load message file") );
				return false;
			}
			picojson::value v;
			std::string errorstr;
			picojson::parse( v, (const char*)buf.get(), (const char*)buf.get()+flen, &errorstr );
			if (errorstr.empty() != true ) {
				tjs_string errmessage;
				if (TVPUtf8ToUtf16( errmessage, errorstr ) ) {
					TVPAddImportantLog( errmessage.c_str() );
				}
				return false;
			}
			TVPLoadMessage(v.get<picojson::array>());
		} catch( const std::exception &e ) {
			std::string path8;
			TVPUtf16ToUtf8( path8, Application->ResourcePath() + TJS_W("messages.json") );
			TVPLOG_ERROR("failed to load message file {}: {}", path8, e.what());
			return false;
		}

		// 初期カレントディレクトリ設定
		TVPSetCurrentDirectory( AppPath() );

		// ログへOS名等出力
		TVPAddImportantLog( TVPFormatMessage(TVPProgramStartedOn, TVPGetOSName(), TVPGetPlatformName()) );

		// アーカイブデリミタ、msgmap.tjsの実行 と言った初期化処理
		TVPInitializeBaseSystems();

		// -userconf 付きで起動されたかどうかチェックする。Android だと Activity 分けた方が賢明
		// if(TVPExecuteUserConfig()) return;
#ifdef KRKRZ_HAS_ELEMENTS
		if (TVPExecuteElementsUserConfig()) {
			TVPSetUserConfigExitFlag(true);
			return false; // SDL_AppInit 側で SDL_APP_SUCCESS として扱う
		}
#endif

		// 非同期画像読み込みは後で実装する
		image_load_thread_ = new tTVPAsyncImageLoader();
		file_cache_thread_ = new tTVPStorageCacheThread();

#ifdef KRKRZ_CPU_CORE_STORAGE
		image_load_thread_->SetProcessorNo(KRKRZ_CPU_CORE_STORAGE);
		image_load_thread_->SetProcessorNo(KRKRZ_CPU_CORE_STORAGE);
#endif

// システム初期化
		TVPSystemInit();

		// スクリプトエンジン取得
		OnInitialize(TVPGetScriptEngine());

		SetTitle( tjs_string(TVPKirikiri) );

#ifndef TVP_IGNORE_LOAD_TPM_PLUGIN
//		TVPLoadPluigins(); // load plugin module *.tpm
#endif

		// 静的リンクされたプラグインのロード
		TVPLoadStaticPluigins();

		// start image load thread
		image_load_thread_->StartThread();
		file_cache_thread_->StartThread();

		// run main loop from activity resume.
		return true;

	} catch( const EAbort & ) {
		// nothing to do
	} catch( const Exception &exception ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const TJS::eTJS &e) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const std::exception &e ) {
		ShowException( ttstr(e.what()).c_str() );
	} catch( const char* e ) {
		ShowException( ttstr(e).c_str() );
	} catch( const tjs_char* e ) {
		ShowException( e );
	} catch(...) {
		ShowException( (const tjs_char*)TVPUnknownError );
	}

	return false;
}

void tTVPApplication::Startup() 
{
	// スクリプト起動指示
	SendAppEvent( AM_STARTUP_SCRIPT, 0, 0 );
}

void tTVPApplication::ResizeScreen()
{
    // resize 通知
	// 全ウインドウの更新実行
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->SendMessage(AM_DISPLAY_RESIZE, 0, 0);
	}
}

void tTVPApplication::RequestUpdate()
{
	// 全ウインドウの表示更新要請
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->SendMessage(AM_REQUEST_UPDATE, 0, 0);
	}
}

/**
 * イベントキューからすべてのイベントをディスパッチ
 */
void
tTVPApplication::Dispatch()
{
	KRKRZ_RENDER_STATS_SCOPE(TVPRenderStatsAddFrameDispatch);
	try {
		// 送信に失敗してリトライキューに積まれたイベントを再送する。
		// (通常イベントは SDL イベント経由で SDL_AppEvent → DispatchAppEvent に届く)
		for(;;) {
			tjs_int message; tjs_int64 wparam, lparam;
			{
				std::lock_guard<std::mutex> lock( retry_que_mutex_ );
				if( retry_que_.empty() ) break;
				std::tie( message, wparam, lparam ) = retry_que_.front();
				retry_que_.pop();
			}
			if( !_SendAppEvent( message, wparam, lparam ) ) {
				// まだ失敗するなら積み直して今回は諦める(次フレームで再試行)
				size_t qsize;
				{
					std::lock_guard<std::mutex> lock( retry_que_mutex_ );
					retry_que_.push( std::make_tuple( message, wparam, lparam ) );
					qsize = retry_que_.size();
				}
				TVPLOG_WARNING("[AppEvent] retry re-send still FAILED msg={} (retry queue size={})", message, qsize);
				break;
			}
		}

		UpdateVideoOverlay();

		// 吉里吉里イベント配信実行
		if(!TVPSystemUninitCalled) {
			DeliverEvents();
		}

	} catch( const EAbort & ) {
		// nothing to do
	} catch( const Exception &exception ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException(exception.what());
	} catch( const TJS::eTJSScriptError &e ) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const TJS::eTJS &e) {
		TVPOnError();
		if(!TVPSystemUninitCalled)
			ShowException( e.GetMessage().c_str() );
	} catch( const std::exception &e ) {
		if(!TVPSystemUninitCalled)
			ShowException( ttstr(e.what()).c_str() );
	} catch( const char* e ) {
		if(!TVPSystemUninitCalled)
			ShowException( ttstr(e).c_str() );
	} catch( const tjs_char* e ) {
		if(!TVPSystemUninitCalled)
			ShowException( e );
	} catch(...) {
		if(!TVPSystemUninitCalled)
			ShowException( (const tjs_char*)TVPUnknownError );
	}

}

void tTVPApplication::UpdateVideoOverlay() 
{
	// 全ウインドウの更新実行
    for (auto it = windows_.begin();it != windows_.end();it++) {
		(*it)->UpdateVideoOverlay();
    }
}

// -------------------------------------------------------------------
// アプリ本体側から通知
// -------------------------------------------------------------------

void tTVPApplication::addEventHandler( AppEventInterface* handler )
{
	std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
	auto it = std::find(event_handlers_.begin(), event_handlers_.end(), handler);
	if( it == event_handlers_.end() ) {
		event_handlers_.push_back( handler );
	}
}

void tTVPApplication::removeEventHandler( AppEventInterface* handler )
{
	std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
	auto it = std::remove(event_handlers_.begin(), event_handlers_.end(), handler);
	event_handlers_.erase( it, event_handlers_.end() );
}

// 任意スレッドから呼べる。即送信し、失敗時のみリトライキューに積む。
void tTVPApplication::SendAppEvent( tjs_int message, tjs_int64 wparam, tjs_int64 lparam )
{
	if( !_SendAppEvent( message, wparam, lparam ) ) {
		size_t qsize;
		{
			std::lock_guard<std::mutex> lock( retry_que_mutex_ );
			retry_que_.push( std::make_tuple( message, wparam, lparam ) );
			qsize = retry_que_.size();
		}
		// 送信失敗 (SDL イベントキュー満杯等)。次フレームの Dispatch() で再送される。
		TVPLOG_WARNING("[AppEvent] _SendAppEvent FAILED msg={} -> retry queue (size={})", message, qsize);
		if( qsize >= 64 )
			TVPLOG_WARNING("[AppEvent] retry queue abnormally large: {} (送信先 SDL イベントキューが詰まっている可能性)", qsize);
	}
}

// メインスレッドでの呼び返し。app 独自イベントを処理し、未消費なら全ハンドラへ配る。
bool tTVPApplication::DispatchAppEvent( tjs_int message, tjs_int64 wparam, tjs_int64 lparam )
{
	// アプリ独自イベント
	switch( message ) {
	case AM_STARTUP_SCRIPT:
		TVPInitializeStartupScript();
		// 初回スクリプトロード完了をホストへ通知 (ローディング表示の終了等)
		OnStartupScriptDone();
		return true;
	default:
		break;
	}

	// 登録ハンドラへブロードキャスト。各ハンドラは message で取捨選択する。
	bool handled = false;
	std::lock_guard<std::mutex> lock( event_handlers_mutex_ );
	for( auto* h : event_handlers_ ) {
		if( h && h->Dispatch( message, wparam, lparam ) ) handled = true;
	}
	return handled;
}

void tTVPApplication::AddWindow( TTVPWindowForm* window ) 
{
	windows_.push_back(window);
}

void tTVPApplication::DelWindow( TTVPWindowForm* window ) 
{
	auto i = std::remove(windows_.begin(), windows_.end(), window );
	windows_.erase( i, windows_.end() );
}

void tTVPApplication::SendMessage( void *window, tjs_int message, tjs_int64 wparam, tjs_int64 lparam )
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendMessage( message, wparam, lparam );
			break;
		}
	}
}

void tTVPApplication::SendTouchMessage( void *window, tjs_int type, float x, float y, float c, int id, tjs_uint64 tick )
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendTouchMessage( type, x, y, c, id, tick );
			break;
		}
	}
}

void tTVPApplication::SendMouseMessage( void *window, tjs_int type, int button, int shift, int x, int y)
{
	// ウインドウにメッセージを送る
	for (auto it = windows_.begin();it != windows_.end();it++) {
		if( (*it)->NativeWindowHandle() == window ) {
			(*it)->SendMouseMessage( type, button, shift, x, y );
			break;
		}
	}
}

void tTVPApplication::LoadImageRequest( class iTJSDispatch2 *owner, class tTJSNI_Bitmap* bmp, const ttstr &name )
{
	if( image_load_thread_ ) {
		image_load_thread_->LoadRequest( owner, bmp, name );
	}
}

void tTVPApplication::LoadImagePrefetchRequest( const ttstr &name )
{
	if( image_load_thread_ ) {
		image_load_thread_->PrefetchRequest( name );
	}
}

void tTVPApplication::CacheFileRequest( const ttstr &name, bool fast, tjs_uint64 minSize )
{
	if (file_cache_thread_) {
		file_cache_thread_->LoadRequest(name, fast, minSize);
	}
}

void tTVPApplication::CacheFileClear( const ttstr &name)
{
	if (file_cache_thread_) {
		file_cache_thread_->ClearCache(name);
	}
}

void tTVPApplication::CacheFileClearOld(int keepTime, bool force)
{
	TVPClearOldStorageCache(keepTime, force);
}

void tTVPApplication::CacheFileSetMaxSize( int maxSize)
{
	TVPSetMaxStorageCacheSize(maxSize);
}

bool tTVPApplication::CacheIsLoading(bool fast) const
{
	if (file_cache_thread_) {
		return file_cache_thread_->IsLoading(fast);
	}
	return false;
}

#include "NullDrawDevice.h"

// DrawDevice実装を返す
tTJSNativeClass* 
tTVPApplication::GetDefaultDrawDevice()
{
	return new tTJSNC_NullDrawDevice();
}

// BitmapBits 系 allocator は Sized mode で current_used / peak_used を追跡。
// size は free 時に iTVPMemoryAllocator::free(void*, size_t) 経由で
// tTVPBitmapBitsAlloc から渡される (record->size + ヘッダ込みの allocbytes)。
// 旧 free(void*) も残しているが、こちらは size 不明のため stats が
// 整合しなくなる (caller 側で必ず size 付きを使う)。
class BasicAllocator : public iTVPMemoryAllocator
{
	tTVPMemoryAllocatorStatsCollector stats_{
		tTVPMemoryAllocatorStatsCollector::Mode::Sized};
public:
	BasicAllocator() {
		TVPAddLog( TJS_W("(info) Use malloc for Bitmap") );
	}
	void* allocate( size_t size ) override { return allocate(size, TVPAllocTag::BitmapBits); }
	void* allocate( size_t size, TVPAllocTag tag ) override {
		void* p = malloc(size);
		if (p) stats_.recordAlloc(size, tag);
		return p;
	}
	void free( void* mem ) override { stats_.recordFree(0); ::free( mem ); }
	void free( void* mem, size_t size ) override {
		stats_.recordFree(size, TVPAllocTag::BitmapBits);
		::free( mem );
	}
	Stats getStats() const override { return stats_.snapshot(); }
	TagStats getTagStats(TVPAllocTag tag) const override { return stats_.tagSnapshot(tag); }
	void resetPeak() override { stats_.resetPeak(); }
};

// BitmapAllocator のプールサイズ既定値 (バイト)。
// CLI `-bitmappoolsize=N` (MB)。none/0 で pool 無効化 → 従来 raw malloc。
size_t TVPGetBitmapAllocatorPoolSize()
{
	tTJSVariant val;
	if(TVPGetCommandLine(TJS_W("-bitmappoolsize"), &val)) {
		ttstr str(val);
		if(str == TJS_W("none") || str == TJS_W("off") || str == TJS_W("0")) {
			return 0;
		}
		tjs_int64 mb = (tjs_int64)val;
		if(mb > 0) return (size_t)mb * 1024 * 1024;
	}
	// 既定: 1024 MB (= 1 GB)。1080p RGBA = ~8 MB ・ 多数同時保持 + Bitmap.loadAsync
	// preload + 拡大画像等を見越して大きめに確保。32-bit ビルドや小型機は
	// 起動時に -bitmappoolsize=N で絞ること。
	return (size_t)1024 * 1024 * 1024;
}

// Bitmap用のAllocatorを返す
iTVPMemoryAllocator *
tTVPApplication::CreateBitmapAllocator()
{
	size_t pool_size = TVPGetBitmapAllocatorPoolSize();
	if(pool_size == 0) {
		TVPAddLog(TJS_W("(info) Use malloc for Bitmap"));
		return new BasicAllocator();
	}
	return new TVPPooledAllocator(pool_size, "BitmapPool", TVPAllocTag::BitmapBits);
}

// システムアロケータ情報を返す
// デフォルトはグローバルなデフォルト実装を返す。
// 組込みプラットフォーム固有実装は、派生クラスでオーバーライドして
// プラットフォームアロケータの情報を返す。
// 注: TVPGetSystemAllocatorInfo() は Application 経由で取りに来るので、
// ここで呼ぶと無限再帰になる。default 取得は専用関数を使う。
iTVPSystemAllocatorInfo *
tTVPApplication::GetSystemAllocatorInfo()
{
	return TVPGetDefaultSystemAllocatorInfo();
}

void 
tTVPApplication::InitRandomGenerator()
{
#ifdef pid_t
	pid_t id = gettid(); // pthread
	TVPPushEnvironNoise(&id, sizeof(id));
#endif
}

tjs_int 
tTVPApplication::GetDensity() const
{
	return 96;
}

void tTVPApplication::BeginContinuousEvent() 
{
	if (!ContinuousEventCalling) {
		ContinuousEventCalling = true;
	}
}

void tTVPApplication::EndContinuousEvent() {
	if (ContinuousEventCalling) {
		ContinuousEventCalling = false;
	}
}

void tTVPApplication::DeliverEvents() {
	// システムイベント無効中 (System.eventDisabled = true) は配信しない。
	// WINVER のメッセージループ停止に相当 (generic/base/EventImpl.cpp 参照)。
	if(TVPGetSystemEventDisabledState()) return;

	if(ContinuousEventCalling)
		TVPProcessContinuousHandlerEventFlag = true; // set flag

	TVPDeliverAllEvents();
}
