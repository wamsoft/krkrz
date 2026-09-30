# Storages のローカルファイル操作 (旧 fstat / systemEx 由来の本体化)

status: **実装済み (2026-09-28)**
対象: WINVER (`win32/`) と SDL・generic (`generic/` / `sdl3/`)
関連: [WindowState.md](WindowState.md) (windowEx 由来の Window / System)

旧 `fstat` / `systemEx` プラグイン (Win32 専用) にしか無かったファイル操作・環境変数などを
本体へ移した。プラグインは SDL / CS 機には無いので、**下回りが全プラットフォームに
揃っているものだけ**を本体 (`common/`) に 1 つの実装で置く。

---

## 1. 方針: 本体は狭い版、プラグインは全部版

下回りが全機種に無い情報 (作成日時 / アクセス日時 / Win32 のファイル属性) は本体に入れない。
本体は取れる範囲の**狭い版**を持ち、同名のメソッドをプラグインが持つ場合は次のどちらかにする。

| 登録の仕方 (プラグイン側) | 使うとき | 該当 |
|---|---|---|
| **IF_MISSING** (本体にあれば登録しない) | 本体版とプラグイン版が同じ仕様 | 大半 (下表) |
| **常に登録** (本体を上書き) | プラグイン版の方が多くを返す | `fstat` / `getTime` / `currentPath` / `dirlistEx` |

「常に登録」のものは、**本体版の返り値がプラグイン版の部分集合**になるよう揃えてある
(キー名も型も同じ。例: どちらも `mtime` は Date)。こうしておけば、プラグインの有無で
スクリプトを書き分けなくて済む (無い情報のキーが無いだけ)。

⚠ 同じキー名で型が違うと互換が壊れる。最初の実装で本体 `fstat` の `mtime` を
FILETIME の整数にしていて、プラグイン版 (Date) 前提の `a.mtime.getTime()` が落ちた。

## 2. TJS API (本体)

### Storages

| 名前 | 本体版 | プラグイン (fstat) を読み込んだとき |
|---|---|---|
| `fstat(storage)` | `%[ size, mtime(Date) ]`。アーカイブ内は `size` だけ | プラグインは `atime` / `ctime` も (上書き) |
| `getTime(target)` | `%[ mtime(Date) ]`。ローカルの実ファイル / フォルダのみ | プラグインは `atime` / `ctime` も (上書き) |
| `currentPath` | 読み取り専用。末尾 `/` のストレージ名。概念が無い環境は `""` | プラグインは代入で変更可 (上書き) |
| `dirlist(dir/)` | `.` / `..` を含まない。アーカイブ内 / `proxy://` 等も列挙可。フォルダが無ければ空配列 | 本体版が使われる (IF_MISSING)。旧プラグイン版は `.` / `..` も返し、フォルダが無いと例外だった |
| `dirtree` / `isExistentDirectory` / `isExistentStorageNoSearchNoNormalize` | | 本体版が使われる (IF_MISSING) |
| `createDirectory` / `removeDirectory` / `moveFile` / `deleteFile` | | 本体版が使われる (IF_MISSING) |
| `copyFile` / `exportFile` / `truncateFile` | | 本体版が使われる (IF_MISSING) |
| `getMD5HashString` / `getTemporaryName` / `clearStorageCaches` | | 本体版が使われる (IF_MISSING) |
| `getLastModifiedFileTime` | FILETIME 整数 (1601 起点・100ns)。フォルダも可 | 本体版が使われる (IF_MISSING) |

**本体に無いもの** (fstat プラグインだけ): `dirlistEx` (size / attrib / 時刻付きの列挙)、
属性の取得・設定 (`getFileAttributes` 等と `FILE_ATTRIBUTE_*` 定数)、時刻の書き込み
(`setTime` / `setLastModifiedFileTime`)、`searchPath`、`getDisplayName`、正規化なし版
(`createDirectoryNoNormalize` / `copyFileNoNormalize`)、`changeDirectory`、`TemporaryFiles`。

`dirlistEx` は一度本体に入れたが外した。本体の列挙の下回り (`TVPGetLocalFolderListAt`) は
名前とフォルダかどうかしか返さず、`%[ name, isDirectory ]` という別物にしかならなかったため。

### System (旧 systemEx)

`readEnvValue` / `writeEnvValue` / `expandEnvString` / `urlencode` / `urldecode` /
`getAboutString`。systemEx 側はすべて IF_MISSING。
レジストリ / DPI / OS バージョン / 既知フォルダ / DLL 検索パス / メッセージポンプは
systemEx に残る。

## 3. 実装

| 層 | ファイル |
|---|---|
| TJS 公開 (Storages) | `common/base/StorageIntf.cpp` (「旧 fstat プラグイン由来」の区画) |
| TJS 公開 (System) | `common/base/SystemIntf.cpp` (「旧 systemEx プラグイン由来」の区画) |
| 列挙の下回り | `TVPGetLocalFolderListAt` (`common/base/StorageIntf.h`)。generic は `iTVPLocalFileSystem::GetListAt`、WINVER は `FindFirstFileW` |
| 時刻の下回り | `TVPLastModifiedFileTime` (WINVER は `GetFileAttributesExW`、SDL3 は `SDL_GetPathInfo`) |

- Date は旧 fstat と同じ換算 (FILETIME → UNIX ミリ秒 → `Date.setTime`) で作る
  (`TVPStoreFileTimeAsDate`)。
- `currentPath` は `std::filesystem::current_path()` を `TVPNormalizeStorageName` に通す。
- `truncateFile` は `std::filesystem::resize_file`。generic の `SetEndOfStorage` は
  論理位置を覚えるだけで実ファイルを縮めないため。
- ⚠ SDL3 の `LastModifiedFileTime` は当初フォルダに 0 を返していた (`SDL_PATHTYPE_FILE`
  限定)。WINVER に合わせてフォルダも返すようにした (`getTime(フォルダ)` 用)。

## 4. 確認 (2026-09-28)

SDL / WINVER (fstat.dll 無し / 有り) の 3 通りで確認。

```
fstat.mtime is Date / == getLastModifiedFileTime        … 3 通りとも OK
getTime(ファイル / フォルダ).mtime is Date              … 3 通りとも OK
getTime(存在しない) は例外                              … 3 通りとも OK
currentPath はカレントのストレージ名 (末尾 /)、代入は拒否 … 本体版 OK
fstat.dll 読み込み後: fstat / getTime に atime・ctime、
  dirlistEx の size / attrib / mtime、FILE_ATTRIBUTE_DIRECTORY … OK
```
