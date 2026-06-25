//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Universal Storage System
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "tjsUtils.h"
#include "MsgIntf.h"
#include "EventIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"

#include "StorageCache.h"
#include "ThreadIntf.h"
#include "UserEvent.h"
#include "EventIntf.h"
#include "StorageIntf.h"
#include "MsgIntf.h"
#include "TickCount.h"

#include "UserEvent.h"

#include "Application.h"
#include "BinaryStreamBuffer.h"

#include <memory>
#include <mutex>

//---------------------------------------------------------------------------


class tTVPSharedMemoryStream : public iTJSBinaryStream
{
protected:
	std::shared_ptr<tTJSBinaryStreamBuffer> Buffer;
	size_t Size;
	tjs_uint CurrentPos;

public:
	tTVPSharedMemoryStream(std::shared_ptr<tTJSBinaryStreamBuffer> buffer) 
	: Buffer(buffer), Size(buffer->size()), CurrentPos(0) {
	}
	virtual ~tTVPSharedMemoryStream() {
	}

	tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
		if (!Buffer) return 0;

		tjs_int64 newpos;
		switch(whence)
		{
		case TJS_BS_SEEK_SET:
			if(offset >= 0)
			{
				if(offset <= Size) CurrentPos = static_cast<tjs_uint>(offset);
			}
			return CurrentPos;

		case TJS_BS_SEEK_CUR:
			if((newpos = offset + (tjs_int64)CurrentPos) >= 0)
			{
				tjs_uint np = (tjs_uint)newpos;
				if(np <= Size) CurrentPos = np;
			}
			return CurrentPos;

		case TJS_BS_SEEK_END:
			if((newpos = offset + (tjs_int64)Size) >= 0)
			{
				tjs_uint np = (tjs_uint)newpos;
				if(np <= Size) CurrentPos = np;
			}
			return CurrentPos;
		}
		return CurrentPos;
	}

	tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) {
		if(CurrentPos + read_size >= Size)
		{
			read_size = Size - CurrentPos;
		}
		memcpy(buffer, Buffer->buffer() + CurrentPos, read_size);
		CurrentPos += read_size;
		return read_size;	
	}
	tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size) {
		TVPThrowExceptionMessage(TVPWriteError);
		return 0;
	}

	void TJS_INTF_METHOD SetEndOfStorage() {
		TVPThrowExceptionMessage(TVPWriteError);
	}

	tjs_uint64 TJS_INTF_METHOD GetSize() { return Size; }
};

size_t MaxStorageCacheSize = 200 * 1024 * 1024; // 200MB
size_t CurrentStorageCacheSize = 0;

int StorageCacheKeepTime = 30; // つかって30秒したら消去候補
int StorageCacheWaitTime = 3;  // キャッシュ再開待ちフレーム数

// 容量ネゴシエーション (doc/legacy/MemoryBudgetNegotiation.md §4)
// FileAllocator が capacity を返した場合のキャッシュ取り分・閾値・予備バイト数。
namespace {
	constexpr float kCacheShareRatio       = 0.5f;
	constexpr float kPressureSoftThreshold = 0.75f;
	constexpr float kPressureHardThreshold = 0.90f;
	constexpr size_t kReserveBytes         = 8 * 1024 * 1024; // 単発の大きなファイル用安全マージン
}
// IsOverMaxStorageCacheSize から available() を参照するための保持ポインタ。
// TVPNegotiateStorageCacheBudget で設定。alloc は g_FileAllocator と同寿命。
static iTVPMemoryAllocator *NegotiatedAllocator = nullptr;

typedef struct {
	std::shared_ptr<tTJSBinaryStreamBuffer> buffer;
	time_t lastaccess;
	int usecount;
	bool pinned;          // P2: pin (sticky) 化されたエントリは LRU/transient 駆逐対象外
} StorageCacheEntry;

// キャッシュデータ保持用テーブル
// キーは worker(キャッシュスレッド)が挿入し、main / pressure callback(任意スレッド)が
// 削除する越境データ。ttstr キーだと COW バッファ共有 + 非 atomic RefCount で
// 二重解放を起こすため、RefCount を持たない tjs_string をキーにする
// (doc/TtstrDataRetention.md H9)。boundary では AsStdString() で独立化する。
std::map<tjs_string, StorageCacheEntry> StorageCacheTable;
tTJSCriticalSection StorageCacheCS;

std::shared_ptr<tTJSBinaryStreamBuffer>
GetStreamBuffer(iTJSBinaryStream *stream)
{
	std::shared_ptr<tTJSBinaryStreamBuffer> result;

	size_t size = (size_t)stream->GetSize();
	auto buf = tTJSBinaryStreamBuffer::create(size);
	if (buf) {
		TVPReadBuffer(stream, (void*)buf->buffer(), buf->size());
		result.reset(buf);
	}
	return result;
}


extern iTJSBinaryStream * _InnerTVPCreateStream(const ttstr &name, tjs_uint32 flags);

// P4: Compact event hook の lazy 登録 (定義は後段)
static void EnsureStorageCacheCompactHookRegistered();

// キャッシュデータ登録。
// minSizeOverride > 0 ならその値を最小キャッシュサイズ閾値として優先使用、
// 0 なら拡張子別登録の閾値を参照する。
// 戻り値:
//   nullptr ... キャッシュに登録した。呼び出し元は StorageCacheTable から取得する。
//   非 nullptr ... 最小サイズ閾値以下だったためキャッシュせず open 済みストリームを
//                  そのまま返す。所有権は呼び出し元へ。
static iTJSBinaryStream* EntryStorageCache(const ttstr &name, tjs_uint64 minSizeOverride = 0)
{
	EnsureStorageCacheCompactHookRegistered(); // P4: lazy 登録
	tjs_uint64 minSize = minSizeOverride;
	if (minSize == 0) {
		// 拡張子別の最小キャッシュサイズ閾値を取得
		ttstr ext = TVPExtractStorageExt(name);
		TVPGetCacheTargetExtensionMinSize(ext, &minSize);
	}

	// open する前にサイズで早期判定 (TVPFileSizeStorage が使えるなら無駄な open を避ける)
	if (minSize > 0) {
		tjs_uint64 size = TVPFileSizeStorage(name);
		if (size > 0 && size > minSize) {
			TVPLOG_DEBUG("StorageCache:skip(over threshold pre-open):{} size={} threshold={}", name, size, minSize);
			return _InnerTVPCreateStream(name, TJS_BS_READ);
		}
	}

	iTJSBinaryStream *Stream = nullptr;
	try {
		Stream = _InnerTVPCreateStream(name, TJS_BS_READ);
		if (Stream) {
			if (minSize > 0 && Stream->GetSize() > minSize) {
				// fallback: TVPFileSizeStorage が 0 を返した場合の保険
				Stream->Seek(0, TJS_BS_SEEK_SET);
				TVPLOG_DEBUG("StorageCache:skip(over threshold):{}", name);
				return Stream;
			}
			StorageCacheEntry entry;
			entry.buffer = GetStreamBuffer(Stream);
			if (!entry.buffer) {
				// FileAllocator が確保失敗 (= 容量不足で file_malloc 二段リトライも
				// 通らなかった)。キャッシュ登録は諦めて Stream を rewind して返却。
				// 読み込み自体は続行可能 (doc/legacy/MemoryBudgetNegotiation.md §4.3)。
				TVPLOG_WARNING("StorageCache:entry:alloc failed, skip cache:{}", name);
				Stream->Seek(0, TJS_BS_SEEK_SET);
				return Stream;
			}
			entry.lastaccess = time(NULL);
			entry.usecount = 1;
			// P2: pin 集合に登録済みなら entry.pinned = true で初期化
			//    (pinCache → load の順でも、load → pinCache の順でも反映される)
			entry.pinned = TVPIsCachePathPinned(name);
			{
				tTJSCriticalSectionHolder Lock(StorageCacheCS);
				StorageCacheTable[name.AsStdString()] = entry;
			}
			CurrentStorageCacheSize += entry.buffer->size();
			TVPLOG_DEBUG("StorageCache:entry:{} size={}", name, entry.buffer->size());
			Stream->Destruct();
			Stream = NULL;
		}
	} catch(...) {
		if (Stream) {
			Stream->Destruct();
			Stream = NULL;
		}
		TVPLOG_ERROR("StorageCache:entry:failed:{}", name);
		throw;
	}
	return nullptr;
}

// キャッシュデータ存在チェック
bool TVPCheckStorageCache(const ttstr &name, bool update) 
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	//TVPLOG_DEBUG("StorageCache: check:{}", name);
	auto i = StorageCacheTable.find(name.AsStdString());
	if (i != StorageCacheTable.end()) {
		if (update) {
			i->second.lastaccess = time(NULL);
			i->second.usecount++;
		}
		return true;
	}
	return false;	
}

// TVPEntryStorageCacheで登録されたキャッシュデータを取得
iTJSBinaryStream *TVPGetStorageCache(const ttstr &_name, bool entry)
{
	ttstr name = TVPGetPlacedPath(_name);
	if(name.IsEmpty()) TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);

	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	auto i = StorageCacheTable.find(name.AsStdString());
	if (i == StorageCacheTable.end() && entry) {
		// キャッシュに無いので登録して取得
		iTJSBinaryStream *direct = EntryStorageCache(name);
		if (direct) {
			// 閾値以下のためキャッシュされなかった。open 済みストリームをそのまま返す
			return direct;
		}
		i = StorageCacheTable.find(name.AsStdString());
	}
	if (i != StorageCacheTable.end()) {
		i->second.lastaccess = time(NULL);
		i->second.usecount--;
		TVPLOG_DEBUG("StorageCache:get:{}", name);
		return new tTVPSharedMemoryStream(i->second.buffer);
	}
	return NULL;
}

// キャッシュエントリの buffer に他の保持者 (= cache 表以外の shared_ptr) が
// いるかどうか。font 系 _fontlist や archive handle pool 等が stream を介して
// shared_ptr を持ち続けている間は use_count() > 1 になる。
// この間は cache 表から駆逐しないことで「buffer alive ⇔ 表に出る」を維持する。
static inline bool TVPStorageCacheEntryReferencedExternally(const StorageCacheEntry &e) {
	return e.buffer.use_count() > 1;
}

// キャッシュを破棄。
// 「存在しない/解決できない path に対するクリア」は no-op 扱いとし、例外を
// 投げない (本来 file 層 cache の clear はベストエフォートで意味があり、
// ない物を clear しようとしてエラーにする必要はない)。
// 旧実装は TVPGetPlacedPath が空を返したら例外を投げていたが、
// 拡張子補完前の path や archive 内の path 等で誤発火するため撤去。
//
// force=false: 他の保持者がいる buffer は **clear をスキップ** する
//              (= 表に残し続ける、保持者が release した後の次の clear で駆逐)
// force=true:  保持者の有無を無視して表から削除 (書き込みの stale invalidate 用)
bool TVPClearStorageCache(const ttstr &_name, bool force)
{
	ttstr name = TVPGetPlacedPath(_name);
	if(name.IsEmpty()) return false;

	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	auto i = StorageCacheTable.find(name.AsStdString());
	if (i != StorageCacheTable.end()) {
		if (!force && TVPStorageCacheEntryReferencedExternally(i->second)) {
			TVPLOG_DEBUG("StorageCache:clearSkip:{} (referenced, use_count={})",
			             name, (int)i->second.buffer.use_count());
			return false;
		}
		CurrentStorageCacheSize -= i->second.buffer->size();
		StorageCacheTable.erase(i);
		TVPLOG_DEBUG("StorageCache:clear:{}{}", name, force ? " (forced)" : "");
		return true;
	}
	return false;
}

// 古いキャッシュを破棄
//
// 旧実装は `for (auto it : StorageCacheTable)` で entry を値コピーしており、
// コピー側 shared_ptr が use_count を +1 する結果 TVPStorageCacheEntryReferencedExternally
// が常に true を返し、駆逐が一切起きていなかった (= LRU 経路が事実上無効化)。
// あわせて range-for 中の erase は隠れ iterator を無効化する UB。
// 明示 iterator + it = .erase(it) パターンに揃え、外部参照判定は it->second
// (参照) で行う。
void TVPClearOldStorageCache(int keepTime, bool force)
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	size_t dropped = 0;
	size_t dropped_bytes = 0;
	time_t cutoff = time(NULL) - keepTime;
	for (auto it = StorageCacheTable.begin(); it != StorageCacheTable.end(); ) {
		if (it->second.pinned) { ++it; continue; } // P2: pinned はスキップ
		if (TVPStorageCacheEntryReferencedExternally(it->second)) { ++it; continue; } // 他に参照者あり
		if (it->second.lastaccess < cutoff && (force || it->second.usecount <= 0)) {
			TVPLOG_DEBUG("StorageCache:clearOld:drop:{} size={} keepTime={} force={}",
			             it->first, it->second.buffer->size(), keepTime, (int)force);
			dropped_bytes += it->second.buffer->size();
			CurrentStorageCacheSize -= it->second.buffer->size();
			it = StorageCacheTable.erase(it);
			++dropped;
		} else {
			++it;
		}
	}
	if (dropped > 0) {
		TVPLOG_DEBUG("StorageCache:clearOld: dropped={} bytes={} remaining={}",
		             dropped, dropped_bytes, StorageCacheTable.size());
	}
}

// 全キャッシュ消去 (pinned 含めて全部、ただし外部参照中は残す)
// MAX Compact / シャットダウン相当だが、font / archive pool 等が stream を
// 持ち続けている間は表から外さない (= 表内容 = 生存 buffer)。
void TVPClearAllStorageCache()
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	size_t dropped = 0, kept = 0;
	size_t dropped_bytes = 0;
	for (auto it = StorageCacheTable.begin(); it != StorageCacheTable.end(); ) {
		if (TVPStorageCacheEntryReferencedExternally(it->second)) {
			++kept;
			++it;
		} else {
			dropped_bytes += it->second.buffer->size();
			CurrentStorageCacheSize -= it->second.buffer->size();
			it = StorageCacheTable.erase(it);
			++dropped;
		}
	}
	if (dropped > 0 || kept > 0) {
		TVPLOG_DEBUG("StorageCache:clearAll: dropped={} bytes={} kept(referenced)={}",
		             dropped, dropped_bytes, kept);
	}
}

// transient (pinned 以外、外部参照中も残す) 全消し。
void TVPClearTransientStorageCache()
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	size_t dropped = 0, dropped_bytes = 0, kept_pinned = 0, kept_ref = 0;
	for (auto it = StorageCacheTable.begin(); it != StorageCacheTable.end(); ) {
		if (it->second.pinned) {
			++kept_pinned;
			++it;
		} else if (TVPStorageCacheEntryReferencedExternally(it->second)) {
			++kept_ref;
			++it;
		} else {
			dropped_bytes += it->second.buffer->size();
			CurrentStorageCacheSize -= it->second.buffer->size();
			it = StorageCacheTable.erase(it);
			++dropped;
		}
	}
	if (dropped > 0 || kept_pinned > 0 || kept_ref > 0) {
		TVPLOG_DEBUG("StorageCache:clearTransient: dropped={} bytes={} kept(pinned)={} kept(referenced)={}",
		             dropped, dropped_bytes, kept_pinned, kept_ref);
	}
}

// pinned 状態を変更。entry が無くても何もしない (load 時に集合参照で初期化される)。
void TVPSetStorageCacheEntryPinned(const ttstr &name, bool pinned)
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	auto i = StorageCacheTable.find(name.AsStdString());
	if (i != StorageCacheTable.end()) {
		if (i->second.pinned != pinned) {
			TVPLOG_DEBUG("StorageCache:pin:{}={}", name, pinned ? "true" : "false");
		}
		i->second.pinned = pinned;
	}
}

//---------------------------------------------------------------------------
// P4: Compact event 連動。doc/legacy/ImagePreloadAndCache.md §18.2 C
//   IDLE / DEACTIVATE: 何もしない
//   MINIMIZE: transient 全消し (pinned 残す)
//   MAX:      pinned 含めて全消し
//---------------------------------------------------------------------------
struct tTVPClearStorageCacheCallback : public tTVPCompactEventCallbackIntf
{
	virtual void TJS_INTF_METHOD OnCompact(tjs_int level)
	{
		if(level >= TVP_COMPACT_LEVEL_MAX) {
			TVPLOG_DEBUG("StorageCache:compact level={} -> clearAll", (int)level);
			TVPClearAllStorageCache();
		} else if(level >= TVP_COMPACT_LEVEL_MINIMIZE) {
			TVPLOG_DEBUG("StorageCache:compact level={} -> clearTransient", (int)level);
			TVPClearTransientStorageCache();
		}
	}
} static TVPClearStorageCacheCallback;

// CompactEventHook の lazy 登録。EntryStorageCache は main thread と
// tTVPStorageCacheThread::Execute の両方から呼ばれうるので、bool フラグだけでは
// 初回呼び出しが同時に走った場合に二重登録される。std::call_once で塞ぐ。
static void EnsureStorageCacheCompactHookRegistered()
{
	static std::once_flag flag;
	std::call_once(flag, []{
		TVPAddCompactEventHook(&TVPClearStorageCacheCallback);
	});
}

// キャッシュサイズを返す
size_t TVPGetStorageCacheSize()
{
	return CurrentStorageCacheSize;
}

// 現在のキャッシュエントリ全件を列挙してコピー。
void TVPGetStorageCacheEntries(std::vector<TVPStorageCacheEntryInfo> &out)
{
	out.clear();
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	out.reserve(StorageCacheTable.size());
	for (auto &it : StorageCacheTable) {
		TVPStorageCacheEntryInfo info;
		// it.first は tjs_string。observation 用 ttstr へは独立コピーで変換。
		info.name       = ttstr(it.first);
		info.size       = it.second.buffer ? it.second.buffer->size() : 0;
		info.lastaccess = it.second.lastaccess;
		info.usecount   = it.second.usecount;
		info.pinned     = it.second.pinned;
		out.push_back(info);
	}
}

// 件数のみ取得 (entries 列挙よりロック保持時間を短くしたい観測用途)。
void TVPGetStorageCacheCount(size_t &total, size_t &pinned)
{
	tTJSCriticalSectionHolder Lock(StorageCacheCS);
	total = StorageCacheTable.size();
	pinned = 0;
	for (auto &it : StorageCacheTable) {
		if (it.second.pinned) ++pinned;
	}
}

// file 層キャッシュ一覧を WARNING ログに出力。
// 1 行目: サマリ (件数 + pinned 数 + 総バイト)
// 続く各行: per-entry 詳細 (path / size / usecount / pinned 印)
void TVPDumpFileCacheList()
{
	std::vector<TVPStorageCacheEntryInfo> entries;
	TVPGetStorageCacheEntries(entries);
	tjs_uint64 total_bytes = 0;
	size_t pinned_count = 0;
	for (auto &e : entries) {
		total_bytes += e.size;
		if (e.pinned) ++pinned_count;
	}
	tjs_char buf[64];
	{
		ttstr msg = TJS_W("FileCache: ");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%zu"), entries.size());
		msg += buf;
		msg += TJS_W(" entries (pinned=");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%zu"), pinned_count);
		msg += buf;
		msg += TJS_W(", totalBytes=");
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char), TJS_W("%llu"),
		             (unsigned long long)total_bytes);
		msg += buf;
		msg += TJS_W(")");
		TVPAddImportantLog(msg);
	}
	for (auto &e : entries) {
		ttstr msg = e.pinned ? TJS_W("  [pin] ") : TJS_W("        ");
		msg += e.name;
		TJS_snprintf(buf, sizeof(buf)/sizeof(tjs_char),
		             TJS_W(" size=%zu use=%d"),
		             e.size, e.usecount);
		msg += buf;
		TVPAddImportantLog(msg);
	}
}

// キャッシュサイズを設定
void TVPSetMaxStorageCacheSize(size_t size)
{
	MaxStorageCacheSize = size;
}

// キャッシュサイズが規定を超えているかどうか
bool IsOverMaxStorageCacheSize()
{
	if (TVPGetStorageCacheSize() > MaxStorageCacheSize) return true;
	// FileAllocator が capacity を申告している場合は、残量が予備バイト数を
	// 下回ったら過負荷とみなす (doc/legacy/MemoryBudgetNegotiation.md §4.2)。
	if (NegotiatedAllocator) {
		size_t avail = NegotiatedAllocator->available();
		if (avail != SIZE_MAX && avail < kReserveBytes) return true;
	}
	return false;
}

// FileAllocator 容量ネゴシエーション (doc/legacy/MemoryBudgetNegotiation.md §4.1)
//
// 旧仕様では `cap * kCacheShareRatio (= 0.5)` を MaxStorageCacheSize として
// 設定していたが、refcount-aware eviction (commit 59fa472d) で外部参照中の
// 駆逐不可 entry も `CurrentStorageCacheSize` に積まれるようになった結果、
// font/archive pool 等の保持で常時 over max 判定 → cache thread back-pressure
// が空振りする状況が発生。
//
// FileAllocator pool は実質 StorageCache 専用 (= tag[FileCache] 1 種のみ
// 経由) なので半分残す意味がない。MaxStorageCacheSize = cap で揃え、
// 「pool が物理的に枯渇しそうな時だけ back-pressure」とする。
// 駆逐不可で over max の場合 thread 側で待っても無意味なので、
// 真に枯渇しない限り wait しない方針に。
void TVPNegotiateStorageCacheBudget(iTVPMemoryAllocator *alloc)
{
	if (!alloc) return;
	NegotiatedAllocator = alloc;

	size_t cap = alloc->capacity();
	if (cap != SIZE_MAX) {
		TVPLOG_INFO("StorageCache: budget set to FileAllocator capacity={}MB",
			cap / (1024 * 1024));
		MaxStorageCacheSize = cap;
	}

	// 0.75 を超えたら未使用エントリを駆逐、0.90 を超えたら使用中含めて駆逐。
	// 任意スレッドから呼ばれうる (TVPClearOldStorageCache 内で StorageCacheCS を
	// 取るのでスレッド安全)。callback 内から allocate / free を呼ばないこと。
	alloc->setPressureCallback([](float pressure) {
		if (pressure >= kPressureHardThreshold) {
			TVPClearOldStorageCache(0, /*force=*/true);
		} else if (pressure >= kPressureSoftThreshold) {
			TVPClearOldStorageCache(0, /*force=*/false);
		}
	});
}


//---------------------------------------------------------------------------

tTVPStorageCacheThread::tTVPStorageCacheThread()
 : tTVPThread("StorageCacheThread"), loading(false), loadingFast(false), waitTick(0), prevTick(0)
{
}

tTVPStorageCacheThread::~tTVPStorageCacheThread() 
{
	ExitRequest();
	WaitFor();
	RequestQueue.clear();
	RequestQueueFast.clear();
}

void tTVPStorageCacheThread::ExitRequest() 
{
	Terminate();
	RequestQueueEvent.Set();
}

void tTVPStorageCacheThread::Execute() 
{
	// プライオリティは最低にする
	SetPriority(ttpIdle);

	while( !GetTerminated() ) {
		// キュー追加イベント待ち
		RequestQueueEvent.WaitFor(0);
		if( GetTerminated() ) break;
		do {
			{
				tTJSCriticalSectionHolder cs(RequestQueueCS);
				loading = false;
				loadingFast = false;
			}

			bool hasFastQueue = false;
			{
				tTJSCriticalSectionHolder cs(RequestQueueCS);
				hasFastQueue = RequestQueueFast.size() > 0;
				loadingFast = hasFastQueue;
			}
			if ( hasFastQueue ) {
				LoadRequestItem item;
				bool got = false;
				{
					tTJSCriticalSectionHolder cs(RequestQueueCS);
					if( RequestQueueFast.size() ) {
						item = RequestQueueFast.front();
						RequestQueueFast.pop_front();
						got = true;
					}
				}
				if (got) {
					// item.name は tjs_string。worker 上で独立した ttstr を一度だけ
					// 作って各 API へ渡し、ttstr バッファを他スレッドと共有させない。
					ttstr n(item.name);
					if (TVPIsCacheTargetFile(n) && !TVPCheckStorageCache(n, true)) {
						iTJSBinaryStream *direct = EntryStorageCache(n, item.minSize);
						if (direct) direct->Destruct(); // 閾値以下でキャッシュされなかったぶんは破棄
					}
				}
			}

			bool hasNormalQueue = false;
			{
				tTJSCriticalSectionHolder cs(RequestQueueCS);
				hasNormalQueue = RequestQueue.size() > 0;
				loading = hasNormalQueue;
			}
			if ( hasNormalQueue ) {

				// 待ち処理
				if (waitTick > 0) {
					uint64_t tick = TVPGetTickCount();
					uint64_t diff = tick - prevTick;
					prevTick = tick;
					if (diff < waitTick) {
						waitTick -= diff;
					} else {
						waitTick = 0;
					}
				}

				if (waitTick == 0) {
					// 空きが無い場合はすこし待つ
					if (IsOverMaxStorageCacheSize()) {
						TVPLOG_INFO("StorageCache: over max size, wait {} sec", StorageCacheWaitTime);
						// 一定以上古い利用済みファイルを破棄
						// TVPClearOldStorageCache の keepTime は time(NULL) との
						// 差分で比較されるため秒単位 (旧 `* 1000` は ms 単位と
						// 取り違えており、~8 時間以上経過した entry しか駆逐
						// 対象にならない誤り)。
						TVPClearOldStorageCache(StorageCacheKeepTime);
						prevTick = TVPGetTickCount();
						waitTick = StorageCacheWaitTime * 1000;
					} else {
						LoadRequestItem item;
						bool got = false;
						{
							tTJSCriticalSectionHolder cs(RequestQueueCS);
							if (RequestQueue.size()) {
								item = RequestQueue.front();
								RequestQueue.pop_front();
								got = true;
							}
						}
						if (got) {
							// item.name は tjs_string。worker 上で独立した ttstr を一度
							// だけ作って各 API へ渡し、バッファを他スレッドと共有させない。
							ttstr n(item.name);
							if (TVPIsCacheTargetFile(n) && !TVPCheckStorageCache(n, true)) {
								iTJSBinaryStream *direct = EntryStorageCache(n, item.minSize);
								if (direct) direct->Destruct(); // 閾値以下でキャッシュされなかったぶんは破棄
							}
						}
					}
				}
			}

			// 適宜処理移管
			TVPYieldNativeThread();

		} while( (loading || loadingFast) && !GetTerminated() );
	}
}

//---------------------------------------------------------------------------

// onLoaded( dic, is_async, is_error, error_mes ); エラーは
// sync ( main thead )
void tTVPStorageCacheThread::LoadRequest( const ttstr &_name, bool fast, tjs_uint64 minSize ) {
	ttstr name = TVPGetPlacedPath(_name); // file must exist
	if (name.IsEmpty()) {
		TVPThrowExceptionMessage(TVPCannotOpenStorage, _name);
	}
	// StorageCache only handles file:// scheme; non-cacheable schemes
	// (e.g. plugin-provided custom media) are skipped silently by the
	// background worker. Warn the caller here so a misuse of
	// Storages.requestCache() against an unsupported target is visible.
	if (!TVPIsCacheTargetFile(name)) {
		TVPLOG_WARNING("StorageCache:requestCache: skipping non-cacheable target: {} (only file:// scheme is cacheable)", _name);
		return;
	}
	// name は tjs_string として保持し、キャッシュスレッドと ttstr バッファを
	// 共有させない (独立化)。
	LoadRequestItem item{ name.AsStdString(), minSize };
	if (fast) {
		tTJSCriticalSectionHolder cs(RequestQueueCS);
		RequestQueueFast.push_back(item);
		RequestQueueEvent.Set();
	} else {
		tTJSCriticalSectionHolder cs(RequestQueueCS);
		RequestQueue.push_back(item);
		RequestQueueEvent.Set();
	}
}


void tTVPStorageCacheThread::CancelLoadQueue( const ttstr &name ) 
{
	// キューをロックしてキャンセル。it->name は tjs_string なので同型で比較する。
	tjs_string nname = name.AsStdString();
	tTJSCriticalSectionHolder cs(RequestQueueCS);
	for (auto it=RequestQueue.begin(); it!=RequestQueue.end();) {
		if (it->name == nname) {
			it = RequestQueue.erase(it);
		} else {
			++it;
		}
	}
	for (auto it=RequestQueueFast.begin(); it!=RequestQueueFast.end();) {
		if (it->name == nname) {
			it = RequestQueueFast.erase(it);
		} else {
			++it;
		}
	}
}

void tTVPStorageCacheThread::CancelAllQueue()
{
	// キューをロックしてキャンセル
	tTJSCriticalSectionHolder cs(RequestQueueCS);
	RequestQueue.clear();
	RequestQueueFast.clear();
}

void tTVPStorageCacheThread::ClearCache( const ttstr &name ) 
{
	if (name != TJS_W("")) {
		TVPClearStorageCache(name);
		CancelLoadQueue(name);
	} else {
		TVPClearAllStorageCache();
		CancelAllQueue();
	}
}

bool tTVPStorageCacheThread::IsLoading(bool fast)
{
	tTJSCriticalSectionHolder Lock(RequestQueueCS);
	int count;
	bool ret;
	if (fast) {
		count = RequestQueueFast.size(); 
		ret = count  > 0 || loadingFast;
 	} else {
		count = RequestQueue.size(); 
		ret = count  > 0 || loading;
	} 
	return ret;
}