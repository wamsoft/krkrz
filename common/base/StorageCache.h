
#ifndef __STORAGE_CACHE_H__
#define __STORAGE_CACHE_H__

#include <queue>
#include <set>
#include <vector>
#include "ThreadIntf.h"
#include "UtilStreams.h"
#include <memory>


class tTVPStorageCacheThread : public tTVPThread {

	/** ロード要求エントリ。 minSize > 0 ならファイルサイズが minSize を超える場合のみキャッシュ。 */
	struct LoadRequestItem {
		// name は メインスレッドが投入し、キャッシュスレッドが取り出す越境データ。
		// ttstr の RefCount は非 atomic で COW バッファ共有が二重解放を招くため、
		// 独立バッファを持つ tjs_string で保持する (doc/TtstrDataRetention.md H8)。
		tjs_string name;
		tjs_uint64 minSize;
	};

	/** 読込み要求のキュー用CS */
	tTJSCriticalSection RequestQueueCS;
	/**  読込みスレッドへ読込み要求があったことを伝えるイベント */
	tTVPThreadEvent RequestQueueEvent;
	/** 読込み要求コマンドキュー */
	std::deque<LoadRequestItem> RequestQueue;
	/** 読込み要求コマンドキュー・優先ロード */
	std::deque<LoadRequestItem> RequestQueueFast;
	/** 読み込み中フラグ */
	bool loading;
	bool loadingFast;
	int waitTick;
	tjs_uint64 prevTick;
	
private:
	/*
	 * ロード要求をキャンセルする
	*/
	void CancelLoadQueue( const ttstr &name );

	void CancelAllQueue();

	/**
	 * 読込みスレッドメイン
	 */
	void Execute();

public:
    tTVPStorageCacheThread();
    ~tTVPStorageCacheThread();

	/**
	 読込みスレッドの終了を要求する(終了は待たない)
	 */
	void ExitRequest();
	
	/**
	 * 読込み要求
	 * メインスレッドから読込みスレッドへ読込みを要求する。
	 * 読込み前にエラーが発生した場合やキャッシュ上に画像があった場合は要求は行われない
	 * minSize > 0 を指定すると、ファイルサイズが minSize を超える場合のみキャッシュする
	 * (拡張子別の最小サイズが登録されている場合は、リクエスト個別の指定が優先)。
	 */
	void LoadRequest( const ttstr &name, bool fast=false, tjs_uint64 minSize=0);

    /**
     * 登録されているキャッシュをクリアする
     * 空文字の場合は全消去
    */
    void ClearCache(const ttstr &name);

	/*
	 * キャッシュ処理中かどうか
	 */
	bool IsLoading(bool fast=false);

};

bool TVPCheckStorageCache(const ttstr &name, bool update=false);
iTJSBinaryStream *TVPGetStorageCache(const ttstr &name, bool entry=false);
// 単一 path クリア。
//   force=false (default): 他に shared_ptr 保持者がいる場合はスキップ
//                         (= 「表 = 生存 buffer」の invariant 維持)
//   force=true:   保持者の有無に関わらず表から削除 (書き込み時の stale invalidate 用)
bool TVPClearStorageCache(const ttstr &name, bool force=false);
void TVPClearOldStorageCache(int keepTime, bool force=false);
void TVPClearAllStorageCache();
// transient (pinned 以外) を全消し。doc/legacy/ImagePreloadAndCache.md §18 参照。
void TVPClearTransientStorageCache();

// 既存 entry の pinned 状態を変更。entry が無ければ何もしない
// (pin/unpin の永続管理は TVPPinCache (StorageIntf.h) 側で行う)。
void TVPSetStorageCacheEntryPinned(const ttstr &name, bool pinned);

// pin 集合 (StorageIntf 側で保持) を参照する forward declaration。
// EntryStorageCache の entry 初期化に使用。
bool TVPIsCachePathPinned(const ttstr &name);

size_t TVPGetStorageCacheSize();
void TVPSetMaxStorageCacheSize(size_t size);

// 現在の file 層キャッシュエントリ列挙用。
// Storages.getFileCacheList / dumpFileCacheList / MemoryOverlay 等の観測系で利用。
struct TVPStorageCacheEntryInfo {
	ttstr  name;
	size_t size;
	time_t lastaccess;
	int    usecount;
	bool   pinned;
};
void TVPGetStorageCacheEntries(std::vector<TVPStorageCacheEntryInfo> &out);
// 件数のみ取得 (overlay 等の軽い観測用)。entries 列挙より速い。
void TVPGetStorageCacheCount(size_t &total, size_t &pinned);
// file 層キャッシュ一覧を WARNING ログに出力。
// Storages.dumpFileCacheList / REPL .filecache から共通で呼ばれる実体。
void TVPDumpFileCacheList();

// FileAllocator の容量と StorageCache 側の上限を整合する。
// alloc->capacity() が SIZE_MAX (= 不明) なら何もしない (素 malloc 経路の従来挙動)。
// alloc->capacity() が値を返す場合は MaxStorageCacheSize を capacity * kCacheShareRatio に
// 切り詰め、setPressureCallback で 0.75 / 0.90 二閾値の駆逐 callback を登録する。
// alloc は以降 IsOverMaxStorageCacheSize から available() を参照するために保持される。
// doc/legacy/MemoryBudgetNegotiation.md §4.1 / §4.2 参照。
class iTVPMemoryAllocator;
void TVPNegotiateStorageCacheBudget(iTVPMemoryAllocator *alloc);


#endif // __SOTRAGE_CACHE_H__
