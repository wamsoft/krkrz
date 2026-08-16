# フォントエンジン内部実装ノート

フォントまわりの内部構成 (バイト供給・遅延ロード・glyphware 統合・tp_stub
公開・ThorVG 連携) の実装 SSOT。**利用者向けの総合ガイドは umbrella の
`doc/guide/FontSystem.md`** (mkdocs サイト「フォントシステム」) を参照。

旧 `FontMetadata.md` / `FontService.md` は本書へ統合した (2026-08-10)。

## コンポーネント対応表

| 役割 | 実装 |
|---|---|
| 名前→ストレージ対応・遅延ロード | `common/visual/FontSystem.{h,cpp}` (`LazyFontFiles` / `LazyFontStorageAll` / `LoadFontMetadata`) |
| 共有オンメモリバイト供給 | `common/visual/FontStream.{h,cpp}` + `common/base/StorageCache.{h,cpp}` |
| 統一フォントエンジン | `external/glyphware` (FreeType+HarfBuzz+SheenBidi、krkr 非依存 submodule) |
| glyphware ホスト接続 | `common/visual/GlyphwareHost.{h,cpp}` (ローダ注入・キー解決・レジストリ) |
| ラスタライザ (drawText) | `common/visual/{FreeTypeFontRasterizer,GlyphwareFontRasterizer}.cpp` / `win32/visual/GDIFontRasterizer.cpp`、切替 = `common/visual/LayerBitmapImpl.cpp` |
| シェイピング描画 | `common/visual/GlyphwareText.{h,cpp}` (`Layer.drawShapedText` / `drawShapedTextArea` / `measureShapedText` / `shapedTextCount`) |
| 矩形テキストレイアウト (折返し/禁則/count) | `external/glyphware` `include/glyphware/Layout.h` + `src/Block.cpp` (`layoutBlock` / `countClusters` / `limitClusters`) |
| GDI システムフォント抽出 (WINVER) | `win32/visual/GlyphwareGDIFont.cpp` (`GetFontData` スナップショット / `@gdi:` キー) |
| プラグイン公開 (tp_stub) | `common/visual/FontServiceIntf.{h,cpp}` |
| TJS 検索/登録 API | `common/visual/LayerIntf.cpp` (`Font.queryFonts` / `getFontInfo` / `registerFontFile`) |
| ThorVG (Elements / layerExVector) 連携 | `common/visual/elements/GlyphwareTvgBridge.cpp` + thorvg フォーク `src/loaders/gw/` |

## 共有オンメモリ・フォントストリーム (FontStream)

旧 `FreeType.cpp` 内 `OpenFontFile`/`_fontfiles`/`_fontlist` の汎用化。

- `TVPGetFontStream(storage)` — StorageCache 上の共有バッファへのストリーム
  ビュー (classic FreeType 等のストリーム消費者向け)。キャッシュ対象外
  (拡張子閾値超え) は従来どおり直接ファイルストリーム。
- `TVPGetFontStreamBuffer(storage)` — 共有バッファそのもの (glyphware 等、
  連続メモリを直接参照するゼロコピー消費者向け)。キャッシュ対象外でも
  全読みバッファを返し、それも弱参照マップで共有される。
- パスは `TVPGetPlacedPath` で正規化してキー化。MRU (既定 10 件、
  `TVPSetFontStreamCacheMax`) がバッファを pin し、shared_ptr 保持中は
  StorageCache の refcount-aware eviction が退避しない。XP3 内フォント対応。
- StorageCache 側の公開口: `TVPGetStorageCacheBuffer` (共有バッファ直接取得) /
  `TVPCreateSharedMemoryStream` (バッファ上のストリームビュー生成)。
- **同一ストレージのフォントは classic FreeType / glyphware / ThorVG 系 /
  プラグインで 1 バッファ共有**。

## fonts.json 宣言と遅延ロード

- **経路**: `FontSystem::InitFontNames` → `LoadFontMetadata()` (picojson) が
  `fonts.json` の `family`/`aliases` を `LazyFontFiles` (name→storage) へ登録
  (この時点で FreeType パースしない)。初回使用時に
  `EnsureLazyFontLoaded()` → `AddExtraFont()` で実ロード。
- `LazyFontStorageAll` は erase されない永続版で、glyphware 側の名前解決
  (`GetLazyFontStorage`) が使う。`Font.addFont`/`registerFontFile` の実行時
  登録も name→storage をここへ記録する。
- **スキーマ**: `file` 必須、他は任意 (`family`/`subfamily`/`fullName`/
  `postScriptName`/`weight`/`width`/`italic`/`faceIndex`/`languages`/
  `aliases`/`scripts`/`ranges`/`flags`= `emoji`|`color`|`monospace`)。
  glyphware `Manifest` パーサ (`external/glyphware/src/Manifest.cpp`) と互換。
- **family は純粋な family 名**とし、classic ラスタライザの face 名規約
  「family subfamily」連結は `aliases` に出す (生成器が自動出力)。
  `TVPColorEmojiFaceName` 既定 "Noto Color Emoji Regular" 等の連結名参照は
  alias で解決される。
- 生成器: `tools/fontgen/gen_fonts_json.py` (umbrella、要 fonttools)。
- **宣言メタの開かず判定**: `FontServiceIntf.cpp` の
  `SyncFontManifestToRegistry` が fonts.json を glyphware Manifest として
  宣言メタ込みでレジストリ登録 (`TVPGlyphwareAddDeclaredEntry`、エントリ
  キャッシュでキー重複排除)。glyphware `Registry::ensureFor` は宣言済み
  style (`styleDeclared`) / scripts / ranges を信頼し、宣言で答えられる
  クエリではフォントを開かない。`Font.queryFonts` も宣言 entry を resolve
  せず宣言値で返す (宣言名も無い裸キーのみ SFNT 解決)。

## glyphware 統合 (drawText / 名前解決)

- **ラスタライザ**: enum は `FONT_RASTER_FREE_TYPE`(0) / WINVER のみ
  `FONT_RASTER_GDI` / `FONT_RASTER_GLYPHWARE` (`LayerBitmapImpl.cpp`)。既定は
  WINVER=GDI・他=FreeType (不変)。glyphware ラスタライザはグリフ生成のみ
  差し替えで cell-stepping/影/縁取り/下線/VS15-16 は classic 経路を流用。
- **名前解決順** (`TVPGlyphwareResolveFontKey`): ①fonts.json/実行時登録の
  宣言名→storage ②実在ストレージパス ③(WINVER) GDI フォント名→`@gdi:` キー
  ④素通し。パス風トークン (`://`/区切り/フォント拡張子) は GDI 解決へ回さない
  (既定フォント代替の乗っ取り防止)。GDI 実在確認は `EnumFontFamiliesEx`。
  ①の宣言テーブル (fonts.json) は classic 経路の `InitFontNames` まで遅延
  構築されるため、解決前に `EnsureFontMetadataLoaded()` を必ず呼ぶ
  (glyphware 側が最初のフォント利用者になるケース: -nostartup REPL 等)。
- **レジストリ**: `TVPGetGlyphwareRegistry()` がプロセス共有。登録口は
  `TVPGlyphwareEntryForKey` / `TVPGlyphwareAddDeclaredEntry` に一本化
  (registry 自体はキー重複を排除しないため、エントリキャッシュが唯一の登録口)。
- 絵文字 color→mono フォールバック、既定フェイスフォールバック、SFNT 実名
  フォールバック (`EntryForToken`) 等の解決堅牢化は
  `GlyphwareHost.cpp` / `FontServiceIntf.cpp` を参照。

## 矩形テキストレイアウト (折返し / 禁則 / count)

**段落分割・単語/文字単位の折返し・行頭行末禁則・整列・クラスタ `count` 制限は
glyphware 側 (`glyphware::layoutBlock`) にある**。core (`GlyphwareText.cpp`) が
持つのはビットマップ依存部 (`BlitLine` / `BlitBand` / `BlitDecorations` /
`PrepareContext`) だけで、`TVPGlyphwareDrawTextArea` は
「`layoutBlock` → 各行を blit」の薄い層。

- 目的は **Layer 版 (`drawShapedTextArea`) と Elements 側のテキスト widget が
  同一ロジックで折り返すこと**。以前は折返しが core の `tTVPBaseBitmap` 密結合
  コードにしか無く、Elements からは使えなかった。
- 全て **UTF-8 バイトオフセット基準** (`PositionedGlyph::cluster` と同じ基準)。
  以前の UTF-16 index 版から書き直してある。
- **折返しは常に全文で確定してから `count` を適用する** ので、タイプライタ表示で
  行が組み替わらない (`BlockLine::layout.width` は制限前の行幅のまま = 整列も
  ぶれない)。
- 行ピッチ・ascent は整数に丸めてから積む (グリフ blit が整数座標のため)。
- `countClusters()` は**折り返さない**全文のクラスタ数 (= `shapedTextCount`)、
  `BlockLayout::totalClusters` は**折り返した行**のクラスタ数。行末で捨てられる
  空白のぶん前者が大きくなることがある (`count` は飽和するだけ)。
- 禁則テーブルは `src/Block.cpp` の `isNoStartCp` / `isNoEndCp` (日本語向け簡易
  版)。言語別に差し替えるならここ。**将来課題**: 言語別の禁則を差し替えられる形
  (テーブル注入) にする。
- **折返し中の幅計測は段落 1 回シェイプ + 前置和** (`ParaWidths`)。クラスタ値が
  バイトオフセットなので、グリフの advance をクラスタで bucket して前置和にすれば
  部分幅は引き算で出る。ただし**筆記体/BiDi 段落だけは部分文字列シェイプのまま**
  (`paragraphIsSimple`)。アラビア語等は「途中で切ると medial → final に変わって
  幅が変わる」ため、最終的に行単位でシェイプして描く現状と計測を一致させる必要が
  ある。実測 (18px・560px 幅・長文): 日本語 6.41→4.23ms/call、英語 4.91→3.52ms/call、
  アラビア語は据置。出力はバイト一致。
- **Elements も同じ折返しを使う**: `common/visual/elements/BlockTextBackend.cpp`
  が elements の注入 I/F `block_text_backend` を glyphware で実装し、新ウィジェット
  `text_area` (JSON) が消費する。既存 `label` / `text_box` は従来の cycfi wrap の
  まま (差し替えると既存画面の改行位置が変わるため)。詳細は
  `doc/ElementsDialog.md` の「矩形テキスト (`text_area`)」。

## フォントサービス (プラグイン向け tp_stub API)

`common/visual/FontServiceIntf.h` で公開。**全関数メインスレッド専用**。
glyphware 無効ビルド (`KRKRZ_USE_GLYPHWARE=OFF`) ではバイト供給/名前解決の
基本部のみ機能し、face/グリフ/シェイピング/検索は失敗を返すスタブになる。

| 分類 | 関数 | 概要 |
|---|---|---|
| バイト供給 | `TVPCreateFontStream(storage)` | 共有バッファ上の読み取りストリーム (delete で解放) |
| | `TVPAcquireFontBuffer(storage, &data, &size)` / `TVPReleaseFontBuffer` | 連続メモリの直接参照 (ゼロコピー、ハンドル解放まで有効) |
| 名前解決 | `TVPFontResolveKey(nameOrPath)` | 宣言名 → storage / パス / (WINVER) GDI 名 → `@gdi:` キー |
| | `TVPFontNameKnown(name)` | 名前が解決可能か |
| face | `TVPFontAcquireFace` / `TVPFontReleaseFace` | 単一 face の取得 (名前解決込み・レジストリ共有) |
| 連鎖 | `TVPFontAcquireFaceChain(csv)` / `TVPFontReleaseFaceChain` | カンマ区切りフォールバック連鎖 (空=既定フェイス) |
| | `TVPFontChainCount` / `TVPFontChainFaceAt` | 連鎖内 face の borrow 参照 |
| | `TVPFontChainFaceForChar(chain, cp, preferLast)` | コードポイント収録 face の選択 (絵文字は preferLast=true) |
| メトリクス | `TVPFontGetLineMetrics(face, px, &out)` | ascent/descent/lineGap/underline/strikeout/unitsPerEm |
| グリフ | `TVPFontGetGlyphIndex(face, cp)` | cp→glyph id (0=未収録) |
| | `TVPFontGetGlyphMetrics(face, gid, px, bold, italic, &out)` | 合成 bold/italic 込み advance/bearing (描画一致) |
| | `TVPFontGetGlyphOutline(face, gid, bold, italic, sink)` | アウトライン分解 (**フォントユニット・y-up**。px/UnitsPerEm でスケール) |
| | `TVPFontGetGlyphBitmap(face, gid, px, color, bold, italic, &out)` | GRAY (8bit) / BGRA (カラー絵文字・前乗算)。バッファは次のグリフ取得まで有効 |
| シェイピング | `TVPFontShapeLine(chain, text, px, baseDir, sink)` | 1 行を BiDi+itemize+HarfBuzz で視覚順の整形済みグリフ列に |
| 検索 | `TVPFontQueryFaces(params, sink)` | 名前+weight/slant+script+収録文字のランク付き検索 |
| | `TVPFontGetFaceInfo(nameOrPath, &out)` | 単一フォントの SFNT メタデータ |
| ThorVG 連携 | `TVPGetFontTvgBridge()` | ThorVG "gw" ローダ用ブリッジ (`TvgGwBridge*`) |

設計メモ:

- face はレジストリで共有キャッシュされる。`TVPFontAcquireFace` は
  keep-alive 参照を増やすだけで、本体 drawText (`Font.rasterizer=2` 相当)
  と同じ face 実体を共有する。
- `TVPFontShapeLine` の出力 (`tTVPFontShapedGlyph`) は視覚順・baseline ペン
  位置。`FaceIndexInChain` → `TVPFontChainFaceAt` で対応 face を得る。
- アウトラインはサイズ非依存 (FT_LOAD_NO_SCALE)。ベクタ消費者は
  `pixelSize / LineMetrics.UnitsPerEm` 倍で変換する。
- bitmap-strike のみのフォント (CBDT/sbix カラー絵文字等) が `Font.face` に
  **直接指定**された場合も FreeType 経路はクラッシュ/例外にしない:
  ascent/underline/strikeout は units_per_EM==0 ガード付きで strike メトリクス
  から近似し (`FreeType.h`)、グリフは FT_LOAD_COLOR でビットマップ読込を許可
  して BGRA 縮小経路に乗せる (`FreeType.cpp` LoadGlyphSlotFromCharcode)。
  遅延ロードされた Noto Color Emoji は `Font.getList` に載るため、列挙名を
  そのまま face に設定するケースは普通に起こる。
- drawText の baseline (ascent) は FreeType 経路 (`Font.rasterizer=0`) と
  glyphware 経路で「`ascender × ppem / unitsPerEm` の切り捨て」に統一している。
  FreeType が `size->metrics.ascender` に格納する値は FT_PIX_CEIL (切り上げ)
  済みなのでそのまま使ってはいけない (最大 1px 下にずれる)。glyphware の
  `LineMetrics` は丸め前素材として `ascenderUnits` / `ppemY` を公開し、
  `GlyphwareFontRasterizer::ApplyFont` がこれで ascent を再計算する。
- 検索対象は「fonts.json 宣言 + 実行時登録 + 使用済みキー」。システム
  フォント全列挙 (allowSystem) は未対応 (将来拡張)。
- **生成フロー**: エクスポートは `FontServiceIntf.h` の `TJS_EXP_FUNC_DEF` と
  `/*[*/ ... /*]*/` マーカーから `common/base/gen_tpstub.py` が生成
  (`FuncStubs.cpp` / `tp_stub/tp_stub.{h,cpp}` → plugins/tp_stub へコピー)。

## ThorVG "gw" テキストローダ連携 (Elements / layerExVector)

ThorVG フォーク (wtnbgo/thorvg) の **gw ローダ (`TVG_LOADER_GW`)** は、
フォント/メトリクス/シェイピング/アウトラインをホスト注入の `TvgGwBridge`
(`inc/thorvg_gw_bridge.h`) から供給する。krkrz は `KRKRZ_USE_GLYPHWARE` 時に
gw ローダを選択し、`GlyphwareTvgBridge.cpp` が glyphware で全 I/F を提供する。
これで Elements / layerExVector のテキストが本体と同じフォントエンジンで
描画され、thorvg 内蔵 FT/HB スタック (と harfbuzz FetchContent) が不要になる。

- レイアウト/折返し/整列は FT ローダと同一ロジック (フォーク内で並置維持。
  レイアウト修正は ft/gw 両ローダへ適用する規約)。
- Elements の計測系 (`glyph_layout`) も GW ビルドでは同ブリッジ実装
  (`elements/lib/src/support/glyph_layout_gw.cpp`) に切り替わる。
- **フォント登録はホストキー渡し (ゼロコピー)**: Elements は
  `tvg::Text::load(storageKey)` / `register_font(key)` で登録し、ブリッジ
  `openFaceByKey` が glyphware レジストリの共有 Face (FontStream 共有バッファ)
  を返す。WINVER 埋め込みフォントも `resource://` キーで登録され、本体
  drawText と 1 バッファ共有になる (elements_gallery 実測で約 7MB 削減)。
  キー共有 face は他消費者がサイズを変えるため、ブリッジの
  shapeRun/glyphAdvance は pixel size=unitsPerEm を毎回張り直す
  (フォントユニット出力の前提維持)。
- **thorvg を独自に静的リンクするプラグイン** (layerExVector 等) は DLL 内に
  thorvg のグローバルを別途持つため、プラグイン初期化で
  `tvgGwSetBridge((const TvgGwBridge*)TVPGetFontTvgBridge())` を呼んで自分の
  コピーへブリッジを注入する (layerExVector の `initThorvg` が実例)。
  `GdiPlus.loadFont` は Storages パス (`resource://` 含む) をそのまま受ける。

## 埋め込み方針 (現状)

- **exe 埋め込み (resource/)**: 最小限 = 日本語 (Noto Sans JP) + 英字 (Roboto)
  + モノクロ絵文字 (Noto Emoji) + アイコン (elements_basic)。
- **data/ 外だし (fonts.json)**: カラー絵文字 (Noto Color Emoji)。案件では
  中国語 (繁/簡) や日本語バリエーション等も data/fonts.json に足す想定。

## 検証手法メモ

- バイト共有: 同一フォントを `Font.rasterizer=0` と `=2` (および Elements)
  で使用後、`Storages.dumpFileCacheList()` にエントリが 1 件だけ載ることを確認。
- 開かず判定: 起動直後 (`-nostartup`) に `Font.queryFonts(%[containsText:…])`
  を実行し、FileCache が 0 entries のままであることを確認。
- 描画パリティ: `data/text_font` の TextRasterizerScene (FreeType/GDI/glyphware
  切替) と `data/elements_gallery` の実画面 pixel diff。

## 未対応 (計画)

- 収録範囲 (coverage) を使った**言語別フォールバックの自動選択** (現状は連鎖の
  先頭から glyph を持つ face が勝つ従来動作)。
- 圧縮 cmap/bitset による包含判定の最適化。
- `TVPGetAllFontList` へメタデータ名を合流 (設定UIのフォント一覧反映)。
- システムフォント全列挙 (`allowSystem`) の検索統合。
