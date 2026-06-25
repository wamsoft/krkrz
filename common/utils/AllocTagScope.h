#pragma once
#include <cstdint>

// Krkrz allocator (operator new override + TJS_malloc redirect) の確保を
// 用途別に振り分けるための thread-local tag スタック。
//
// 使い方:
//   void doSomething() {
//       TVPAllocTagScope guard{TVPAllocTag::TJS2};
//       // ここで起きる operator new / TJS_malloc は tag=TJS2 として集計
//       script->Execute();
//   }
// (RAII で push/pop。scope を抜けると元の tag に戻る)
//
// TJS API 経路 (System.beginAllocTag / endAllocTag) では手動 push/pop を使う。
//
// 設計上の注意:
//   - thread_local stack なので、startup の static initializer 経由の alloc は
//     Unknown tag に流れる (これは意図通り)。
//   - スタック深さは固定 (16)。overflow したら警告 1 回出して tag を更新しない。
//   - free 時の tag は Header から回収するので、ここの thread-local とは独立。
//
// KRKRZ_ENABLE_MEMSTAT_DETAIL 未定義時は本ヘッダの全 API が inline 空 (no-op)
// に置換され、scope ctor/dtor も thread-local も触らない。利用側 (event
// dispatch / TJS executor / image decoder) のコードはそのままで OK、
// コンパイラが死コードとして除去する。

// Forward declare. Full enum は Application.h (generic/win32 両方の).
enum class TVPAllocTag : uint16_t;

#ifdef KRKRZ_ENABLE_MEMSTAT_DETAIL

// RAII ガード。
class TVPAllocTagScope {
public:
	explicit TVPAllocTagScope(TVPAllocTag tag) noexcept;
	// 文字列版: TVPAllocTagFromName(name) で enum 解決。
	// Application.h を include したくない場所 (common/tjs2/ 等) から使う。
	explicit TVPAllocTagScope(const char *tag_name) noexcept;
	~TVPAllocTagScope() noexcept;
	TVPAllocTagScope(const TVPAllocTagScope&) = delete;
	TVPAllocTagScope& operator=(const TVPAllocTagScope&) = delete;
private:
	bool pushed_;  // overflow 時は false で dtor が pop しない
};

// 現在 thread の tag stack top。空 (or overflow) なら Unknown。
TVPAllocTag TVPCurrentAllocTag() noexcept;

// TJS API / 動的押し下げ用。手動 push/pop。
// pop が unbalanced (空 stack に対して呼ばれる) ならログ警告のみ。
void TVPPushAllocTag(TVPAllocTag tag) noexcept;
void TVPPopAllocTag() noexcept;

// 名前 → enum (例: "TJS2" → TVPAllocTag::TJS2)。
// 一致しない or NULL なら TVPAllocTag::User。
TVPAllocTag TVPAllocTagFromName(const char *name) noexcept;

#else // !KRKRZ_ENABLE_MEMSTAT_DETAIL

// no-op stubs。コンパイラが完全に除去できるよう、すべて inline かつ空。
class TVPAllocTagScope {
public:
	explicit TVPAllocTagScope(TVPAllocTag) noexcept {}
	explicit TVPAllocTagScope(const char *) noexcept {}
	~TVPAllocTagScope() noexcept = default;
	TVPAllocTagScope(const TVPAllocTagScope&) = delete;
	TVPAllocTagScope& operator=(const TVPAllocTagScope&) = delete;
};

// 値を必要とする場面でも常に Unknown を返すことで、operator new override の
// per-tag accounting も TagSlot[Unknown] にすら入らないようにしたいので、
// 実装側 (GlobalAllocStats.cpp) で本関数を呼ぶ前段に同じ ifdef を被せている。
inline TVPAllocTag TVPCurrentAllocTag() noexcept {
	return static_cast<TVPAllocTag>(0); // = TVPAllocTag::Unknown
}
inline void TVPPushAllocTag(TVPAllocTag) noexcept {}
inline void TVPPopAllocTag() noexcept {}
inline TVPAllocTag TVPAllocTagFromName(const char *) noexcept {
	return static_cast<TVPAllocTag>(8); // = TVPAllocTag::User (一致しないとき相当)
}

#endif // KRKRZ_ENABLE_MEMSTAT_DETAIL
