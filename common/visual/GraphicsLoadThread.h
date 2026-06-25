
#ifndef __GRAPHICS_LOAD_THREAD_H__
#define __GRAPHICS_LOAD_THREAD_H__

#include <queue>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include "ThreadIntf.h"
// WINVER は従来の Win32 窓ベース NativeEventQueue、それ以外 (SDL/Generic) は
// Application の AppEventInterface 経由で完了通知を受ける。doc/AppEvent.md 参照。
#ifdef __WINVER__
#include "NativeEventQueue.h"
#else
#include "Application.h"
#endif
#include "GraphicsLoaderIntf.h"

// BaseBitmap を使うとリエントラントではないので、別の構造体に独自にロードする必要がある
struct tTVPTmpBitmapImage {
	tjs_uint32 w;
	tjs_uint32 h;
	tjs_int pitch;
	tjs_uint32* buf;
	std::vector<tTVPGraphicMetaInfoPair> * MetaInfo;
	tTVPTmpBitmapImage();
	~tTVPTmpBitmapImage();
// パレット関連は現状読まない、ファイルに従うのではなく、事前指定方式なので
};

struct tTVPImageLoadCommand {
	iTJSDispatch2*			owner_;	// send to event (prefetch_only=true 時は NULL)
	class tTJSNI_Bitmap*	bmp_;	// set bitmap image (prefetch_only=true 時は NULL)
	// path_/result_ は メインスレッドとロードワーカー間を CommandQueue/LoadedQueue
	// で受け渡す越境データ。ttstr の COW バッファ共有 + 非 atomic RefCount で
	// 二重解放を起こすため、独立バッファを持つ tjs_string で保持する
	// (doc/TtstrDataRetention.md H1)。ttstr API を呼ぶ箇所では各スレッドで
	// 一時 ttstr に変換する。
	tjs_string				path_;
	tTVPTmpBitmapImage*		dest_;
	tjs_string				result_;
	bool					prefetch_only_; // owner/bmp 不要、cache に push するだけ
	tTVPImageLoadCommand();
	~tTVPImageLoadCommand();
};

// Prefetch 進行中エントリ。同期 TVPLoadGraphic から「進行中なら待つ」用に共有。
struct tTVPImagePrefetchInFlight {
	tjs_string        path;       // 正規化済み (H2: tjs_string 独立保持)
	tTVPThreadEvent   completeEvent;
	std::atomic<bool> done;
	tTVPImagePrefetchInFlight() : done(false) {}
};

class tTVPAsyncImageLoader : public tTVPThread
#ifndef __WINVER__
	, public AppEventInterface
#endif
{
	/** 読込み要求コマンドのキュー用CS */
	tTJSCriticalSection CommandQueueCS;
	/** 読込み済み画像キュー用CS */
	tTJSCriticalSection ImageQueueCS;
	/** Prefetch 進行中タスクテーブル CS */
	tTJSCriticalSection InFlightCS;

#ifdef __WINVER__
	/** ロード完了後メインスレッドで処理するためのメッセージキュー */
	NativeEventQueue<tTVPAsyncImageLoader> EventQueue;
#endif
	/**  読込みスレッドへ読込み要求があったことを伝えるイベント */
	tTVPThreadEvent PushCommandQueueEvent;

	/** 読込み要求コマンドキュー */
	std::queue<tTVPImageLoadCommand*> CommandQueue;
	/** 読込み完了画像キュー (owner-bound = Bitmap.loadAsync 経路のみ使用) */
	std::queue<tTVPImageLoadCommand*> LoadedQueue;

	/** 進行中 prefetch + loadAsync を path で索引するテーブル。
	 *  worker が pop した瞬間からも検索可能にしておくため、
	 *  CommandQueue から pop されても削除しない。完了 (cache push) 時に削除。
	 */
	// キー: main 挿入 ⇄ worker(FinalizePrefetchOnWorker) 削除の越境。RefCount を
	// 持たない tjs_string をキーにして二重解放を防ぐ (doc/TtstrDataRetention.md H3)。
	std::map<tjs_string, std::shared_ptr<tTVPImagePrefetchInFlight>> InFlightTable;

private:
	/**
	 * 読込みスレッドからメインスレッドへ読込みが完了したことを通知する
	 */
	void SendToLoadFinish();
	/**
	 * 読込み完了した画像をメインスレッドでBitmapへ格納して、イベント通知する
	 */
	void HandleLoadedImage();

	/**
	 * 読込みを読込みスレッドに要求する(キューへ入れる)
	 */
	void PushLoadQueue( iTJSDispatch2 *owner, tTJSNI_Bitmap *bmp, const ttstr &nname );

	/**
	 * 読込みスレッド実体
	 * キューにコマンドが入るのを待ち、イベントが来たらキューからコマンドを取り出して読込み処理を実行
	 * 読込みが完了したら読込み済み画像キューに入れてメインスレッドへ完了を通知する
	 */
	void LoadingThread();

	/**
	 * 画像読込み処理
	 */
	void LoadImageFromCommand( tTVPImageLoadCommand* cmd );

	/**
	 * 読込みスレッドメイン
	 */
	void Execute();

	/**
	 * メインスレッドハンドラ
	 * メインスレッドへのイベント(メッセージ)通知を受ける
	 */
#ifdef __WINVER__
	void Proc( NativeEvent& ev );
#else
	bool Dispatch( tjs_int message, tjs_int64 wparam, tjs_int64 lparam ) override;
#endif

	/**
	 * Prefetch 完了直後に worker スレッドから呼ぶ。
	 * cache に push 済みの状態にしてから InFlight エントリを done 化して signal する。
	 */
	void FinalizePrefetchOnWorker( tTVPImageLoadCommand* cmd );

public:
	tTVPAsyncImageLoader();
	~tTVPAsyncImageLoader();

	/**
	 読込みスレッドの終了を要求する(終了は待たない)
	 */
	void ExitRequest();

	/**
	 * 読込み要求 (Bitmap.loadAsync 経路)
	 * メインスレッドから読込みスレッドへ読込みを要求する。
	 * 読込み前にエラーが発生した場合やキャッシュ上に画像があった場合は要求は行われず
	 * 即座に終了し、onLoaded イベントを発生させる。
	 */
	void LoadRequest( iTJSDispatch2 *owner, tTJSNI_Bitmap* bmp, const ttstr &name );

	/**
	 * Prefetch 要求 (Storages.requestCache 経路で画像系拡張子の場合に呼ばれる)
	 * 既にキャッシュ済み or 進行中ならスキップ。それ以外は背景でデコードして
	 * TVPGraphicCache に登録する。完了通知 (onLoaded) は発火しない。
	 */
	void PrefetchRequest( const ttstr &name );

	/**
	 * 進行中エントリの取得 (なければ nullptr)。同期側 wait に使う。
	 */
	std::shared_ptr<tTVPImagePrefetchInFlight> FindInFlight( const ttstr &nname );

	/**
	 * Prefetch キューに残っている prefetch_only エントリをすべて破棄する。
	 * Compact event (MINIMIZE) で TVPClearGraphicCache と一緒に呼ぶ。
	 * worker が掴み中のものは止められない (= 完走させる) 。
	 */
	void FlushPrefetchQueue();

	/**
	 * 進行中の prefetch / loadAsync エントリが 1 つでもあれば true。
	 * Storages.isImagePrefetchLoading から呼ばれる polling 用。
	 */
	bool IsAnyInFlight();
};

// GraphicsLoaderIntf.cpp 等から呼ぶグローバル helper。
// 内部で Application->image_load_thread_ にアクセス。
bool TVPWaitForImagePrefetch(const ttstr &nname, tjs_int timeoutMs);
void TVPFlushImagePrefetchQueue();
void TVPRequestImagePrefetch(const ttstr &name);
// 進行中 prefetch / loadAsync の有無 (= InFlightTable が空でない)。
// TJS Storages.isImagePrefetchLoading から呼ぶ polling 用。
bool TVPIsImagePrefetchLoading();

#endif // __GRAPHICS_LOAD_THREAD_H__
