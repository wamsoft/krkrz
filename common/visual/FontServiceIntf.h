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

// グリフ単位のメトリクス (ピクセル。TVP_FONT_METRICS_UNSCALED 指定時のみ
// フォントユニット)
struct tTVPFontGlyphMetrics
{
	float AdvanceX;
	float AdvanceY;
	float BearingX;
	float BearingY;
	float Width;
	float Height;
};

// グリフメトリクスの取得モード (TVPFontGetGlyphMetricsEx)
#define TVP_FONT_METRICS_HINTED		0	// グリッドフィット (描画と一致。既定)
#define TVP_FONT_METRICS_UNHINTED	1	// リニア (advance が整数に丸まらない。組版用)
#define TVP_FONT_METRICS_UNSCALED	2	// フォントユニット (pixelSize 無視・サイズ非依存)

// バリアブルフォント (fvar) の軸情報
struct tTVPFontVarAxis
{
	tjs_uint32 Tag;             // ビッグエンディアン詰めタグ ('wght' 等)
	float MinValue;
	float DefaultValue;
	float MaxValue;
};

// バリアブルフォントの軸座標指定
struct tTVPFontVarCoord
{
	tjs_uint32 Tag;
	float Value;
};

// グリフのラスタライズ指定 (TVPFontRenderGlyphMask)
//
// アウトラインをピクセルにする処理は品質 (AA / ストローク / グリッドフィット)
// を外すと目立つので本体側で持つ。消費者は返った 8bit カバレッジを好きな色で
// 合成するだけでよく、本体 drawText と見た目が揃う。
#define TVP_FONT_JOIN_MITER		0
#define TVP_FONT_JOIN_ROUND		1
#define TVP_FONT_JOIN_BEVEL		2
#define TVP_FONT_CAP_BUTT		0
#define TVP_FONT_CAP_ROUND		1
#define TVP_FONT_CAP_SQUARE		2

struct tTVPFontRenderParams
{
	// 行優先 2x3: {xx, xy, dx, yx, yy, dy}
	// **フォントユニット (y-up) → ピクセル (y-up)** のアフィン。サイズ・斜体
	// シアー・幅スケール・サブピクセル位置をここに畳み込む (アウトラインに
	// 焼き込んでからラスタライズするので後段でのスケール劣化が無い)
	float Transform[6];
	bool Bold;                  // 合成ボールド
	bool Italic;                // 合成イタリック
	float StrokeWidth;          // 0 = 塗り、>0 = 縁取り (内部は塗らない)
	tjs_int Join;               // TVP_FONT_JOIN_*
	tjs_int Cap;                // TVP_FONT_CAP_*
	float MiterLimit;
};

// 8bit カバレッジマスク。Left/Top はペン原点からのオフセット (ピクセル、
// **y 上向き正**)。Buffer は同一 face への次のラスタライズ呼び出しまで有効
struct tTVPFontGlyphMask
{
	tjs_int Left;
	tjs_int Top;
	tjs_int Width;
	tjs_int Height;
	tjs_int Pitch;
	const tjs_uint8 * Buffer;
};

// カラーグリフ (COLR v0/v1) のレイヤー
//
// COLR グリフは「アウトライン + 塗り」のレイヤーを変換で入れ子にしたペイント
// グラフで、FreeType が合成したビットマップを貰う代わりにグラフを貰えば、
// **消費側のラスタライザ**で任意サイズに描ける (ベクタテキスト向け)。
// 座標系は FreeType 準拠の y-up。アウトラインは TVPFontGetGlyphOutline の
// フォントユニット、Transform とグラデーション座標は指定ピクセルサイズ基準
// (Transform がフォントユニット→ピクセルのスケールを含む)。
#define TVP_FONT_PAINT_SOLID	0
#define TVP_FONT_PAINT_LINEAR	1	// 線形グラデーション
#define TVP_FONT_PAINT_RADIAL	2	// 放射グラデーション

struct tTVPFontColorStop
{
	float Offset;               // 0..1
	tjs_uint8 R, G, B, A;
};

struct tTVPFontColorLayer
{
	tjs_uint32 GlyphId;         // 塗りつぶす対象のアウトライングリフ
	float Transform[6];         // 行優先 2x3: {xx, xy, dx, yx, yy, dy}
	tjs_int PaintKind;          // TVP_FONT_PAINT_*
	tjs_uint8 R, G, B, A;       // SOLID
	float X0, Y0, X1, Y1;       // LINEAR: 始点/終点、RADIAL: 焦点/中心
	float R0, R1;               // RADIAL: 半径
	tjs_int StopCount;          // グラデーションのカラーストップ数
	const tTVPFontColorStop * Stops;   // コールバック中のみ有効
};

// カラーレイヤーの受け取り (背面から前面の順に Layer が呼ばれる)
class iTVPFontColorLayerSink
{
public:
	virtual void TJS_INTF_METHOD Layer(const tTVPFontColorLayer & layer) = 0;
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

TJS_EXP_FUNC_DEF(tTVPFontFaceHandle, TVPFontAcquireFaceInstance, (const ttstr & nameOrPath,
	const tTVPFontVarCoord * coords, tjs_int count));
	// バリアブルフォントの**専用インスタンス**を取得する。TVPFontAcquireFace と
	// 違い、レジストリ共有 face ではなく専用の face を開く (軸座標は face の
	// 状態なので、共有 face に設定すると本体/他プラグインの描画まで変わる)。
	// フォントバイト列は共有バッファのまま。解放は TVPFontReleaseFace

TJS_EXP_FUNC_DEF(bool, TVPFontGetFaceData, (tTVPFontFaceHandle face,
	const tjs_uint8 ** data, tjs_uint64 * size, tjs_int * faceIndex));
	// face の SFNT バイト列 (共有バッファ) を直接参照する。自前でシェイパを
	// 走らせる利用者 (minikin 等、フォントデータから独自に hb_face を作る系) 向け。
	// ストレージパスキーでも "@gdi:" キーでも取得でき、face ハンドルの生存中有効

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

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphMetricsEx, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, tjs_int pixelSize, bool bold, bool italic, tjs_int mode,
	tTVPFontGlyphMetrics * out));
	// mode (TVP_FONT_METRICS_*) 付きのメトリクス取得。
	// TVPFontGetGlyphMetrics は mode=HINTED と等価。
	// 組版エンジンは UNHINTED (または UNSCALED) を使うこと: HINTED の advance は
	// 整数ピクセルに丸められるので、文字を並べるほど位置がずれる

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphOutline, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, bool bold, bool italic, iTVPFontOutlineSink * sink));
	// グリフアウトラインを分解して sink へ通知する。座標は**フォントユニット**
	// (y-up)。ピクセルへは pixelSize / LineMetrics.UnitsPerEm でスケールする

TJS_EXP_FUNC_DEF(bool, TVPFontGetGlyphBitmap, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, tjs_int pixelSize, bool color, bool bold, bool italic,
	tTVPFontGlyphBitmap * out));
	// グリフをラスタライズする。color=true でカラー絵文字 (BGRA) を要求
	// (非カラーフォントでは GRAY にフォールバック)。Buffer は次のグリフ取得まで有効

TJS_EXP_FUNC_DEF(bool, TVPFontRenderGlyphMask, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, const tTVPFontRenderParams * params, tTVPFontGlyphMask * out));
	// グリフをラスタライズして 8bit カバレッジマスクを得る。アウトラインを
	// 持たないグリフ (ビットマップのみのカラー絵文字) では false

TJS_EXP_FUNC_DEF(tjs_int, TVPFontGetColorLayers, (tTVPFontFaceHandle face,
	tjs_uint32 glyphId, tjs_int pixelSize, iTVPFontColorLayerSink * sink,
	float * clipBox));
	// COLR (v0/v1) のペイントグラフをレイヤー列に展開して sink へ通知する。
	// 戻り値はレイヤー数 (0 = ペイントグラフを持たないグリフ。CBDT/sbix の
	// ビットマップ絵文字はこちらではなく TVPFontGetGlyphBitmap を使う)。
	// clipBox が非 nullptr なら {xMin,yMin,xMax,yMax} (ピクセル・y-up) を書く
	// (クリップボックスを持たないフォントでは 4 要素すべて 0)

//---------------------------------------------------------------------------
// バリアブルフォント (fvar)
//---------------------------------------------------------------------------

TJS_EXP_FUNC_DEF(tjs_int, TVPFontGetVarAxes, (tTVPFontFaceHandle face,
	tTVPFontVarAxis * out, tjs_int maxCount));
	// face の可変軸を out に最大 maxCount 個書き込み、軸の総数を返す
	// (out=nullptr / maxCount=0 で個数だけ問い合わせ可能)。可変軸が無ければ 0

TJS_EXP_FUNC_DEF(bool, TVPFontSetVariations, (tTVPFontFaceHandle face,
	const tTVPFontVarCoord * coords, tjs_int count));
	// 軸座標を設定する。値は軸の範囲にクランプされ、指定しなかった軸は現状維持。
	// **軸座標は face の状態**なので、共有 face (TVPFontAcquireFace) に対して
	// 呼ぶと他の利用者の描画も変わる。専用インスタンスには
	// TVPFontAcquireFaceInstance を使うこと

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
