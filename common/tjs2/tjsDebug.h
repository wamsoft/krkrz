//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// TJS Debugging support
//---------------------------------------------------------------------------
#ifndef tjsDebugH
#define tjsDebugH

#include "tjsString.h"
#include "tjsVariant.h"

#include <list>
#include <string>
#include <vector>

namespace TJS
{

struct ScopeKey {
	int ClassIndex;	//!< クラス名インデックス
	int FuncIndex;	//!< 関数名インデックス
	int FileIndex;	//!< ファイル名インデックス
	int CodeOffset;	//!< VM コードオフセット

	ScopeKey()
	: ClassIndex(-1), FuncIndex(-1), FileIndex(-1), CodeOffset(-1)
	{}
	ScopeKey( int classidx, int func, int file, int codeoffset )
	: ClassIndex(classidx), FuncIndex(func), FileIndex(file), CodeOffset(codeoffset)
	{}
	void Set( int classidx, int func, int file, int codeoffset ) {
		ClassIndex = classidx;
		FuncIndex = func;
		FileIndex = file;
		CodeOffset = codeoffset;
	}

	bool operator ==( const ScopeKey& rhs ) const {
		return( ClassIndex == rhs.ClassIndex && FuncIndex == rhs.FuncIndex && FileIndex == rhs.FileIndex && CodeOffset == rhs.CodeOffset );
	}
	bool operator !=( const ScopeKey& rhs ) const {
		return( ClassIndex != rhs.ClassIndex || FuncIndex != rhs.FuncIndex || FileIndex != rhs.FileIndex || CodeOffset != rhs.CodeOffset );
	}
	bool operator < ( const ScopeKey& rhs ) const {
		// クラス、関数名
		if( ClassIndex == rhs.ClassIndex ) {
			if( FuncIndex == rhs.FuncIndex ) {
				if( FileIndex == rhs.FileIndex ) {
					return CodeOffset < rhs.CodeOffset;
				} else {
					return FileIndex < rhs.FileIndex;
				}
			} else {
				return FuncIndex < rhs.FuncIndex;
			}
		} else {
			return ClassIndex < rhs.ClassIndex;
		}
	}
};

//---------------------------------------------------------------------------
// ObjectHashMap : hash map to track object construction/destruction
//---------------------------------------------------------------------------
// object hash map flags
#define TJS_OHMF_EXIST        1  // The object is in object hash map
#define TJS_OHMF_INVALIDATED  2  // The object had been invalidated  // currently not used
#define TJS_OHMF_DELETING     4  // The object is now being deleted
#define TJS_OHMF_SET          (~0)
#define TJS_OHMF_UNSET        (0)
//---------------------------------------------------------------------------
class tTJSScriptBlock;
struct tTJSObjectHashMapRecord;

class tTJSObjectHashMap;
extern tTJSObjectHashMap * TJSObjectHashMap;
extern iTJSBinaryStream * TJSObjectHashMapLog;
extern void TJSAddRefObjectHashMap();
extern void TJSReleaseObjectHashMap();
extern void TJSAddObjectHashRecord(void * object);
extern void TJSRemoveObjectHashRecord(void * object);
extern void TJSObjectHashSetType(void * object, const ttstr &type);
extern void TJSSetObjectHashFlag(void * object, tjs_uint32 flags_to_change, tjs_uint32 bits);
extern void TJSReportAllUnfreedObjects(iTJSConsoleOutput * output);
extern bool TJSObjectHashAnyUnfreed();
extern void TJSObjectHashMapSetLog(iTJSBinaryStream * stream);
extern void TJSWriteAllUnfreedObjectsToLog();
extern void TJSWarnIfObjectIsDeleting(iTJSConsoleOutput * output, void * object);
extern void TJSReplayObjectHashMapLog();
static inline bool TJSObjectHashMapEnabled() { return TJSObjectHashMap || TJSObjectHashMapLog; }
extern inline bool TJSObjectTypeInfoEnabled() { return 0!=TJSObjectHashMap; }
extern inline bool TJSObjectFlagEnabled() { return 0!=TJSObjectHashMap; }
extern ttstr TJSGetObjectTypeInfo(void * object);
extern tjs_uint32 TJSGetObjectHashCheckFlag(void * object);
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// StackTracer : stack to trace function call trace
//---------------------------------------------------------------------------
class tTJSStackTracer;
class tTJSInterCodeContext;
extern tTJSStackTracer * TJSStackTracer;
extern void TJSAddRefStackTracer();
extern void TJSReleaseStackTracer();
extern void TJSStackTracerPush(tTJSInterCodeContext *context, bool in_try);
extern void TJSStackTracerSetCodePointer(const tjs_int32 * codebase, tjs_int32 * const * codeptr);
extern void TJSStackTracerPop();
extern ttstr TJSGetStackTraceString(tjs_int limit = 0, const tjs_char *delimiter = NULL);
static inline bool TJSStackTracerEnabled() { return 0!=TJSStackTracer; }
//---------------------------------------------------------------------------

// Debugger hook (Phase 1: stub. tjsDebuggerHook.cpp で no-op 実装)
// Phase 2 以降: DAP server の DebuggerCore に転送される。
// exception パラメータは EXCEPT hook 時に投げられた値 (取れる場合)、それ以外 NULL。
extern void TJSDebuggerHook( tjs_int evtype, const tjs_char *filename, tjs_int lineno, tTJSInterCodeContext* ctx = NULL, tTJSVariant* exception = NULL );
extern void TJSDebuggerLog( const ttstr &line, bool impotant );

// Debugger symbol tables (tjsDebuggerSymbols.cpp で実装)
// コンパイル時に名前→id とローカル/クラス変数のレジスタ位置情報を蓄積する。
extern void TJSDebuggerGetScopeKey( struct ScopeKey& scope,  const tjs_char* classname, const tjs_char* funcname, const tjs_char* filename, int codeoffset );
extern void TJSDebuggerAddLocalVariable( const struct ScopeKey& key, const tjs_char* varname, int regaddr );
extern void TJSDebuggerAddLocalVariable( const tjs_char* filename, const tjs_char* classname, const tjs_char* funcname, int codeoffset, const tjs_char* varname, int regaddr );
extern void TJSDebuggerGetLocalVariableString( const struct ScopeKey& key, tTJSVariant* ra, std::list<tjs_string>& values );
extern void TJSDebuggerGetLocalVariableString( const tjs_char* filename, const tjs_char* classname, const tjs_char* funcname, int codeoffset, tTJSVariant* ra, std::list<tjs_string>& values );
extern void TJSDebuggerClearLocalVariable( const ScopeKey& key );
extern void TJSDebuggerClearLocalVariable( const tjs_char* classname, const tjs_char* funcname, const tjs_char* filename, int codeoffset );

extern void TJSDebuggerAddClassVariable( const tjs_char* classname, const tjs_char* varname, int regaddr );
extern void TJSDebuggerGetClassVariableString( const tjs_char* classname, tTJSVariant* ra, tTJSVariant* da, std::list<tjs_string>& values );
extern void TJSDebuggerClearLocalVariable( const tjs_char* classname );

// variant ベース版 (Phase 5a 追加): 子オブジェクト展開のため値そのものを返す。
struct TJSDebuggerVar {
	tjs_string  name;
	tTJSVariant value;
};
extern void TJSDebuggerGetLocalVariables( const struct ScopeKey& key, tTJSVariant* ra, std::vector<TJSDebuggerVar>& out );
extern void TJSDebuggerGetClassVariables( const tjs_char* classname, tTJSVariant* ra, tTJSVariant* da, std::vector<TJSDebuggerVar>& out );

// マルチフレーム stack trace 用 (Phase 5b 追加): StackTracer の Stack を
// 構造化して返す。InTry frame (try/catch 実装上の人工 frame) はスキップ。
struct TJSStackFrame {
	tTJSInterCodeContext* ctx;       //!< scope/variable 引きに使う
	tjs_string            filename;  //!< Block->GetName() (TJS 内部パス)
	tjs_string            funcname;  //!< "ClassName.funcName" / "funcName" / "(top)"
	int                   line;      //!< 1-based source line, -1 if unknown
};
extern void TJSGetStackTraceFrames( std::vector<TJSStackFrame>& out );

} // namespace TJS

#endif
