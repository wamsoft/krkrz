# フォントメタデータと遅延ロード (data/fonts.json)

FreeType 系ラスタライザ向けに、**data フォルダのフォントをメタデータで宣言し、
名前だけ先に登録して実ファイルは初回使用時に読み込む**仕組み。exe への大きな
フォント埋め込み(特にカラー絵文字)を避けつつ、フォント名指定で使えるようにする。

## 背景

フォント名(`Font.face`)は `FontSystem` の登録名にしか解決できない
(`FontExists()` で絞り込み、未登録名は既定へフォールバック)。従来は既定フォントを
`resource/` に置き exe へ埋め込んでいたが、カラー絵文字 (Noto Color Emoji, CBDT)
は約 10.7MB と大きく exe を肥大化させていた。

## 仕組み

- **`data/fonts.json`** にフォントを宣言する:
  ```json
  {
    "version": 1,
    "fonts": [
      { "file": "fonts/notocoloremoji.ttf",
        "family": "Noto Color Emoji Regular",
        "scripts": ["Zsye"], "flags": ["emoji","color"],
        "ranges": [[9728,9983],[126976,129791]] }
    ]
  }
  ```
  - `family` は**実 family 名**にすること(遅延ロード後の登録名と一致させるため)。
  - `scripts` は ISO15924 (粗い分類)、`ranges` は cmap の収録コードポイント区間
    (coverage・手軽版)。現状は情報として保持(将来のフォールバック選択用)。
- **起動時** (`FontSystem::InitFontNames` → `LoadFontMetadata`):
  `fonts.json` を picojson で読み、`family`/`aliases` を `LazyFontFiles`
  (name→storage) へ登録する。**この時点では FreeType パースしない**(軽い)。
- **初回使用時** (`FreeTypeFontRasterizer::ApplyFont`):
  指定 face 名が未登録でメタデータ対象なら `FontSystem::EnsureLazyFontLoaded()`
  が `AddExtraFont()` で実ファイルを読み、実 family 名で登録する。以降は通常フォント
  同様に使える。絵文字フォールバック連鎖への追加も同じ経路を通る。

埋め込みリソース (`resource://`) の従来経路も残しており、メタデータに無ければ
そちらを試す(モノクロ絵文字等の同梱分・後方互換)。

## 埋め込み方針 (現状)

- **exe 埋め込み (resource/)**: 最小限 = 日本語 (Noto Sans JP) + 英字 (Roboto) +
  モノクロ絵文字 (Noto Emoji)。
- **data/ 外だし (fonts.json)**: カラー絵文字 (Noto Color Emoji)。案件では
  中国語 (繁体/簡体) や日本語のバリエーション等も data/fonts.json に足す想定。

## メタデータ生成

`tools/fontgen/gen_fonts_json.py` (umbrella、要 fonttools) で data フォルダの
フォントを走査し family/scripts/ranges/flags を算出して `fonts.json` を生成できる:

```
python tools/fontgen/gen_fonts_json.py \
    --fonts-dir src/core/data/fonts --root src/core/data --out src/core/data/fonts.json
```

## 実装

- `common/visual/FontSystem.{h,cpp}` — `LazyFontFiles` / `LoadFontMetadata()` /
  `EnsureLazyFontLoaded()`。
- `common/visual/FreeTypeFontRasterizer.cpp` — `ApplyFont` の face 解決および
  絵文字フォールバックで遅延ロードを起動。

## 未対応 (計画)

- 収録範囲 (coverage) を使った**言語別フォールバックの自動選択**(現状は連鎖の
  先頭から glyph を持つ face が勝つ従来動作)。
- 圧縮 cmap/bitset による「開かずに包含判定」の最適化。
- `TVPGetAllFontList` へメタデータ名を合流(設定UIのフォント一覧反映)。
