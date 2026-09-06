# `user://` ストレージメディア (SDL ビルド)

セーブデータ等の書込先として登録される `user://` ストレージメディアの初期化と、
プラットフォーム側で実装を差し替えるためのフックについて。

実装: `sdl3/base/storage.cpp` (`InitStorageSystem` / `DoneStorageSystem`) と
`sdl3/environ/app.cpp` (`SDL3Application::InitDataPath`)。

## 既定 (SDL user storage)

`InitDataPath()` が起動時に一度だけ `InitStorageSystem(orgname, appname, ...)` を
呼び、 `SDL_OpenUserStorage(orgname, appname)` の結果を `user://` として
`TVPRegisterStorageMedia` する (ready になるまで待つ)。 失敗すると例外
(`TVPFailedToOpenUserStorage`)。

- `orgname` / `appname` の既定は `wamsoft` / `krkrz`。 コマンドラインの
  `-orgname=` / `-appname=` で差し替えられる。
- Windows / Linux 以外では `$(personalpath)` / `$(appdatapath)` / `$(vistapath)` /
  `$(savedgamespath)` は `user://./` へ展開される (データパスの既定解決)。

## プラットフォーム差し替えフック

```cpp
// sdl3/environ/app.h
virtual class iTVPStorageMedia2 * CreateUserStorageMedia(
    const char *orgname, const char *appname) { return nullptr; }
```

`SDL3Application` の派生クラスでこれを override し **非 null を返すと、 SDL user
storage の代わりにそれが `user://` として登録される**。 既定は `nullptr` で従来
どおり。 セーブデータのマウント寿命やトランザクション (書込のコミット / 破棄) を
プラットフォーム側の作法で管理する必要があるコンシューマ機向けの口。

- 返したメディアの所有権はエンジンへ渡る。 終了時 `DoneStorageSystem()` が
  `TVPUnregisterStorageMedia` + `Release()` するので、 参照カウントをその前提で
  実装すること。
- 非 null を返した場合 `SDL_OpenUserStorage` は**呼ばれない**。
- 呼ばれるのは `InitDataPath()` の中 = データパス解決の直前。 この時点で
  コマンドライン (`-orgname` / `-appname`) は解決済み。
