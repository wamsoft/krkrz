//---------------------------------------------------------------------------
//!@file UserConfig 用オプション記述データ構造とローダ宣言
//
// 元々 win32/msg/ReadOptionDesc.h にあった型定義を共通化したもの。
// パーサとマージは common/msg/ReadOptionDescUtil.cpp で実装。
// ローダ (TVPGetEngineCommandDesc / TVPGetPluginCommandDesc) はプラットフォーム
// ごとに実装が異なる:
//   Win32  : win32/msg/ReadOptionDesc.cpp (PE リソース)
//   SDL3   : generic/msg/ReadOptionDesc.cpp (resource/ 配下の JSON ファイル)
//---------------------------------------------------------------------------
#ifndef __READ_OPTION_DESC_H__
#define __READ_OPTION_DESC_H__

#include "tjsCommHead.h"
#include <vector>
#include <string>

struct tTVPCommandOptionsValue {
	tjs_string Value;
	tjs_string Description;
	bool IsDefault;
};
struct tTVPCommandOption {
	enum ValueType {
		VT_Select,
		VT_String,
		VT_Unknown
	};
	tjs_string Caption;
	tjs_string Description;
	tjs_string Name;
	ValueType Type;
	tjs_int Length;
	tjs_string Value;
	std::vector<tTVPCommandOptionsValue> Values;
	bool User;
};
struct tTVPCommandOptionCategory {
	tjs_string Name;
	std::vector<tTVPCommandOption> Options;
};
struct tTVPCommandOptionList {
	std::vector<tTVPCommandOptionCategory> Categories;
};

//! @brief プラットフォーム共通: JSON バッファをパースして option list を返す。
//!        失敗時は nullptr。caller が delete する。
extern tTVPCommandOptionList* TVPParseCommandDescJson(const char* buf, size_t size);

//! @brief プラットフォーム共通: option list を merge する (同名 category は統合)。
extern void TVPMargeCommandDesc(tTVPCommandOptionList& dest, const tTVPCommandOptionList& src);

//! @brief プラグイン (.dll / .so 等) から option desc を取得する。
//!        Win32: PE リソースから。SDL3: 隣接する `<basename>.options.json` から。
extern tTVPCommandOptionList* TVPGetPluginCommandDesc(const tjs_char* name);

//! @brief 本体 (krkrz exe) の option desc を取得する。
//!        Win32: PE リソースから。SDL3: resource/optiondesc.json から。
extern tTVPCommandOptionList* TVPGetEngineCommandDesc();

#endif // __READ_OPTION_DESC_H__
