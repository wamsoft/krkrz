
#include "tjsCommHead.h"

#include "BitmapIntf.h"
#include "GraphicsLoadThread.h"
#include "ThreadIntf.h"
#include "UserEvent.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "StorageCache.h"  // TVPClearStorageCache (auto-drop on prefetch finish/fail)
#include "LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "UtilStreams.h"
#include "LayerIntf.h"

#include "Application.h"
#include "BitmapBitsAlloc.h"

tTVPTmpBitmapImage::tTVPTmpBitmapImage()
	: w(0), h(0), pitch(0), buf(NULL), MetaInfo(NULL)
{}
tTVPTmpBitmapImage::~tTVPTmpBitmapImage() {
	if(buf) {
		tTVPBitmapBitsAlloc::Free( buf );
		buf = NULL;
	}
	if( MetaInfo ) {
		delete MetaInfo;
		MetaInfo = NULL;
	}
}
tTVPImageLoadCommand::tTVPImageLoadCommand() : owner_(NULL), bmp_(NULL), dest_(NULL), prefetch_only_(false) {}
tTVPImageLoadCommand::~tTVPImageLoadCommand() {
	if( owner_ ) {
		owner_->Release();
		owner_ = NULL;
	}
	if( dest_ ) {
		delete dest_;
		dest_ = NULL;
	}
	bmp_ = NULL;
}

static void TVPLoadGraphicAsync_SizeCallback(void *callbackdata, tjs_uint w, tjs_uint h)
{
	tTVPTmpBitmapImage* img = (tTVPTmpBitmapImage*)callbackdata;
	img->h = h;
	img->w = w;

	BitmapInfomation *info = TVPCreateBitmapInfo(w, h, 32);
	img->buf = (tjs_uint32*)tTVPBitmapBitsAlloc::Alloc( info->GetImageSize(), w, h );
	img->pitch = info->GetPitchBytes();
	delete info;
}
//---------------------------------------------------------------------------
static void* TVPLoadGraphicAsync_ScanLineCallback(void *callbackdata, tjs_int y)
{
	tTVPTmpBitmapImage* img = (tTVPTmpBitmapImage*)callbackdata;
	if( y >= 0 ) {
		if( y < (tjs_int)img->h ) {
			return (img->h - y -1 ) * img->pitch + (tjs_uint8*)img->buf;
		} else {
			return NULL;
		}
	}
	return NULL; // -1 の時のフラッシュ処理は何もしない
}
//---------------------------------------------------------------------------
static void TVPLoadGraphicAsync_MetaInfoPushCallback(void *callbackdata, const ttstr & name, const ttstr & value)
{
	tTVPTmpBitmapImage * img = (tTVPTmpBitmapImage *)callbackdata;

	if(!img->MetaInfo) img->MetaInfo = new std::vector<tTVPGraphicMetaInfoPair>();
	img->MetaInfo->push_back(tTVPGraphicMetaInfoPair(name, value));
}
//---------------------------------------------------------------------------

tTVPAsyncImageLoader::tTVPAsyncImageLoader()
: tTVPThread("AsyncImageLoader")
#ifdef __WINVER__
, EventQueue(this,&tTVPAsyncImageLoader::Proc)
#endif
{
#ifdef __WINVER__
	EventQueue.Allocate();
#else
	Application->addEventHandler(this);
#endif
}
tTVPAsyncImageLoader::~tTVPAsyncImageLoader() {
	ExitRequest();
	WaitFor();
#ifdef __WINVER__
	EventQueue.Deallocate();
#else
	Application->removeEventHandler(this);
#endif
	while( CommandQueue.size() > 0 ) {
		tTVPImageLoadCommand* cmd = CommandQueue.front();
		CommandQueue.pop();
		delete cmd;
	}
	while( LoadedQueue.size() > 0 ) {
		tTVPImageLoadCommand* cmd = LoadedQueue.front();
		LoadedQueue.pop();
		delete cmd;
	}
}
void tTVPAsyncImageLoader::ExitRequest() {
	Terminate();
	PushCommandQueueEvent.Set();
}
void tTVPAsyncImageLoader::Execute() {
	// プライオリティは最低にする
	SetPriority(ttpIdle);
	LoadingThread();
}
void tTVPAsyncImageLoader::SendToLoadFinish() {
#ifdef __WINVER__
	NativeEvent ev(TVP_EV_IMAGE_LOAD_THREAD);
	EventQueue.PostEvent(ev);
#else
	Application->SendAppEvent( TVP_EV_IMAGE_LOAD_THREAD, 0, 0 );
#endif
}
#ifdef __WINVER__
void tTVPAsyncImageLoader::Proc( NativeEvent& ev )
{
	if(ev.Message != TVP_EV_IMAGE_LOAD_THREAD) {
		EventQueue.HandlerDefault(ev);
		return;
	}
	HandleLoadedImage();
}
#else
bool tTVPAsyncImageLoader::Dispatch( tjs_int message, tjs_int64 /*wparam*/, tjs_int64 /*lparam*/ )
{
	if( message != TVP_EV_IMAGE_LOAD_THREAD ) return false;
	HandleLoadedImage();
	return true;
}
#endif
void tTVPAsyncImageLoader::HandleLoadedImage() {
	bool loading;
	do {
		loading = false;
		tTVPImageLoadCommand* cmd = NULL;
		{
			tTJSCriticalSectionHolder cs(ImageQueueCS);
			if( LoadedQueue.size() > 0 ) {
				cmd = LoadedQueue.front();
				LoadedQueue.pop();
				loading = true;
			}
		}
		if( cmd != NULL ) {
			// prefetch_only_ なエントリは worker 側で cache push 済み。ここに来るのは
			// loadAsync (owner 付き) 専用。
			// path_ は tjs_string。メインスレッドで独立 ttstr に変換して ttstr API へ渡す。
			ttstr path(cmd->path_);
			cmd->bmp_->SetLoading( false );
			if( cmd->result_.length() > 0 ) {
				// error: file 層 cache に entry が残っているので駆逐 (push が走らないと
				// auto-drop が走らずリーク)
				TVPClearStorageCache(path);

				tTJSVariant param[4];
				param[0] = tTJSVariant((iTJSDispatch2*)NULL,(iTJSDispatch2*)NULL);
				param[1] = 1; // true async
				param[2] = 1; // true error
				param[3] = cmd->result_.c_str(); // error_mes
				static ttstr eventname(TJS_W("onLoaded"));
				if( cmd->owner_->IsValid(0,NULL,NULL,cmd->owner_) == TJS_S_TRUE ) {
					TVPPostEvent(cmd->owner_, cmd->owner_, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
				}

				if( cmd->dest_->MetaInfo ) {
					delete cmd->dest_->MetaInfo;
					cmd->dest_->MetaInfo = NULL;
				}
			} else {
				iTJSDispatch2* metainfo = TVPMetaInfoPairsToDictionary(cmd->dest_->MetaInfo);

				cmd->bmp_->SetSizeAndImageBuffer( cmd->dest_->w, cmd->dest_->h, cmd->dest_->buf );
				cmd->dest_->buf = NULL;
				// 読込み完了時にもキャッシュチェック(非同期なので完了前に読み込まれている可能性あり)
				if( TVPHasImageCache( path, glmNormal, 0, 0, TVP_clNone ) == false ) {
					TVPPushGraphicCache( path, cmd->bmp_->GetBitmap(), cmd->dest_->MetaInfo );
					cmd->dest_->MetaInfo = NULL;
				} else {
					// 既に decode 層 cache に entry あり → push を skip。
					// この経路は file 層 cache の auto-drop も走らないので明示駆逐。
					TVPClearStorageCache(path);
					delete cmd->dest_->MetaInfo;
					cmd->dest_->MetaInfo = NULL;
				}

				tTJSVariant param[4];
				param[0] = tTJSVariant(metainfo,metainfo);
				if( metainfo ) metainfo->Release();
				param[1] = 1; // true async
				param[2] = 0; // false error
				param[3] = TJS_W(""); // error_mes
				static ttstr eventname(TJS_W("onLoaded"));
				if( cmd->owner_->IsValid(0,NULL,NULL,cmd->owner_) == TJS_S_TRUE ) {
					TVPPostEvent(cmd->owner_, cmd->owner_, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
				}
			}
			// loadAsync 経路でも InFlight 表に登録されているので削除して signal
			// (sync TVPLoadGraphic からの wait はメインスレッド走行中なので
			//  通常は使わないが、将来の多 waiter 化に備えて signal は出す)
			{
				tTJSCriticalSectionHolder cs(InFlightCS);
				auto it = InFlightTable.find(cmd->path_);
				if(it != InFlightTable.end()) {
					it->second->done.store(true);
					it->second->completeEvent.Set();
					InFlightTable.erase(it);
				}
			}
			delete cmd;
		}
	} while(loading);
}
//---------------------------------------------------------------------------
void tTVPAsyncImageLoader::FinalizePrefetchOnWorker( tTVPImageLoadCommand* cmd )
{
	// worker スレッドから直接呼ばれる。cache push (TVPGraphicCacheCS で守られている)
	// と InFlight signal だけ行い、メインスレッドへの通知はしない。
	// path_ は tjs_string。worker スレッドで独立 ttstr に変換して ttstr API へ渡す。
	ttstr path(cmd->path_);
	if( cmd->result_.length() == 0 && cmd->dest_ && cmd->dest_->buf ) {
		// dest_->buf を tTVPBaseBitmap に詰めて cache push
		// 注: buf 所有権は SetSizeAndImageBuffer (tTVPBitmap ctor) に移るので
		//     dest_->buf を NULL にしておく
		// 注: TVPGetInitialBitmap() は静的共有 tTVPBitmap を持つためコピー
		//     ctor 経由だと worker / main で RefCount を非 atomic に触り合うこと
		//     になる。ここでは 1x1 ダミーで構築 (即座に SetSizeAndImageBuffer
		//     で破棄するので無駄なアロケーションは小さい)
		try {
			tTVPBaseBitmap *tmp = new tTVPBaseBitmap( 1, 1, 32 );
			tmp->SetSizeAndImageBuffer( cmd->dest_->w, cmd->dest_->h, cmd->dest_->buf );
			cmd->dest_->buf = NULL;
			if( TVPHasImageCache( path, glmNormal, 0, 0, TVP_clNone ) == false ) {
				TVPPushGraphicCache( path, tmp, cmd->dest_->MetaInfo );
				cmd->dest_->MetaInfo = NULL; // ownership moved
			} else {
				// 既に decode 層 cache に積まれていたので push を skip。
				// ただし、ここまで decode を走らせるためにこのパス用の file 層
				// cache entry を作ってしまっている (= 並走 sync load の auto-drop
				// 後に prefetch worker 側で再度 EntryStorageCache が走った)。
				// push 経路の auto-drop が動かないので、明示的に file 層 cache
				// から該当 entry を駆逐して buffer リークを防ぐ。
				TVPClearStorageCache(path);
				if( cmd->dest_->MetaInfo ) { delete cmd->dest_->MetaInfo; cmd->dest_->MetaInfo = NULL; }
			}
			delete tmp;
		} catch(...) {
			// ログのみ。prefetch エラーは sync 側で従来通り再 decode される
			cmd->result_ = TJS_W("prefetch finalize failed");
		}
	}
	// 失敗 prefetch の場合も file 層 cache に entry が残っているので駆逐
	// (cmd->dest_->buf が NULL = decode 中エラー、または cmd->result_ 非空 = handler 失敗)
	if( cmd->result_.length() != 0 ) {
		TVPClearStorageCache(path);
	}
	// InFlight 表から外して signal
	{
		tTJSCriticalSectionHolder cs(InFlightCS);
		auto it = InFlightTable.find(cmd->path_);
		if(it != InFlightTable.end()) {
			it->second->done.store(true);
			it->second->completeEvent.Set();
			InFlightTable.erase(it);
		}
	}
	delete cmd;
}
//---------------------------------------------------------------------------

// onLoaded( dic, is_async, is_error, error_mes ); エラーは
// sync ( main thead )
void tTVPAsyncImageLoader::LoadRequest( iTJSDispatch2 *owner, tTJSNI_Bitmap* bmp, const ttstr &name ) {
	//tTVPBaseBitmap* dest = new tTVPBaseBitmap( 32, 32, 32 );
	tTVPBaseBitmap dest( TVPGetInitialBitmap() );
	iTJSDispatch2* metainfo = NULL;
	ttstr nname = TVPNormalizeStorageName(name);
	if( TVPCheckImageCache(nname,&dest,glmNormal,0,0,TVP_clNone,&metainfo) ) {
		// キャッシュ内に発見、即座に読込みを完了する
		bmp->CopyFrom( &dest );
		bmp->SetLoading( false );

		tTJSVariant param[4];
		param[0] = tTJSVariant(metainfo,metainfo);
		if( metainfo ) metainfo->Release();
		param[1] = 0; // false
		param[2] = 0; // false
		param[3] = TJS_W(""); // error_mes
		static ttstr eventname(TJS_W("onLoaded"));
		TVPPostEvent(owner, owner, eventname, 0, TVP_EPT_IMMEDIATE, 4, param);
		return;
	}
	if( TVPIsExistentStorage(name) == false ) {
		TVPThrowExceptionMessage(TVPCannotFindStorage, name);
	}
	ttstr ext = TVPExtractStorageExt(name);
	if(ext == TJS_W("")) {
		TVPThrowExceptionMessage(TJS_W("Filename extension not found/%1"), name);
	}

	PushLoadQueue( owner, bmp, nname );
}

// tTJSCriticalSectionHolder cs_holder(TVPCreateStreamCS);
//	iTJSBinaryStream* stream = TVPCreateStream(nname, TJS_BS_READ);
// TVPCreateStream はロックされているので、非同期で実行可能
void tTVPAsyncImageLoader::PushLoadQueue( iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp, const ttstr &nname ) {
	tTVPImageLoadCommand* cmd = new tTVPImageLoadCommand();
	cmd->owner_ = owner;
	owner->AddRef();
	cmd->bmp_ = bmp;
	cmd->path_ = nname.AsStdString();
	cmd->dest_ = new tTVPTmpBitmapImage();
	cmd->result_.clear();
	cmd->prefetch_only_ = false;
	// InFlight 登録 (重複時は既存エントリ流用、completeEvent も共有)。
	// loadAsync 自体は重複命令でも従来通り独自に decode + bmp attach を行う必要が
	// あるため、CommandQueue へのプッシュはスキップしない。
	{
		tTJSCriticalSectionHolder cs(InFlightCS);
		tjs_string key = nname.AsStdString();
		if(InFlightTable.find(key) == InFlightTable.end()) {
			auto e = std::make_shared<tTVPImagePrefetchInFlight>();
			e->path = key;
			InFlightTable[key] = e;
		}
	}
	{
		// キューをロックしてプッシュ
		tTJSCriticalSectionHolder cs(CommandQueueCS);
		CommandQueue.push(cmd);
	}
	// 追加したことをイベントで通知
	PushCommandQueueEvent.Set();
}
//---------------------------------------------------------------------------
void tTVPAsyncImageLoader::PrefetchRequest( const ttstr &name )
{
	// cache key は autopath 解決後の物理 path に統一 (TVPLoadGraphic 同期側と整合)。
	// 同一 file を異なる名前 ("bg.jpg" vs "image/bg.jpg") で要求しても 1 entry に集約される。
	ttstr nname = TVPResolveCachePath(name);

	// 既にキャッシュ済みなら何もしない
	if( TVPHasImageCache( nname, glmNormal, 0, 0, TVP_clNone ) ) return;

	// 既に進行中ならスキップ (loadAsync 経路の InFlight ともここで重複検出)
	{
		tTJSCriticalSectionHolder cs(InFlightCS);
		tjs_string key = nname.AsStdString();
		if(InFlightTable.find(key) != InFlightTable.end()) return;
		auto e = std::make_shared<tTVPImagePrefetchInFlight>();
		e->path = key;
		InFlightTable[key] = e;
	}

	// 拡張子チェック (handler が無い場合 InFlight 登録を取り消す)
	ttstr ext = TVPExtractStorageExt(nname);
	if(ext.IsEmpty() || TVPGetGraphicLoadHandler(ext) == NULL) {
		tTJSCriticalSectionHolder cs(InFlightCS);
		auto it = InFlightTable.find(nname.AsStdString());
		if(it != InFlightTable.end()) {
			it->second->done.store(true);
			it->second->completeEvent.Set();
			InFlightTable.erase(it);
		}
		return;
	}

	tTVPImageLoadCommand* cmd = new tTVPImageLoadCommand();
	cmd->owner_ = NULL;
	cmd->bmp_ = NULL;
	cmd->path_ = nname.AsStdString();
	cmd->dest_ = new tTVPTmpBitmapImage();
	cmd->result_.clear();
	cmd->prefetch_only_ = true;
	{
		tTJSCriticalSectionHolder cs(CommandQueueCS);
		CommandQueue.push(cmd);
	}
	PushCommandQueueEvent.Set();
}
//---------------------------------------------------------------------------
std::shared_ptr<tTVPImagePrefetchInFlight>
tTVPAsyncImageLoader::FindInFlight( const ttstr &nname )
{
	tTJSCriticalSectionHolder cs(InFlightCS);
	auto it = InFlightTable.find(nname.AsStdString());
	if(it == InFlightTable.end()) return nullptr;
	return it->second;
}
//---------------------------------------------------------------------------
bool tTVPAsyncImageLoader::IsAnyInFlight()
{
	tTJSCriticalSectionHolder cs(InFlightCS);
	return !InFlightTable.empty();
}
//---------------------------------------------------------------------------
void tTVPAsyncImageLoader::FlushPrefetchQueue()
{
	// CommandQueue から prefetch_only エントリのみ取り除く
	std::queue<tTVPImageLoadCommand*> kept;
	std::vector<tTVPImageLoadCommand*> dropped;
	{
		tTJSCriticalSectionHolder cs(CommandQueueCS);
		while(!CommandQueue.empty()) {
			tTVPImageLoadCommand* c = CommandQueue.front();
			CommandQueue.pop();
			if(c->prefetch_only_) dropped.push_back(c);
			else                  kept.push(c);
		}
		CommandQueue.swap(kept);
	}
	// dropped の InFlight エントリも消す + signal して待ち手を起こす
	{
		tTJSCriticalSectionHolder cs(InFlightCS);
		for(auto* c : dropped) {
			auto it = InFlightTable.find(c->path_);
			if(it != InFlightTable.end()) {
				it->second->done.store(true);
				it->second->completeEvent.Set();
				InFlightTable.erase(it);
			}
		}
	}
	for(auto* c : dropped) delete c;
}
//---------------------------------------------------------------------------
void tTVPAsyncImageLoader::LoadingThread() {
	while( !GetTerminated() ) {
		// キュー追加イベント待ち
		PushCommandQueueEvent.WaitFor(0);
		if( GetTerminated() ) break;
		bool loading;
		do {
			loading = false;
			tTVPImageLoadCommand* cmd = NULL;

			{ // Lock
				tTJSCriticalSectionHolder cs(CommandQueueCS);
				if( CommandQueue.size() ) {
					cmd = CommandQueue.front();
					CommandQueue.pop();
				}
			}
			if( cmd ) {
				loading = true;
				LoadImageFromCommand(cmd);
				if( cmd->prefetch_only_ ) {
					// prefetch: worker スレッドで完結 (cache push + InFlight signal)
					FinalizePrefetchOnWorker(cmd);
				} else {
					// loadAsync: メインスレッドへ完了通知して bmp attach + onLoaded 発火
					{	// Lock
						tTJSCriticalSectionHolder cs(ImageQueueCS);
						LoadedQueue.push(cmd);
					}
					// Send to message
					SendToLoadFinish();
				}
			}
		} while( loading && !GetTerminated() );
	}
}
void tTVPAsyncImageLoader::LoadImageFromCommand( tTVPImageLoadCommand* cmd ) {
	// path_ は tjs_string。worker スレッドで独立 ttstr に変換して ttstr API へ渡す。
	ttstr path(cmd->path_);
	ttstr ext = TVPExtractStorageExt(path);
	tTVPGraphicHandlerType* handler = NULL;
	if(ext == TJS_W("")) {
		cmd->result_ = TJS_W("Filename extension not found");
	} else {
		handler = TVPGetGraphicLoadHandler(ext);
	}
	if( handler ) {
		try {
			tTVPStreamHolder holder(path);
			(handler->Load)(handler->FormatData, (void*)cmd->dest_, TVPLoadGraphicAsync_SizeCallback,
				TVPLoadGraphicAsync_ScanLineCallback, TVPLoadGraphicAsync_MetaInfoPushCallback,
				holder.Get(), -1, glmNormal );
		} catch(...) {
			// 例外は全てキャッチ
			cmd->result_ = TVPFormatMessage(TVPImageLoadError, path).c_str();
		}
	} else {
		// error
		cmd->result_ = TVPFormatMessage(TVPUnknownGraphicFormat, path).c_str();
	}
}
//---------------------------------------------------------------------------
// global helpers (GraphicsLoaderIntf.cpp 等から呼ばれる)
//---------------------------------------------------------------------------
bool TVPWaitForImagePrefetch(const ttstr &nname, tjs_int timeoutMs)
{
	if(!Application) return false;
	tTVPAsyncImageLoader *loader = Application->GetImageLoadThread();
	if(!loader) return false;

	auto entry = loader->FindInFlight(nname);
	if(!entry) return false; // 進行中ではない
	if(entry->done.load()) return true; // 既に完了済み

	// 完了通知を待つ。timeout は ms 単位。
	// 完了後に done=true, completeEvent.Set() が worker から行われる。
	// race のため WaitFor 後に done を再確認する。
	bool signaled = entry->completeEvent.WaitFor(timeoutMs);
	if(entry->done.load()) return true;
	return signaled; // signaled=true でも done=false は通常起きないが安全側
}
//---------------------------------------------------------------------------
void TVPFlushImagePrefetchQueue()
{
	if(!Application) return;
	tTVPAsyncImageLoader *loader = Application->GetImageLoadThread();
	if(!loader) return;
	loader->FlushPrefetchQueue();
}
//---------------------------------------------------------------------------
void TVPRequestImagePrefetch(const ttstr &name)
{
	if(!Application) return;
	tTVPAsyncImageLoader *loader = Application->GetImageLoadThread();
	if(!loader) return;
	loader->PrefetchRequest(name);
}
//---------------------------------------------------------------------------
bool TVPIsImagePrefetchLoading()
{
	if(!Application) return false;
	tTVPAsyncImageLoader *loader = Application->GetImageLoadThread();
	if(!loader) return false;
	return loader->IsAnyInFlight();
}
//---------------------------------------------------------------------------

