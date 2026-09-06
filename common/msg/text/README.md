# メッセージ定義の生成 (gen_messages.py)

吉里吉里Z のエンジンメッセージ(TJS エラー / TVP 例外・ログ)を **1 つの CSV** から
各プラットフォーム向けの生成物へ変換する。

## 源 (source of truth)

**`messages.csv`** (UTF-8, LF)。列:

| 列 | 説明 |
|----|------|
| `section` | `tjs` = TJS 言語エラー / `tvp` = TVP 共通 / `tvp_win32` = TVP Win32 系。decl の出力先を決める |
| `id` | メッセージ名シンボル (例 `TVPCannotOpenStorage`)。C++ から `TVPThrowExceptionMessage(id, ...)` で使う |
| `ja` / `en` / `chs` | 各言語のメッセージ。**普通の文字列**(引用符はそのまま `"`。CSV 標準クォートで囲われる)。制御文字は `\n` `\r` `\t` トークン、literal backslash は `\\` |
| `opt` | 空 / `CRLF` / `ANSI`。同一 id で改行コードや文字コード別のバリアントを持つとき(win32 のみ #ifdef で切替) |
| `flags` | 空 / `nodecl`。`nodecl` は decl(`TVP_MSG_DECL_NULL`)を**生成しない** — `common/msg/MsgIntf.h` で手管理宣言済みの id 用(二重定義回避) |

`%1` `%2` … は `TVPFormatMessage` のプレースホルダ。

## 生成

```sh
python gen_messages.py            # messages.csv から全生成物を出力(各所へ直接書き込み)
python gen_messages.py --check    # 生成せず、既存生成物との差分だけ報告(検証用)
```

Python 標準ライブラリのみ。**Excel / Win32::OLE / Perl は不要**。どの OS でも走る。

## 生成物 (すべて編集禁止 — CSV を直せ)

| 出力 | 用途 |
|------|------|
| `common/tjs2/tjsErrorInc.h` | TJS エラーの `TJS_MSG_DECL_NULL` (section=tjs) |
| `common/msg/MsgIntfInc.h` | TVP 共通の `TVP_MSG_DECL_NULL` (section=tvp) |
| `win32/msg/MsgImpl.h` | TVP Win32 の `TVP_MSG_DECL_NULL` (section=tvp_win32) |
| `resource/messages{,-en,-chs,-cht}.json` | SDL/generic 用。位置配列 JSON。実行時 `generic/msg/MsgLoad.cpp` が index で読む。**cht 列が空の行は chs へフォールバック** |
| `win32/vcproj/string_table_{jp,en,chs}.rc` + `string_table_resource.h` | WINVER 用 Win32 STRINGTABLE (UTF-16LE+BOM+CRLF)。`win32/msg/MsgLoad.cpp` が `LoadString` で読む |
| `generic/msg/MsgLoad.cpp` / `win32/msg/MsgLoad.cpp` | enum + ロード関数 |

## 設計メモ

- **JSON はプレーン**。旧 `gentext_generic.pl` は `"` を `&quot;` に HTML エスケープしていたが、
  実行時に誰もデコードせず SDL 等で `&quot;` がそのまま表示されるバグだった。本生成器は
  正しい JSON エスケープ(`"`→`\"`)で出力する。RC は RC エスケープ(`"`→`""`)。
- `messages.json` は **キー無し位置配列**(loader が enum index で参照)。人が編集するのは
  この JSON ではなく **`messages.csv`**。順序整合は生成器が保証する。
- **WINVER は現状 `.rc`+`LoadString`**、SDL/generic は `messages.json`。将来 WINVER も
  JSON ロードへ統一し `.rc` 経路を撤去する予定。
- 旧 `Messages.xlsx` / `gentext*.pl`(Excel+Win32::OLE+Perl)は本 Python 生成器へ置換・撤去。
  `Messages.xlsx` は歴史的参照としてのみ残置(源ではない)。
