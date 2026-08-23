//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors
	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//! @brief 設定ファイル (.cf / .cfu / 埋め込みオプション) の 1 行正規化
//---------------------------------------------------------------------------
#ifndef __CONFIG_LINE_H__
#define __CONFIG_LINE_H__

#include <string>

//---------------------------------------------------------------------------
// 設定ファイルの 1 行を、オプション文字列として解釈する前に正規化する。
//
// 行を読み出す実装がバリアントで違い、 行末に余計な文字が残っていた:
//   win32   : fgets (テキストモード) → 行末の '\n' がそのまま残る
//   generic : std::getline → '\n' では切れるが CRLF の '\r' が残る
//
// これがそのまま値に紛れ込むため、 `key=value` 形式の素の値は
// "value\n" / "value\r" になり、 `== TJS_W("yes")` のような比較が
// 静かに失敗していた。 さらに `=` を持たない行 ( 値省略で "yes" 扱いに
// なるはずの行 ) は、 オプション名側に改行が付いて参照不能だった。
// クォート付きの値 (`key="value"`) だけは TJSParseString が閉じクォートで
// 止まるため無事だった ( .cfu が全値クォートで書かれているのはこのため )。
//
// ここで行末の改行・前後の空白・先頭の UTF-8 BOM をまとめて落とす。
// クォート付きの値の中身には触らない ( 引用符の外側だけを削るため )。
//---------------------------------------------------------------------------
inline std::string TVPNormalizeConfigLine( const std::string & line )
{
	std::string::size_type b = 0, e = line.size();

	// 先頭の UTF-8 BOM ( Windows のエディタが付けがち )
	if( e - b >= 3 &&
		static_cast<unsigned char>(line[b  ]) == 0xEF &&
		static_cast<unsigned char>(line[b+1]) == 0xBB &&
		static_cast<unsigned char>(line[b+2]) == 0xBF ) b += 3;

	while( b < e && (line[b] == ' ' || line[b] == '\t') ) b++;
	while( e > b ) {
		const char c = line[e-1];
		if( c == '\r' || c == '\n' || c == ' ' || c == '\t' ) e--;
		else break;
	}
	return line.substr( b, e - b );
}
//---------------------------------------------------------------------------
//! @brief 正規化済みの行が読み飛ばすべき行 ( 空行 / ';' コメント ) か
//---------------------------------------------------------------------------
inline bool TVPIsConfigLineIgnorable( const std::string & line )
{
	return line.empty() || line[0] == ';';
}
//---------------------------------------------------------------------------

#endif // __CONFIG_LINE_H__
