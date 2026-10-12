# リリース時の exe への埋め込み (検討メモ)

標準ツールのリリーサ (krkrz_tools、Windows 専用) が、配布用の exe に対して何を
どこへ書き込むかの検討。**状態: 実装済み (2026-10-09)**。リリーサは krkrz_tools の `krkrrelease`
(`libs/release`)。本体側の «結合 xp3 の探索の高速化» と «SDL 版での結合 xp3» も実装済み。
利用者向けの説明は umbrella の `doc/guide/Releaser.md`。

実装で分かったこと:

- WINVER の exe は、オプション領域 (`win32/vcproj/optionarea.txt`) が空のため、リソースコンパイラが
  TEXT/139 を落としていて、exe に無い。リリーサは書くときに足す (本体は `FindResource` で探すので読める)。
  WINVER / SDL の見分けは «TEXT リソース (141 / 143) があれば WINVER»
- SDL の BINARY/CONFIG.CF (`resource/config.cf`) には本体の既定値 (`padinterval`) が入っているので、
  上書きせずに重ねる
- オプションの行は狭い文字列のまま `ttstr` に変換されるので、ASCII 以外の値は `name="\xNN..."`
  (TJS の文字列リテラル) にして書く (本体同梱の config.cf と同じ書き方)
- SDL の exe のアイコンは 107 ではなく `MAINICON` (CMake が生成する `.rc` で付ける。`KRKRZ_WIN_ICON` を指定しなければ
  WINVER と同じ既定アイコン)。リリーサは既存のグループを全部差し替え、無ければ WINVER は 107、SDL は `MAINICON` として足す。
  SDL3 はウィンドウのアイコンに exe の先頭のアイコングループを使うので、名前は問わない

## 吉里吉里2 との違い

吉里吉里2 の exe はバイナリの末尾近くに目印を持ち、リリーサはそこを書き換えていた。

| 目印 | 吉里吉里2 での用途 | 吉里吉里Z (現状) |
|---|---|---|
| `XOPT_EMBED_AREA_` | 埋め込みオプション (この後ろに `.cf` と同じ書式の文字列) | **無い**。オプションは Win32 リソース `TEXT` / `IDR_OPTION` (139) から読む (`win32/base/SysInitImpl.cpp` `TVPGetEmbeddedOptions`) |
| `XCORE_SIG_______` / `XRELEASE_SIG____` | exe に埋め込む電子署名の置き場所 | **無い**。sigcheck プラグインは目印が無ければ exe も «普通のファイル» として全体を検証する (`.sig` 別置き) |
| XP3 のヘッダ (16 バイト境界) | exe への xp3 の結合 | **WINVER・SDL とも対応** (下の «本体側の対応»)。探索順は WINVER が data.xp3 → data.exe → **自分自身** → data フォルダ、SDL が data.xp3 → **自分自身** → data/ |
| `-- TVPSystemSecurityOptions ... --` | セキュリティ設定 (文字列をその場で書き換える) | **同じ** (WINVER / SDL とも。`forcedataxp3` / `acceptfilenameargument` / `disablemsgmap` / `disableapplock`) |

アイコン・バージョン情報は、どちらも通常の Win32 リソース (`ICON` は WINVER が 107 = `IDI_TVPWIN32`、SDL が `MAINICON`、`RT_VERSION` 1。
版情報は CMake が生成する `krkrz_version.rc`)。

## 実際のリリースで行われていること (案件で確認)

吉里吉里Z を使う既存のリリース手順 (独自ツール群) を確認した。要点:

- exe への **xp3 の結合はしていない**。`data.xp3` ほかを exe と同じフォルダに置く。
- 埋め込みオプションは **`TEXT` / 139 リソースを `UpdateResource` で書き換え** (`readencoding=...` に続けて
  `debugwin="no"` など、改行区切りの `名前=値`)。
- セキュリティ設定は **`-- TVPSystemSecurityOptions` の文字列をその場で書き換え** (`forcedataxp3(1)` /
  `acceptfilenameargument(0)` など)。
- アイコン (ICON 107) とバージョン情報 (`RT_VERSION` の FileDescription / Copyright) をリソースで差し替え、
  exe を製品名に改名。
- 署名は **ファイルごとの `.sig` 別置き** (exe・xp3・プラグインとも)。exe への署名埋め込みはしていない。
- ストア向けの DRM ラップ (Steam など) は最後に行い、そのあと `.sig` を作り直す。
- Authenticode (signtool) は使っていない。

## 標準のリリーサで行う処理 (案)

上の実例に合わせ、本体側に新しい目印を足さずに、既にある仕組みだけで行う。

1. **exe をコピーして改名** (元の exe は書き換えない)。
2. **リソースの書き換え** (`BeginUpdateResource` / `UpdateResource` / `EndUpdateResource`):
   - 埋め込みオプション: WINVER は `TEXT` / 139。SDL (Windows) は `BINARY` / `CONFIG.CF` (`resource://` の
     `config.cf`)。どちらも `.cf` と同じ書式の文字列。
   - アイコン: `RT_GROUP_ICON` 107 と、それが指す `RT_ICON` 群を `.ico` から作り直す (複数サイズ)。
   - バージョン情報: `RT_VERSION` 1 の文字列 (ProductName / FileDescription / CompanyName / LegalCopyright /
     FileVersion など) を差し替える。
3. **セキュリティ設定の書き換え**: `-- TVPSystemSecurityOptions ... --` を探し、各項目の数字だけを
   同じ桁数で置き換える (長さが変わらないので PE 構造に触れない。リソース書き換えの後で行う)。
4. **データ**: 既定は «exe と同じフォルダに data.xp3» (xp3 ツール `krkrxp3` で作る)。
   **exe への結合は選択肢** として残す (ファイル 1 つで配りたい場合。WINVER・SDL の Windows / Linux)。
5. **署名**: krkrsign の `.sig` を exe・xp3・プラグインに別置きで作る (今の sigcheck と破損チェックツールで検証できる)。
6. 順序の決まり: リソース書き換え → セキュリティ設定 → (xp3 の結合) → (Authenticode や DRM ラップは利用者が
   ここで行う) → `.sig` 作成。**`.sig` は必ず最後** (後から exe を触ると検証に失敗する)。

`UpdateResource` は exe の末尾に付いたデータ (結合した xp3 や Authenticode の署名) を保持しないので、
結合と署名は必ずリソース書き換えの後に行う。

## 本体側の対応 (実装済み、2026-10-09)

**結合 xp3 の探索** (`common/base/ExecutableOverlay.{h,cpp}`、`XP3Archive.cpp` の `TVPGetXP3ArchiveOffset`):

- 付け足したデータは実行ファイルのイメージの後ろにあるので、ヘッダから **イメージの終わり** を求め、
  そこから先だけを 16 バイト境界で探す。PE はセクションの `PointerToRawData + SizeOfRawData` の最大、
  ELF (リトルエンディアン 32 / 64bit) はプログラムヘッダ・セクションヘッダの表と中身のあるセグメント・
  セクションの終わりの最大。ヘッダを解釈できないときは以前と同じく先頭から探す。
- 以前は先頭から exe 全体を 16 バイトずつ調べていた (結合していない exe でも、data.xp3 が無ければ
  起動のたびに exe 全体を読んでいた)。末尾に新しい印を足す案もあったが、ヘッダから求められるので
  **exe の書式を変えずに済む** こちらにした (既に結合済みの exe もそのまま速くなる)。
- 計測 (同じファイルをキャッシュに載った状態で読む): 15MB の exe で、結合なし 約 29ms → 0.01ms、
  結合あり 約 32ms → 0.5ms 未満。見つかる位置は以前と同じ。
- リリーサは xp3 を **イメージの終わり (ファイルの末尾) から 16 バイト境界に揃えて** 付け足すこと
  (PE のファイル末尾は通常 512 バイト境界なので詰め物は要らないが、念のため揃える)。

**SDL 版での結合 xp3** (`sdl3/environ/stdapp.cpp`): data.xp3 の次、data/ より前に «自分自身» を見る
(Android / iOS / wasm は対象外)。プロジェクトの場所はストレージ層の準備前に決めるので、ファイルを直接
読んで同じ探索を行う。結合した exe の中身は従来どおり `TVPGetXP3ArchiveOffset` が開く (ELF も対応)。

確認: WINVER と SDL (Windows) で xp3 を結合した exe が自分自身から起動すること、結合していない exe が
従来どおり data/ から起動すること。ELF は Linux 版の実行ファイル (64bit) と 32bit の共有ライブラリで
イメージの終わりと結合した xp3 の位置が正しく求まることを確認し、**Steam Deck (SteamOS 3.8、sniper SDK でビルドした Linux 版) で、data フォルダを置かずに xp3 を結合した実行ファイルだけで起動する** ことを確認 (startup.tjs が `.../krkrz>startup.tjs` から読まれる)。
macOS はバンドルと署名の都合で実行ファイルに付け足さない前提なので対象外 (Mach-O は «実行ファイルではない» 扱い)。

## 残りの検討

| 項目 | 内容 | 優先度 |
|---|---|---|
| 埋め込みオプションの置き場所の統一 | WINVER (`TEXT`/139) と SDL (`BINARY`/`CONFIG.CF`) で別。リリーサはどちらにも書けるので、今は揃えなくてよい | 低 |
| 署名の exe 埋め込み | 吉里吉里2 の `XRELEASE_SIG____` 相当。`.sig` 別置きで足りているので **作らない** | — |

## 決めたこと

- [x] «標準のリリーサで行う処理» は上の案でよい (2026-10-09)
- [x] SDL 版でも結合 xp3 を使えるようにする → 実装済み
- [x] 探索の高速化は、末尾の印ではなく «ヘッダからイメージの終わりを求める» 方式で実装済み
