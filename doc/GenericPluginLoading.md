# generic (SDL) 版のプラグイン解決

status: **直した (2026-09-26)**
対象: `generic/base/StorageImpl.cpp` の `tTVPPluginHolder`

## 症状

SDL 版で `Plugins.link("KAGParserEx.dll")` が「プラグインを読み込めません」で失敗する。

- **自動検索パス (`Storages.addAutoPath`) に置いたプラグインが見つからない**
- **`file://` のフルパスを渡すと必ず失敗する**

実行ファイルの真隣に置いたときだけ成功していた。

## 原因

WINVER 版の `tTVPPluginHolder` は **まずストレージシステムに聞く**:

```cpp
ttstr place(TVPGetPlacedPath(aname));       // autopath / フルパスを解決
if(!place.IsEmpty()) LocalTempStorageHolder = new tTVPLocalTempStorageHolder(place);
else { /* exe の隣 / system/ / plugin/ を直接探す */ }
```

generic 版はこの**最初の一段が無く**、`AppPath() + name` と `PluginPath() + name` しか
見ていなかった。フルパスを渡すと `AppPath() + "file://..."` という文字列になって当然落ちる。

## 直し方

generic 側も WINVER と同じ順にした (storage → exe の隣 → プラグインフォルダ)。
アーカイブ内にあるプラグインもテンポラリへ取り出して使えるようになる。

## 関連: 「generic = CS 機」という思い込み

ゲーム側のスクリプトで `kirikiriz_generic` を「CS 機だから DLL も xp3 も使えない」の意味で
使っていた。これは**本体が generic かどうか**でしかなく、デスクトップの SDL 版も generic になる。
判定は `System.platformTag` (windows/linux/macos/switch/ps5/android…) で行うこと。

## ⚠⚠ generic では Win32 専用 API が「空関数」としてプラグインへ渡る (2026-09-27 判明)

`common/base/FuncStubs.cpp` (makestub.pl 生成) は Win32 専用の本体 API を
**`#ifdef __WINVER__` で囲み、generic ビルドでは引数も戻り値も無い空関数に差し替えて**
エクスポートしている。

```cpp
#ifdef __WINVER__
static IStream * STDCALL TVP_Stub_9974...(const ttstr & name, tjs_uint32 flags)
{ return TVPCreateIStream(name, flags); }
#else
static void STDCALL TVP_Stub_9974...(){}      // ← 型が違う空関数
#endif
```

**プラグイン側はこれを本物だと思って呼ぶので、戻り値としてゴミを受け取る。**
そのゴミをポインタとして辿った瞬間に落ちる。現在 19 個の API がこの形:
`TVPGetApplicationWindowHandle` / `TVPCreateIStream` ほか。

実例: `PackinOne.dll` (旧プラグインの詰め合わせ) が SDL ビルドで
`Plugins.link("csvParser.dll")` の直後にアクセス違反で即死していた。
WINVER では同じ DLL が問題なく動く。

**直すなら**: 生成側 (makestub.pl) で、generic でも**同じ戻り値型で 0 / nullptr を返す**
スタブにする。少なくとも「ゴミが返る」状態は無くなり、プラグインが NULL 判定できる。

⚠ **その上で、Win32 専用 API に依存するプラグイン自体は generic では機能しない**。
PackinOne の場合、依存しているのはファイル選択ダイアログ (`FileSelector` / `selfile`)、
DPI アイコン (`DpiIconManager`)、PE 判定 (`pemachinetype`) — いずれも Win32 の UI / COM。
generic 版ではこれらを外してビルドする必要がある。

### PackinOne を現行ツリーでビルドする (途中まで)

PackinOne 用の `CMakeLists.txt` を用意した。
premake5.lua のファイル一覧と include をそのまま写したもの。

- ✅ `add_subdirectory` で拾われるところまで通る
  (本体側の探索を「CMakeLists.txt を持つフォルダ」に直した。同名の空フォルダを掴んでいたため)
- ✅ `systemEx/main.cpp` に `#include <winternl.h>` を足して `NTSTATUS` 未定義を解消 (未コミット)
- ⚠ **残り 177 エラー**。中身は上記の Win32 専用 API と、tp_stub の名前空間化
  (`krkrz_plugin::tjs_char` になった) による型不一致。**generic 向けには Win32 部分の
  切り離しが要る**ので、TVP_PLUGINS にはまだ入れていない。
