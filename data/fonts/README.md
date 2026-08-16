# data/fonts — 外だしフォント (fonts.json 宣言・遅延ロード)

ここに置いたフォントは `../fonts.json` で宣言され、名前だけ起動時に登録・
実ファイルは初回使用時に遅延ロードされる (doc/FontEngine.md 参照)。
追加/削除したら生成器で fonts.json を更新すること:

```
python tools/fontgen/gen_fonts_json.py \
    --fonts-dir src/core/data/fonts --root src/core/data --out src/core/data/fonts.json
```

## 同梱フォントとライセンス

すべて SIL Open Font License 1.1 (OFL)。

| ファイル | family | 用途 | 出典 |
|---|---|---|---|
| notocoloremoji.ttf | Noto Color Emoji | カラー絵文字 (CBDT) | https://github.com/googlefonts/noto-emoji |
| notosansarabic-regular.ttf | Noto Sans Arabic | アラビア文字 (RTL/連結) | https://github.com/notofonts/arabic |
| notosanshebrew-regular.ttf | Noto Sans Hebrew | ヘブライ文字 (RTL) | https://github.com/notofonts/hebrew |

他スクリプトを足す場合も Noto ファミリ (https://notofonts.github.io/ の
per-script リポジトリ、`fonts/<Family>/hinted/ttf/`) から取得すると
見た目が Noto Sans JP / Roboto (embedded) と揃う。候補: Noto Sans Thai /
Noto Sans Devanagari / Noto Sans KR / SC / TC など。
