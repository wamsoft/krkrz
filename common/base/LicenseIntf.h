//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// ライセンス情報の内部保持 (圧縮) と収集
//
// 3 系統のライセンス文を一つの一覧に合流させる:
//  1. 本体内蔵     — licenses/manifest.json から gen_licenses_c.py が生成する
//                    zlib deflate 圧縮テーブル (BuiltinLicenses.cpp)
//  2. プラグイン登録 — tp_stub 公開の TVPRegisterLicense(Text) で登録
//  3. storage 収集  — プロジェクト data の licenses/*.txt を実行時に列挙
//                    (案件が追加フォント等のライセンス文を置くだけで一覧に載る)
//
// TJS からは System.getLicenseList() / System.getLicenseText(name) で参照でき、
// ゲーム側のライセンス表示 UI (フォント選択画面等) を自前で組める。
//---------------------------------------------------------------------------
#ifndef __LICENSE_INTF_H__
#define __LICENSE_INTF_H__

#include "tjsCommHead.h"
#include <vector>

//---------------------------------------------------------------------------
// 本体内蔵テーブル (生成物 BuiltinLicenses.cpp が定義する)
//---------------------------------------------------------------------------
struct tTVPBuiltinLicenseEntry
{
	const tjs_char * name;
	const tjs_char * group;
	const unsigned char * deflated;   // zlib deflate 済み UTF-8 テキスト
	unsigned int deflatedSize;
	unsigned int originalSize;
};
extern const tTVPBuiltinLicenseEntry TVPBuiltinLicenses[];
extern const int TVPBuiltinLicenseCount;

//---------------------------------------------------------------------------
// 収集一覧のエントリ (エンジン内部/TJS バインド用)
//---------------------------------------------------------------------------
struct tTVPLicenseInfo
{
	ttstr Name;
	ttstr Group;    // "engine" / "font" / "plugin" / "data" 等 (登録側が自由に付与)
	ttstr Source;   // "builtin" / "plugin" / "storage"
};

// 全ライセンスを列挙する (内蔵 + プラグイン登録 + storage licenses/*.txt)
void TVPGetLicenseList(std::vector<tTVPLicenseInfo> & out);

/*[*/
//---------------------------------------------------------------------------
// ライセンス収集 API の存在判定マクロ
//   古い tp_stub / 本体ヘッダには TVPRegisterLicense 系の宣言が無いため、
//   プラグイン側のライセンス登録コード (生成物 LicensesGen.cpp 等) は
//   #ifdef TVP_HAS_LICENSE_API でガードし、未対応バージョンの tp_stub と
//   組み合わせたビルドでは登録をスキップしてビルドを通す。
//---------------------------------------------------------------------------
#define TVP_HAS_LICENSE_API 1

//---------------------------------------------------------------------------
// ライセンス列挙の受け取りインターフェース (プラグイン向け)
//---------------------------------------------------------------------------
class iTVPLicenseListSink
{
public:
	virtual void TJS_INTF_METHOD Found(const ttstr & name, const ttstr & group,
		const ttstr & source) = 0;
};
/*]*/

TJS_EXP_FUNC_DEF(void, TVPRegisterLicense, (const ttstr & name, const ttstr & group,
	const tjs_uint8 * deflated, tjs_uint deflatedSize, tjs_uint originalSize));
	// zlib deflate 済みライセンス文 (UTF-8) を登録する。データはコピーされず
	// 参照保持されるため、静的データ (生成器 gen_licenses_c.py --mode plugin の
	// 出力等) を渡すこと。同名は上書き

TJS_EXP_FUNC_DEF(void, TVPRegisterLicenseText, (const ttstr & name,
	const ttstr & group, const ttstr & text));
	// 生テキストでライセンス文を登録する (短いもの向け)

TJS_EXP_FUNC_DEF(bool, TVPGetLicenseText, (const ttstr & name, ttstr & text));
	// 名前でライセンス文を取得する (内蔵/プラグイン/storage 横断)。無ければ false

TJS_EXP_FUNC_DEF(tjs_int, TVPEnumLicenses, (iTVPLicenseListSink * sink));
	// 全ライセンスを列挙して sink へ通知する。戻り値は件数

#endif // __LICENSE_INTF_H__
