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

- **ラスタライザ**: 番号は全ビルド固定 — `FONT_RASTER_FREE_TYPE`(0) /
  `FONT_RASTER_GDI`(1、WINVER のみ搭載) / `FONT_RASTER_GLYPHWARE`(2)
  (`LayerBitmapImpl.cpp`)。未搭載番号の指定は FreeType(0) へフォールバック
  (読み戻しは実効値)。既定は WINVER=GDI・他=FreeType (不変)。glyphware
  ラスタライザはグリフ生成のみ差し替えで cell-stepping/影/縁取り/下線/
  VS15-16 は classic 経路を流用。
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
| | `TVPFontAcquireFaceInstance(nameOrPath, coords, n)` | **専用** face (非共有) を開き軸座標を適用。解放は `TVPFontReleaseFace` |
| | `TVPFontGetFaceData(face, &data, &size, &faceIndex)` | face の SFNT バイト列 (共有バッファ) の直接参照。`@gdi:` キーでも取れる |
| 連鎖 | `TVPFontAcquireFaceChain(csv)` / `TVPFontReleaseFaceChain` | カンマ区切りフォールバック連鎖 (空=既定フェイス) |
| | `TVPFontChainCount` / `TVPFontChainFaceAt` | 連鎖内 face の borrow 参照 |
| | `TVPFontChainFaceForChar(chain, cp, preferLast)` | コードポイント収録 face の選択 (絵文字は preferLast=true) |
| メトリクス | `TVPFontGetLineMetrics(face, px, &out)` | ascent/descent/lineGap/underline/strikeout/unitsPerEm |
| グリフ | `TVPFontGetGlyphIndex(face, cp)` | cp→glyph id (0=未収録) |
| | `TVPFontGetGlyphMetrics(face, gid, px, bold, italic, &out)` | 合成 bold/italic 込み advance/bearing (描画一致・グリッドフィット) |
| | `TVPFontGetGlyphMetricsEx(face, gid, px, bold, italic, mode, &out)` | `TVP_FONT_METRICS_HINTED`/`UNHINTED`/`UNSCALED`。**組版エンジンは UNHINTED か UNSCALED を使う** |
| | `TVPFontGetGlyphOutline(face, gid, bold, italic, sink)` | アウトライン分解 (**フォントユニット・y-up**。px/UnitsPerEm でスケール) |
| | `TVPFontGetGlyphBitmap(face, gid, px, color, bold, italic, &out)` | GRAY (8bit) / BGRA (カラー絵文字・前乗算)。バッファは次のグリフ取得まで有効 |
| | `TVPFontGetColorLayers(face, gid, px, sink, clipBox)` | COLR (v0/v1) を「アウトライン+変換+塗り」のレイヤー列に展開。**消費側のラスタライザでベクタ描画**する用 (ビットマップ絵文字は対象外) |
| シェイピング | `TVPFontShapeLine(chain, text, px, baseDir, sink)` | 1 行を BiDi+itemize+HarfBuzz で視覚順の整形済みグリフ列に |
| 可変軸 | `TVPFontGetVarAxes(face, out, maxCount)` | fvar 軸 (tag/min/default/max) の列挙。戻り値は総数 |
| | `TVPFontSetVariations(face, coords, n)` | 軸座標の設定。**face の状態**なので共有 face に使わないこと |
| 検索 | `TVPFontQueryFaces(params, sink)` | 名前+weight/slant+script+収録文字のランク付き検索 |
| | `TVPFontGetFaceInfo(nameOrPath, &out)` | 単一フォントの SFNT メタデータ |
| ThorVG 連携 | `TVPGetFontTvgBridge()` | ThorVG "gw" ローダ用ブリッジ (`TvgGwBridge*`) |

設計メモ:

- face はレジストリで共有キャッシュされる。`TVPFontAcquireFace` は
  keep-alive 参照を増やすだけで、本体 drawText (`Font.rasterizer=2` 相当)
  と同じ face 実体を共有する。
- **ピクセルサイズ・変形・可変軸座標は face の状態**であり、共有 face では
  他の利用者がいつでも書き換える。グリフ取得系が毎回 `pixelSize` を受け取るのは
  このためで、利用者は取得のたびにサイズを指定する (ブリッジも同様に張り直す)。
  可変軸のように「自分専用の設定を保ちたい」場合は
  `TVPFontAcquireFaceInstance` で専用 face を開く (バイト列は共有のまま)。
- 自前でシェイパを走らせる利用者 (minikin のようにフォントデータから独自に
  hb_face を作るもの) は `TVPFontGetFaceData` で SFNT バイト列を借りる。
  advance は必ず `TVPFontGetGlyphMetricsEx` の UNHINTED/UNSCALED で取ること
  (HINTED は整数ピクセルに丸まるので、文字を並べるほど位置がずれる)。
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

## バリアブルフォント (可変軸) の全体展開 【P0〜P5 実装済 (2026-08-24)】

P0 (named instances) / P1 (tTVPFont + 描画経路 + TJS) / P2 (fonts.json) /
P3 (照会 API) / P4 (自動マッピング) / P5 (Elements / gw ブリッジへの軸伝搬 =
`#tag=val` サフィックス、下記) まで実装済み。TJS からの使い方:

```tjs
Font.rasterizer = 2;                 // drawText で使う場合 (drawShapedText 系は常時有効)
layer.font.weight = 700;             // wght 軸 (100-900、void で解除)
layer.font.variations = "wdth=87.5"; // 汎用軸 (正規化されて保持される)
Font.getVarAxes("フォント名");        // [%[tag, name, min, default, max]]
Font.getFontInfo("フォント名");       // axes / namedInstances 入り辞書
Font.defaultUseVarStyle = true;      // bold/italic を軸で表現 (オプトイン)
```

fonts.json 宣言 (P2):

```json
{ "file": "myfont.ttf", "family": "MyFont SemiBold", "instance": "SemiBold" }
{ "file": "myfont.ttf", "family": "MyFont Narrow",   "axes": { "wdth": 75 } }
```

実装時の確定事項 (設計からの差分含む):

- **適用順 (勝ち順)**: fonts.json の named instance → 同 axes 直書き →
  `Font.weight` → `Font.variations` (同名軸は後者が上書き)。重ね掛けは
  face の現在座標へ合成してから private face を作るので、宣言インスタンスの
  他軸を潰さない (`TVPGlyphwareFaceWithVariations` の合成規則)。
- **queryFonts は軸を返さない** (「宣言 entry を開かず判定する」性質を守る
  ため)。軸情報は `getFontInfo` / `getVarAxes` (解決のために開く API) で返す。
- **自動マッピング (P4)** の軸有無判定は連鎖の **primary face 基準**で、
  合成 bold/italic の無効化は連鎖全体に効く (フォールバック face 単位の
  細分化はしない)。bold は wght=700、italic は slnt=-10 → ital=1 の順。
- 軸違いのグリフ誤ヒット防止はフォントハッシュ
  (`tTVPNativeBaseBitmap::ApplyFont` の FontHash) に Weight/Variations を
  含めることで実現。`tTVPFont::operator==` にも両方入っている。
- 実装ファイル: 正規化/パース = `common/visual/FontVariations.{h,cpp}`、
  private face LRU (既定 32) と宣言軸適用 = `common/visual/GlyphwareHost.cpp`、
  経路適用 = `GlyphwareFontRasterizer.cpp` (drawText) /
  `GlyphwareText.cpp` `PrepareContext` (shaped 系)。

### `#tag=val` サフィックス表記 (P5: Elements / gw ブリッジへの軸伝搬)

フォント名/キーに `#tag=val[,tag=val...]` を後置すると、その名前が指す
フォントの**可変軸インスタンス**を意味する (例: `"MyFont#wght=700"`、
`"MyFont#wght=700,wdth=75"`)。次の場所で一様に通る:

- `Font.face` のトークン (フォールバック連鎖の各要素に個別指定可)
- Elements JSON の `"font"` (label / text_area など。text_area の折り返し計算
  = BlockTextBackend も同じインスタンスで測る)
- gw ブリッジのホストキー (`tvg::Text::load` に渡る storage パス表記)

**無指定軸の既定 (wght=400 正規化)**: 可変フォントを wght 未指定で参照した
場合 (サフィックス無し、fonts.json 宣言にも wght 無し、下記の登録も無し)、
**fvar の既定インスタンスではなく wght=400 相当**で表示する (CSS の
font-weight 既定に合わせた規則)。fvar 既定が Regular でないフォント
(Noto VF = Thin 等) も、VF 1 本だけの登録で無指定が Regular 相当に読める。
wght 軸を持たない face は不変、fvar 既定が既に 400 の face は共有 face の
まま (無駄なインスタンス化はしない)。明示指定は常に勝つ。

無指定時の既定を変えたい場合は `Font.setDefaultVariations(name, axes)` で
名前単位に登録する (`axes` = `"wght=300,wdth=87.5"` 形式、void/空文字列で
解除、現在値は `getDefaultVariations`)。優先順は弱い方から:
**wght=400 正規化 < setDefaultVariations < fonts.json 宣言 (axes/instance) <
`#suffix` / Font.weight / Font.variations** (同名軸は強い方が勝つ)。
elements 単体 (FT ローダ) ビルドには同じ規則の
`cycfi::elements::set_default_variations()` がある (gw ビルドではエンジン側が
一元管理するため elements 側は no-op)。

同じファミリ名で static 版と VF 版の両方が登録されている場合 (例:
`NotoSansSC-Regular.otf` と `NotoSansSC-VF.ttf` を同じ "Noto Sans SC" で登録)、
サフィックス**無し**の解決は従来どおり先着エントリ (= static を先に登録して
おけば既定の見た目は不変)、サフィックス**付き**の解決は可変フォントの
エントリを優先する (static を掴むと軸が黙って効かないため)。可変判定は
elements 登録時に行う — バイトが手元にあるビルドは sfnt の 'fvar' 直接判定、
gw ビルドはブリッジ `isVariable` (glyphware face の axes 有無) で判定する。

解決は `TVPGlyphwareFaceForToken()` (`GlyphwareHost.cpp`) に一本化:
ベース名を fonts.json 宣言 → storage パス → GDI 名の順で解決し、宣言軸 →
サフィックス軸の順で private face LRU から合成インスタンスを得る。fonts.json
宣言に軸が付いていた場合もサフィックスは**その上に重なる** (同名軸は
サフィックスが勝つ)。カンマが連鎖/ファミリ列挙の区切りと衝突する問題は
「`tag=数値` 形状のトークンは直前の `#` 付きトークンの続き」とみなす結合で
回避している (`TVPGlyphwareBuildChain` と elements 側 `match_ex` の双方)。

thorvg 側 (fork) は `名前#tag=val` を独立フォントとして登録する。FT ローダ
ビルド (elements 単体アプリ等) では file IO が無効でも、メモリ登録済みの
ベースフォントから `LoaderMgr::font()` がインスタンスを派生生成するので、
同じ JSON がエンジン外 (elements_console 等) でも同一表示になる。

### 言語連動フォント置換 (font_languages — Elements 層) 【2026-08-24 実装】

文字体系ごとの別フォント (Noto Sans JP/TC/SC 等) を持つ多言語 UI で、
共有コードポイントの漢字が常に authoring 時の family の字形で出てしまう
問題への対応。**言語コード → {family→family の置換表, fallback チェーン}**
を宣言しておくと、表示言語に応じてフォント解決時に family を差し替える。

- **実装は elements 側に一元化** (`lib/src/support/font.cpp`:
  `set_font_language_table` / `set_font_language` / `substitute_font_family`)。
  `font::font(font_descr)` 内で families の各トークンへ適用するため、widget を
  作り直さず invalidate だけで言語切替に追従する。`#tag=val` サフィックスは
  温存 (JP/TC/SC が同軸 VF ならウェイトが揃う)。別名 (registerFont の
  エイリアス) もトークンとして置換できる
- **適用言語の優先順**: widget 明示 `"locale"` (`font_descr::_lang` 経由、
  label/anchored_text/rich text run に配線済) > `set_language()` の現在言語。
  将来の「run 単位の多言語混在」も `_lang` の拡張で対応できる
- **宣言の入口は 3 系統**: 画面 JSON / app.jsonc の top-level
  `"font_languages"` (elements_modal がパース、言語単位マージ) /
  ホスト直登録 `elements_modal::apply_font_languages_json()` (krkrz の
  `Dialog.fontLanguages` プロパティはこれを呼ぶ)
- **fallback**: 表に宣言があれば `set_language` 時に theme の既定
  families チェーンをその言語用の並びへ swap (無い言語では復元)
- **既知の制限**: `text_area` はビルド時にフォント確定 (表示中の言語切替に
  非追従、開き直しで反映)。置換表はプロセスグローバル (異なる表を持つ画面の
  同時表示は非対応)
- 詳細: [ElementsDialog.md](ElementsDialog.md) の fontLanguages 節 /
  elements リポ `external/elements_modal/README.md` / ヘッドレステスト =
  elements_console `tests/font_language_test.cpp`。**対象は Elements 経路のみ**
  — `Layer.drawText` / `Font.face` 側の言語別フォールバックは下の
  「未対応 (計画)」のまま

以下は設計時の記録 (決定事項・データモデルの根拠)。

現状 (2026-08-20) の到達点は「**エンジンとプラグイン公開 API は VF 対応済み、
TJS のフォントパラメータとして指定する口が無い**」。

- glyphware: `Descriptor::axes` (fvar 軸列挙)、`Face::setVariations` /
  `variations` / `axisRange`。`FT_Set_Var_Design_Coordinates` の後に
  `hb_ft_font_changed()` を呼ぶのでシェイパ側も同期する (`src/Face.cpp`)。
- tp_stub: `TVPFontGetVarAxes` / `TVPFontSetVariations` /
  `TVPFontAcquireFaceInstance` (専用インスタンス)。
- krkr_richtext: 独自 FreeType 実装は撤去済みで、`HostFontBackend` が可変軸を
  持つフォントを検出すると `TVPFontAcquireFaceInstance` に切り替える。TJS へは
  `RichText.Style.fontWeight` (wght) / `fontWidth` (wdth) として露出。
- **欠けているもの**: `tTVPFont` に軸フィールドが無いため、`Layer.font` /
  `drawText` / `drawShapedText` 系 / Elements から軸を指定できない。
  `fonts.json` にも軸宣言が無く、`getFontInfo` / `queryFonts` も軸を返さない。

### 決定事項 (2026-08-20)

| # | 論点 | 決定 |
|---|---|---|
| ① | 適用ラスタライザ | **glyphware 経路 (`rasterizer=2`) のみ**。GDI / 旧 FreeType 経路には実装しない (軸指定は無視 + 警告)。**最終的にはラスタライザを glyphware へ一本化する**方針なので、旧経路への二重実装は行わない (一本化自体は別議題) |
| ② | 指定の粒度 | `font.weight` (一級・100-900) + `font.variations` (汎用軸) の 2 本立て |
| ③ | bold/italic の軸マッピング | 既定 OFF + グローバルスイッチでオプトイン |
| ④ | キャッシュ | 軸値を量子化 + 軸付き face は LRU |
| ⑤ | `fonts.json` | 軸直書き (`axes`) と named instance 名 (`instance`) の両対応 |
| ⑥ | フォールバック連鎖 | **同名軸を持つ face にだけ適用** |

### データモデル

`tTVPFont` (`common/visual/tvpfontstruc.h`) に 2 つ足す:

```
tjs_int Weight = -1;      // 100-900、-1 = 未指定
ttstr   Variations;       // 正規化済み "wdth=87.5,wght=700"
```

- `Weight` と `Variations` を分けるのは、**weight が「軸」と「face 選択条件」の
  二役**を持つため。VF に `wght` 軸があれば軸へ、無ければレジストリ検索での
  face 選択 (`tTVPFontQueryParams::Weight`) へ回す。`Variations` に `wght` が
  明示されていればそちらが優先。
- `Variations` は setter で正規化する: タグは 4 文字小文字、**タグ昇順**、
  重複は後勝ち、量子化 (下記) 済み、**軸の既定値と一致する項目は落とす**。
  正規化しておくと文字列比較だけでキャッシュキーとして使える。
- `operator==` に両方を含める。さらに `tTVPFontAndCharacterData` /
  `tTVPFontHashFunc` (`common/visual/CharacterData.h`) にも反映する。
  **ここを忘れると軸違いの同じ文字がグリフキャッシュで誤ヒットする**。

### 軸値の量子化とキャッシュ

軸をアニメーションさせるとグリフキャッシュと face インスタンスが際限なく
増えるので、正規化の段階で丸める。

- 既定の丸め幅: `wght` = 1、その他の軸 = 0.5。
- 軸付き face は `TVPGlyphwareOpenPrivateFace` (`GlyphwareHost.h`、既存) を
  ラップした LRU キャッシュで再利用する。キーは
  `(loaderKey, faceIndex, 正規化 Variations)`。既定上限 32 face。
- **共有 face (`TVPFontAcquireFace` / レジストリ) に `setVariations` しない**
  規約は従来どおり。適用すべき軸が空になった face は共有 face をそのまま使い、
  無駄なインスタンス化をしない。

### 適用経路

1. **drawText**: `GlyphwareFontRasterizer::RebuildChain()`
   (`common/visual/GlyphwareFontRasterizer.cpp`) で連鎖構築時に軸を適用。
   `TVPGlyphwareBuildChain()` に軸付きオーバーロードを足す。
2. **drawShapedText 系**: `TVPShapedTextStyleFromFont()`
   (`common/visual/GlyphwareText.cpp:18`) が `tTVPFont` → `tTVPShapedTextStyle`
   の唯一の変換点なので、`tTVPShapedTextStyle` に軸を足して
   `PrepareContext()` の連鎖構築へ渡せば `drawShapedText` /
   `drawShapedTextArea` / `measureShapedText` / `shapedTextCount` が一斉に通る。
3. **連鎖への適用規則 (⑥)**: 各 face について「その face が実際に持つ軸のうち、
   指定と同名のものだけ」を適用する。日本語フォントと絵文字フォントで軸構成が
   違うのが普通なので、全適用も無適用も不自然になる。
4. **Elements / ThorVG gw**: フォント登録がホストキー渡しなので、キー表記に軸を
   埋める (`<key>#wght=700` 等) か Elements 側 style に axes を足すかを実装時に
   決める。段階としては後回し (P5)。

### bold / italic → 軸の自動マッピング (③)

- グローバルスイッチ `Font.defaultUseVarStyle` (既定 `false`)。
- `true` のとき、`TVP_TF_BOLD` かつ `wght` 軸あり → `wght=700`、
  `TVP_TF_ITALIC` かつ `slnt` / `ital` 軸あり → `slnt=-10` / `ital=1`。
- 明示された `Weight` / `Variations` があればそちらが勝つ。
- 軸で太らせた/傾けた場合は**合成ボールド/イタリックを無効化**する
  (二重適用の防止)。
- 既定 OFF なのは既存案件の見た目が変わるため。①(a) の間は影響範囲が
  `rasterizer=2` の利用者に限られるので、一本化のタイミングで既定を見直す。

### fonts.json 宣言インスタンス (⑤)

```json
{ "name": "MyFont SemiBold", "file": "...", "axes": { "wght": 600 } }
{ "name": "MyFont SemiBold", "file": "...", "instance": "SemiBold" }
```

- `instance` は fvar の **named instance** 名。glyphware が現状 named instance
  を読んでいないので、`Descriptor` に `namedInstances` を足す
  (`FT_MM_Var::namedstyle` を拾うだけ) 必要がある。
- 宣言インスタンスは「名前 → キー + 軸」の対応になるため、名前解決の戻り値に
  軸を添える必要がある。`TVPGlyphwareResolveFontKey()` は `std::string` を
  返す既存 API なので、**軸付きの内部解決関数を別に足して既存 API は互換維持**
  する。
- 案件側は「名前を指すだけ」で VF を使えるので、`font.weight` /
  `font.variations` に触れなくても恩恵が届く。

### TJS API 追加

- `Font.weight` (インスタンス、100-900、既定 `void`/-1)
- `Font.variations` (インスタンス、`"wght=700,wdth=87.5"`)
- `Font.defaultUseVarStyle` (静的、既定 false)
- `Font.getVarAxes(nameOrPath)` → `[%[tag, min, default, max]]`
- `Font.getFontInfo` / `Font.queryFonts` の返す辞書に `axes` /
  `namedInstances` を追加 (`LayerIntf.cpp:10151` の
  `TVPFontFaceInfoToDictionary`)

### 段階分け

| Phase | 内容 | 規模 |
|---|---|---|
| P0 | glyphware に `namedInstances` を追加 | 小 |
| P1 | `tTVPFont` 拡張 + 正規化 + キャッシュキー + 軸付き face LRU + drawText/shaped 経路への適用 + `font.weight` / `font.variations` | **中 (本丸)** |
| P2 | `fonts.json` の `axes` / `instance` + 名前解決への軸伝搬 | 小〜中 |
| P3 | `getVarAxes` / `getFontInfo` の軸露出 | 小 |
| P4 | bold/italic 自動マッピング (オプトイン) | 小 |
| P5 | Elements / gw ブリッジへの軸伝搬 | 中 |
| — | (別議題) ラスタライザの glyphware 一本化 | 大 |

### 非対応の明示

GDI ラスタライザ (WINVER 既定) と旧 FreeType ラスタライザ (非 WINVER 既定) では
軸指定は**効かない**。黙って無視すると原因が分からないので、軸指定付きの描画が
これらの経路に来たら**起動後 1 回だけ警告ログ**を出す。

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
  先頭から glyph を持つ face が勝つ従来動作)。Elements 経路は宣言式の
  言語連動フォント置換 (上記 font_languages) で対応済みだが、
  `Layer.drawText` / `Font.face` 側は未対応のまま。
- 圧縮 cmap/bitset による包含判定の最適化。
- `TVPGetAllFontList` へメタデータ名を合流 (設定UIのフォント一覧反映)。
- システムフォント全列挙 (`allowSystem`) の検索統合。
