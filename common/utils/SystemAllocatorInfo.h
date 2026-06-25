#pragma once

// システムアロケータからの一般的なメモリ情報を取得するインターフェース。
// コンソール機等のプラットフォームアロケータが提供する以下のような機能を
// 抽象化し、一般的な OS 環境でも可能な範囲で情報を提供する:
//   - GetTotalFreeSize() — アロケータに存在する空き領域の合計
//   - GetAllocatableSize() — アロケータから確保可能な最大サイズ
//   - Dump() — アロケータ内部の情報を表示
//
// デフォルト実装 (tTVPDefaultSystemAllocatorInfo) は一般 OS 向けで、
// malloc/free が使える環境で OS から取得可能な情報を返す。
// 組込みプラットフォーム固有実装は、Application を継承したクラスで
// GetSystemAllocatorInfo() をオーバーライドして独自実装を返す。
//
// doc/MemoryDesign.md も参照のこと。

#include <cstddef>
#include <cstdint>
#include <string>

// システムアロケータ情報構造体。
// SIZE_MAX は「情報が取得できない / 未対応」を示す。
struct TVPSystemAllocatorStats {
	size_t total_size       = SIZE_MAX; // アロケータの総容量
	size_t total_free_size  = SIZE_MAX; // 空き領域の合計 (GetTotalFreeSize 相当)
	size_t allocatable_size = SIZE_MAX; // 確保可能な最大サイズ (GetAllocatableSize 相当)
	                                    // ※フラグメンテーションにより total_free_size より
	                                    //   小さくなる場合がある
	size_t used_size        = SIZE_MAX; // 使用中のサイズ
	size_t peak_used_size   = SIZE_MAX; // ピーク使用量

	// プロセスメモリ情報 (OSレベル)
	size_t process_rss      = SIZE_MAX; // Resident Set Size (物理常駐サイズ)
	size_t process_peak_rss = SIZE_MAX; // ピーク RSS
	size_t process_vsize    = SIZE_MAX; // Virtual Memory Size

	// システム全体のメモリ情報
	size_t system_total_physical = SIZE_MAX; // システム全体の物理メモリ
	size_t system_avail_physical = SIZE_MAX; // システムで利用可能な物理メモリ
};

// システムアロケータ情報インターフェース。
// コンソール機等のプラットフォームアロケータが提供する機能を抽象化。
class iTVPSystemAllocatorInfo {
public:
	virtual ~iTVPSystemAllocatorInfo() = default;

	// ----------------------------------------------------------------
	// プラットフォームアロケータ互換メソッド
	// ----------------------------------------------------------------

	// アロケータに存在する空き領域の合計を取得。
	// フラグメンテーションを考慮しない単純な空き容量。
	// SIZE_MAX = 情報が取得できない。
	virtual size_t GetTotalFreeSize() const noexcept = 0;

	// アロケータから確保可能な最大サイズを取得。
	// 連続した空き領域の最大値。フラグメンテーションにより
	// GetTotalFreeSize() より小さくなる場合がある。
	// SIZE_MAX = 情報が取得できない。
	virtual size_t GetAllocatableSize() const noexcept = 0;

	// アロケータ内部の情報をログに表示。
	// TVPHeapDump 等から呼び出される。
	// (ログフォーマット内で std::string allocation を行うため noexcept ではない)
	virtual void Dump() const = 0;

	// ----------------------------------------------------------------
	// 拡張メソッド
	// ----------------------------------------------------------------

	// すべての統計情報を一括で取得。
	// (TVPGetProcessMemoryInfo 内部で fopen/std::string を使うため noexcept ではない)
	virtual TVPSystemAllocatorStats GetStats() const = 0;

	// 1 行サマリ文字列を取得 (REPL / オーバレイ表示用)。
	virtual std::string GetSummary() const = 0;
};

// 一般 OS 向けのデフォルト実装。
// malloc/free が使える環境で、OS から取得可能な情報を返す。
// GetTotalFreeSize() / GetAllocatableSize() は一般的なデスクトップ OS では
// 正確な値が取得できないため、システムの空き物理メモリを近似値として返す。
class tTVPDefaultSystemAllocatorInfo : public iTVPSystemAllocatorInfo {
public:
	tTVPDefaultSystemAllocatorInfo() = default;
	~tTVPDefaultSystemAllocatorInfo() override = default;

	size_t GetTotalFreeSize() const noexcept override;
	size_t GetAllocatableSize() const noexcept override;
	void Dump() const override;
	TVPSystemAllocatorStats GetStats() const override;
	std::string GetSummary() const override;
};

// システムアロケータ情報を取得する。
// Application が存在すれば Application->GetSystemAllocatorInfo() に
// delegate するので、プラットフォーム固有 override (Switch の
// NXSystemAllocatorInfo 等) がそのまま反映される。
// Application 初期化前 / 終了後はデフォルト実装にフォールバック。
iTVPSystemAllocatorInfo* TVPGetSystemAllocatorInfo();

// デフォルト実装 (一般 OS 向け) のグローバル singleton を取得。
// 主に tTVPApplication::GetSystemAllocatorInfo() の base 実装で使う
// (TVPGetSystemAllocatorInfo() を呼ぶと無限再帰するため、こちらを使う)。
iTVPSystemAllocatorInfo* TVPGetDefaultSystemAllocatorInfo();

// システムアロケータ情報をログにダンプ (TVPHeapDump から呼ばれる)。
void TVPDumpSystemAllocatorInfo();

// 1 行サマリを返す (REPL 等用)。
std::string TVPSummarizeSystemAllocatorInfo();
