//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// フォントサービス (共有フォントストリーム + glyphware 統一フォントエンジン) の
// プラグイン公開 API。
//
// - バイト供給: 同一フォントは本体/全プラグインで 1 共有バッファ (FontStream)
// - 名前解決: fonts.json 宣言名 / ストレージパス / (WINVER) GDI フォント名
// - face/チェーン: フォールバック連鎖 (カンマ区切り) + 文字単位の face 選択
// - グリフ供給: メトリクス / アウトライン (フォントユニット) / ビットマップ
// - シェイピング: 1 行レイアウト (BiDi + itemize + HarfBuzz、視覚順)
// - リッチ検索: 名前 + スタイル + スクリプト + 収録文字
//
// glyphware 無効ビルド (KRKRZ_USE_GLYPHWARE=OFF) ではストリーム/バッファ系のみ
// 機能し、face/グリフ/検索系は失敗 (nullptr / false / 0 件) を返す。
// いずれの関数もメインスレッドから呼ぶこと。
//---------------------------------------------------------------------------
#ifndef __FONT_SERVICE_INTF_H__
#define __FONT_SERVICE_INTF_H__

#include "tjsCommHead.h"

/*[*/
//---------------------------------------------------------------------------
// フォントサービス 型定義
//---------------------------------------------------------------------------

// 不透明ハンドル
typedef void * tTVPFontBufferHandle;     // 共有フォントバッファの保持ハンドル
typedef void * tTVPFontFaceHandle;       // 単一フォント face
typedef void * tTVPFontFaceChainHandle;  // フォールバック連鎖

// face 全体のラインメトリクス (現在のピクセルサイズ基準。offset は baseline
// からの距離で下方向が正)
struct tTVPFontLineMetrics
{
	float Ascent;
	float Descent;              // 下方向の広がり (正値)
	float LineGap;
	float UnitsPerEm;           // アウトライン (フォントユニット) のスケール基準
	float UnderlineOffset;
	float UnderlineThickness;
	float StrikeoutOffset;
	float StrikeoutThickness;
};

// グリフ単位のメトリクス (ピクセル)
struct tTVPFontGlyphMetrics
{
	float AdvanceX;
	float AdvanceY;
	float BearingX;
	float BearingY;
	float Width;
	float Height;
};

// グリフビットマップ形式
#define TVP_FONT_BITMAP_GRAY	0	// 1 byte/px (0-255, 256 階調)
#define TVP_FONT_BITMAP_BGRA	2	// 4 byte/px BGRA (カラー絵文字、前乗算済)

// グリフビットマップ。Buffer は同一 face への次のグリフ取得呼び出しまで有効
// (必要ならコピーして保持すること)
struct tTVPFontGlyphBitmap
{
	tjs_int Format;             // TVP_FONT_BITMAP_*
	tjs_int Left;               // ビットマップ原点への bearing
	tjs_int Top;
	tjs_int Width;
	tjs_int Height;
	tjs_int Pitch;              // バイト/行 (負値ならボトムアップ)
	const tjs_uint8 * Buffer;
};

// アウトライン受け取りインターフェース。座標は**フォントユニット** (y-up、
// FreeType 格納系)。ピクセルへは pixelSize / UnitsPerEm 倍で変換する
class iTVPFontOutlineSink
{
public:
	virtual void TJS_INTF_METHOD MoveTo(float x, float y) = 0;
	virtual void TJS_INTF_METHOD LineTo(float x, float y) = 0;
	virtual void TJS_INTF_METHOD QuadTo(float cx, float cy, float x, float y) = 0;
	virtual void TJS_INTF_METHOD CubicTo(float c1x, float c1y, float c2x, float c2y,
		float x, float y) = 0;
	virtual void TJS_INTF_METHOD ClosePath() = 0;
};

// 1 行レイアウトの整形済みグリフ (視覚順・x 昇順)。X/Y はペン位置 (baseline
// 原点、ピクセル)、FaceIndexInChain は連鎖内のどの face か
// (TVPFontChainFaceAt で取得)
struct tTVPFontShapedGlyph
{
	tjs_uint32 GlyphId;
	tjs_int FaceIndexInChain;
	float X;
	float Y;
	float XOffset;              // シェイパの per-glyph オフセット
	float YOffset;
	float Advance;
	tjs_uint32 Cluster;         // 元 UTF-8 文字列へのバイトオフセット
	bool RTL;
};

// 1 行レイアウト結果の受け取りインターフェース。Begin が 1 回呼ばれた後、
// グリフ数だけ Glyph が呼ばれる
class iTVPFontShapeSink
{
public:
	virtual void TJS_INTF_METHOD Begin(tjs_int glyphCount, float width,
		float ascent, float descent) = 0;
	virtual void TJS_INTF_METHOD Glyph(const tTVPFontShapedGlyph & glyph) = 0;
};

// ベース方向 (TVPFontShapeLine)
#define TVP_FONT_BASEDIR_AUTO	0
#define TVP_FONT_BASEDIR_LTR	1
#define TVP_FONT_BASEDIR_RTL	2

// リッチ検索の条件。未指定 (-1 / 空文字列) の項目は制約しない
struct tTVPFontQueryParams
{
	ttstr Name;                 // family / 別名 / fullName / PostScript 名
	tjs_int Weight;             // 100-900 (OS/2 usWeightClass)、-1=不問
	tjs_int Slant;              // 0=normal 1=italic 2=oblique、-1=不問
	ttstr Script;               // ISO-15924 タグ (例 "Jpan" "Hans")
	ttstr ContainsText;         // この文字列の全コードポイントを収録すること
	tjs_int Monospace;          // 0/1、-1=不問
	tjs_int Color;              // 0/1 (カラー絵文字)、-1=不問
	tTVPFontQueryParams() : Weight(-1), Slant(-1), Monospace(-1), Color(-1) {}
};

// 検索結果 / メタデータ照会の 1 face 分の情報
struct tTVPFontFaceInfo
{
	ttstr Key;                  // フォントキー (TVPFontAcquireFace に渡せる)
	tjs_int FaceIndex;
	ttstr Family;
	ttstr Subfamily;
	ttstr FullName;
	ttstr PostScriptName;
	tjs_int Weight;             // 100-900
	tjs_int Slant;              // 0=normal 1=italic 2=oblique
	bool Bold;
	bool Color;
	bool Monospace;
	bool Scalable;
};

// 検索結果の受け取りインターフェース (ランク順に Found が呼ばれる)
class iTVPFontQuerySink
{
public:
	virtual void TJS_INTF_METHOD Found(const tTVPFontFaceInfo & info) = 0;
};
/*]*/

//---------------------------------------------------------------------------
// 共有フォントバイト供給 (FontStream)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(iTJSBinaryStream *, TVPCreateFontStream, (const ttstr & storage));
	// フォントを共有オンメモリバッファ上の読み取りストリームとして開く
	// (本体 FreeType と同一バッファ共有)。解放は delete。開けない場合は例外

TJS_EXP_FUNC_DEF(tTVPFontBufferHandle, TVPAcquireFontBuffer, (const ttstr & storage,
	const tjs_uint8 ** data, tjs_uint64 * size));
	// フォントの共有バッファ (連続メモリ) を直接取得する。data/size に
	// バッファが返り、戻り値のハンドルを TVPReleaseFontBuffer するまで有効。
	// 開けない場合は nullptr

TJS_EXP_FUNC_DEF(void, TVPReleaseFontBuffer, (tTVPFontBufferHandle buffer));
	// TVPAcquireFontBuffer の保持を解除する

//---------------------------------------------------------------------------
// フォント名解決
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(ttstr, TVPFontResolveKey, (const ttstr & nameOrPath));
	// フォント名/パス (単一トークン) をフォントキーへ解決する。
	// 解決順: fonts.json 宣言名 → ストレージパス → (WINVER) GDI フォント名。
	// 返ったキーは TVPFontAcquireFace / TVPCreateFontStream (パスの場合) に渡せる

TJS_EXP_FUNC_DEF(bool, TVPFontNameKnown, (const ttstr & name));
	// フォント名が解決可能 (fonts.json 宣言 / 実行時登録 / GDI 実在) かどうか

//---------------------------------------------------------------------------
// face / フォールバック連鎖 (glyphware)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(tTVPFontFaceHandle, TVPFontAcquireFace, (const ttstr & nameOrPath));
	// 単一フォントの face を取得する (名前解決込み)。失敗時 nullptr。
	// 解放は TVPFontReleaseFace

TJS_EXP_FUNC_DEF(void, TVPFontReleaseFace, (tTVPFontFaceHandle face));

TJS_EXP_FUNC_DEF(tTVPFontFaceChainHandle, TVPFontAcquireFaceChain,
	(const ttstr & commaSeparatedNames));
	// カンマ区切りのフォント名リストからフォールバック連鎖を構築する。
	// 空文字列なら既定フェイス連鎖。解決できた face が 0 個でも連鎖自体は返る
	// (TVPFontChainCount で確認)。解放は TVPFontReleaseFaceChain

TJS_EXP_FUNC_DEF(void, TVPFontReleaseFaceChain, (tTVPFontFaceChainHandle chain));

TJS_EXP_FUNC_DEF(tjs_int, TVPFontChainCount, (tTVPFontFaceChainHandle chain));

TJS_EXP_FUNC_DEF(tTVPFontFaceHandle, TVPFontChainFaceAt, (tTVPFontFaceChainHandle chain,
	tjs_int index));
	// 連鎖内の face を借用参照で返す (Release 不要、連鎖ハンドルの生存中のみ有効)

TJS_EXP_FUNC_DEF(tjs_int, TVPFontChainFaceForChar, (tTVPFontFaceChainHandle chain,
	tjs_uint32 codepoint, bool preferLast));
	// コードポイントを収録する face の連鎖内 index を返す (-1=どれも未収録)。
	// preferLast=true は絵文字向け (末尾 face を優先)

//---------------------------------------------------------------------------
// メトリクス / グリフ供給 (glyphware)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(bool, TVPFontGetLineMetrics, (tTVPFontFaceHandle face,
	tjs_int pixelSize, tTVPFontLineMetrics * out));

TJS_EXP_FUNC_DEF(tjs_uint32, TVPFontGetGlyphIndex, (tTVPFontFaceHandle face,
	tjs_uint32 codepoint));
	// コードポイント→グリフ ID (0=未収録)

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphMetrics, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, tjs_int pixelSize, bool bold, bool italic,
	tTVPFontGlyphMetrics * out));
	// 合成 bold/italic 適用後の advance/bearing (描画と一致する値)

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphOutline, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, bool bold, bool italic, iTVPFontOutlineSink * sink));
	// グリフアウトラインを分解して sink へ通知する。座標は**フォントユニット**
	// (y-up)。ピクセルへは pixelSize / LineMetrics.UnitsPerEm でスケールする

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphBitmap, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, tjs_int pixelSize, bool color, bool bold, bool italic,
	tTVPFontGlyphBitmap * out));
	// グリフをラスタライズする。color=true でカラー絵文字 (BGRA) を要求
	// (非カラーフォントでは GRAY にフォールバック)。Buffer は次のグリフ取得まで有効

//---------------------------------------------------------------------------
// 1 行レイアウト (BiDi + itemize + HarfBuzz シェイピング、glyphware)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(bool, TVPFontShapeLine, (tTVPFontFaceChainHandle chain,
	const ttstr & text, tjs_int pixelSize, tjs_int baseDirection,
	iTVPFontShapeSink * sink));
	// 1 行 (改行を含まない) を視覚順の整形済みグリフ列にする。
	// baseDirection は TVP_FONT_BASEDIR_*。カーニング/合字/複雑スクリプト対応

//---------------------------------------------------------------------------
// リッチ検索 / メタデータ (glyphware)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(tjs_int, TVPFontQueryFaces, (const tTVPFontQueryParams & params,
	iTVPFontQuerySink * sink));
	// 登録済みフォント (fonts.json 宣言 + 実行時登録) から条件に合う face を
	// ランク順で sink へ通知する。戻り値は件数

TJS_EXP_FUNC_DEF(bool, TVPFontGetFaceInfo, (const ttstr & nameOrPath,
	tTVPFontFaceInfo * out));
	// 単一フォントを解決して SFNT メタデータを返す

//---------------------------------------------------------------------------
// ThorVG "gw" テキストローダ連携
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(const void *, TVPGetFontTvgBridge, ());
	// ThorVG フォークの "gw" テキストローダ用ブリッジ (TvgGwBridge*、
	// thorvg_gw_bridge.h 定義) を返す。thorvg を独自に静的リンクする
	// プラグインは、自分の thorvg コピーへ
	// tvgGwSetBridge((const TvgGwBridge*)TVPGetFontTvgBridge()) で注入する。
	// glyphware/Elements 無効ビルドでは nullptr

#endif // __FONT_SERVICE_INTF_H__
