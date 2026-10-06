# Linux ビルドの検証環境 (基準環境)

Linux 版のビルド確認基準は、[steamdev](https://github.com/wamsoft/steamdev)
リポジトリの `deckbuild/` で構築する Docker ビルド環境とする。
umbrella (`krkrz_dev`) / `src/core` とも「この環境で configure〜install が通り、
生成バイナリが下記の互換性基準を満たす」ことをもって Linux ビルド確認済みとする。

## 基準環境の中身

- **ベースイメージ**: Valve 公式 **Steam Linux Runtime 3.0 "sniper" SDK**
  (`registry.gitlab.steamos.cloud/steamrt/sniper/sdk`)
  = **Debian 11 / glibc 2.31**
- **コンパイラ**: SDK 同梱の **gcc-14** (Valve バックポート版) を既定で使用。
  SDK 標準の gcc-10 は新しめの intrinsic (`_mm256_cvtsi256_si32` 等) を持たず
  コンパイルが通らないことがある。gcc-14 でも glibc ターゲットは 2.31 のまま
- **追加ツール**: CMake 3.31 (SDK 同梱の Debian 11 cmake はプリセット v2 に
  足りないため差し替え) / vcpkg (full clone — manifest の `builtin-baseline`
  解決に履歴が必要) / nasm・yasm (vcpkg の libvpx 等が要求)
- **実行形態**: WSL2 上の docker (Linux ホストの docker でも可)

## なぜ sniper SDK か

Linux バイナリの互換性はほぼ **ビルド環境の glibc バージョン**で決まる
(新しい glibc でビルドしたバイナリは古い glibc では動かない)。手元の WSL Ubuntu
等 (glibc 2.39+) で直ビルドすると、SteamOS を含む配布先で動かないリスクが高い。

sniper SDK (glibc 2.31) でビルドしたバイナリは:

- SteamOS ネイティブ (glibc はより新しい) でそのまま動く
- Steam Linux Runtime コンテナ内 (`compat_tool=SteamLinuxRuntime_sniper`)
  = Steam 配布タイトルと同一の実行環境でも動く
- glibc 2.31 以降の一般的なディストリビューションでも動く

つまり「Steam が公式にサポートする Linux ターゲット」に合わせる方針。
他ディストリビューションでの直ビルドが通ること自体は歓迎だが、確認基準では
ない (基準はあくまでこの環境)。

## 使い方

セットアップとビルド (詳細は steamdev リポジトリの `deckbuild/README.md`):

```bash
# WSL 内、初回のみ: ビルドイメージ作成 (数 GB ダウンロード)
<steamdev>/deckbuild/deckbuild.sh image

# umbrella をビルド (configure + build + install)
<steamdev>/deckbuild/deckbuild.sh -s /mnt/d/work/kirikiri/krkrz_dev all

# プリセット/構成/追加引数 (既定: PRESET=x64-linux, BUILD_TYPE=Release)
PRESET=x64-linux BUILD_TYPE=Debug CMAKEOPT='-DKRKRZ_USE_SJIS=YES' \
    <steamdev>/deckbuild/deckbuild.sh -s ... all

# 個別ステップ / コンテナ内調査 / ビルドツリー破棄
<steamdev>/deckbuild/deckbuild.sh -s ... configure|build|install|shell|clean
```

Windows からは `deckbuild.ps1` (WSL 経由で同スクリプトを呼ぶ) が使える。

### Linux ホストで直接使う場合の注意

上記は WSL 前提の例だが、Linux ホストの docker でもそのまま使える
(パスは `-s ~/kirikiri/krkrz_dev` のようにホストのパスを渡す)。

- コンテナは root で動くので、install 先 `bin/x64-linux/` が **root 所有**になる。
  ホスト側で触る前に所有者を戻す:
  `docker run --rm -v $PWD/bin:/b deckbuild-sniper chown -R $(id -u):$(id -g) /b`
- ホストでそのまま起動できる (同梱 SDL3 は RPATH で解決、下記「既知の注意点」)。
  Wayland セッションでは `WAYLAND_DISPLAY` を渡す
- 画面 (コンポジタ) がスリープ中に Wayland で起動すると、SDL3 の初期化
  (`Wayland_ShowCursor`) で segfault する。ビルドの問題ではないので、画面を
  復帰させてから実行する

- ビルドツリー (`build/x64-linux`) は docker named volume 上
  (bind mount の遅い I/O 回避)。ホストからは見えない
- 成果物は install でソース側 `bin/x64-linux/Release/` に出るので
  Windows 側からそのまま見える
- vcpkg のバイナリキャッシュは volume に永続化され 2 回目以降は速い
- configure は FetchContent (harfbuzz / sqlite3 / SDL3 等) で
  ネットワークアクセスが必要。コンテナはネットワークありで実行される
- コンパイラ差し替えは `DECKBUILD_CC` / `DECKBUILD_CXX` (例: clang)。
  変えたら `clean` でビルドツリーを作り直すこと

umbrella ルートには `deckproject.toml` があり、steamdev CLI から
ビルド→資材構築→Steam Deck への転送・起動まで一括で回せる
(`steamdev -d <deck> project -p . ship linux`)。

**リリース前の実機確認手順** (転送・起動・観測・後片付けとハマりどころ) は
umbrella の `doc/topics/core/steamdeck.md` にチェックリスト化してある。

## バイナリの合格基準

ビルドが通るだけでなく、生成バイナリが以下を満たすこと
(コンテナ内 = `deckbuild.sh shell` で確認):

```bash
# 要求 glibc シンボルの最大バージョン — 2.31 以下なら OK
objdump -T <exe> | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1

# GLIBCXX 依存が出ないこと (libstdc++ は静的リンク)
objdump -T <exe> | grep -o "GLIBCXX_[0-9.]*" | sort -Vu | tail -1

# 動的リンクする .so の一覧 — SDK にしか無い .so が NEEDED に出たら
# 同梱 (exe の RPATH = $ORIGIN で解決) するか、静的リンクに倒す
readelf -d <exe> | grep -E "NEEDED|RPATH"
```

実測 (2026-09-06 時点の krkrz x64-linux): GLIBC ≤ 2.30、GLIBCXX 依存なし。
再確認 (2026-10-06, Linux ホスト docker): krkrz 本体 GLIBC_2.30 / プラグイン・libSDL3 とも ≤ 2.31、GLIBCXX 依存なし。
ホスト (Wayland) で `-demotest -demotestcap` 全 24 シーン ok。
Steam Deck 実機のネイティブ実行 / sniper コンテナ実行の両方で動作確認済み。

### 既知の注意点

- 同梱 SDL3 は install 先に実体 `libSDL3.so.0.x.y` と soname リンク
  `libSDL3.so.0` を置き、exe には `$ORIGIN` を **DT_RPATH** で埋め込む
  (`--disable-new-dtags`)。Steam から起動すると `LD_LIBRARY_PATH` の先頭に
  Steam ランタイムと `/usr/lib` が入り、ゲームフォルダは末尾に付くだけなので、
  `LD_LIBRARY_PATH` の後に探される RUNPATH では SteamOS 側の libSDL3
  (2026-10 時点 3.2.18) が勝ってしまう。`LD_LIBRARY_PATH=.` も不要
  (`readelf -d krkrz` で `(RPATH) Library rpath: [$ORIGIN]` を確認できる)
- `krkrz_plugin()` (tp_stub/krkrz.cmake) で作る共有プラグインにも
  `$ORIGIN:$ORIGIN/..` を DT_RPATH で付けている。プラグインが同梱 .so に依存する
  場合は `plugin/` か exe のフォルダに置けば読まれる。プラグインに RUNPATH を
  付けると exe 側の DT_RPATH も効かなくなるので、独自に RPATH を設定するときも
  `--disable-new-dtags` を使うこと (`krkrz_plugin()` を使わないプラグインは対象外)
- sniper SDK イメージはローリング更新される。再現性を厳密にしたい場合は
  Dockerfile の `FROM` をダイジェスト固定にする

## 配布パッケージとデータ保存場所

- 作品の配布物 (そのまま動くフォルダ / tar.gz) は外枠 **krkrz_linux**
  (https://github.com/wamsoft/krkrz_linux) で作る。`linux-config.json` に案件を書き、
  この sniper 環境 (steamdev の `deckbuild-sniper` イメージ) でビルドして、
  install 結果・`<exe名>.cf`・`.desktop`/アイコン・資材をまとめる。
  使い方は umbrella の `doc/topics/core/linux_package.md`
- **データ保存場所の既定 (2026-10〜)**: `-datapath` 未指定時は
  `~/.local/share/<orgname>/<appname>/` (`SDL_GetPrefPath` のローカルパス)。
  以前は exe の隣の `savedata` で、末尾 `/` が欠けて `savedataX` と書かれていた。
  配布物は `<exe名>.cf` に `orgname` / `appname` を書く (krkrz_linux が生成)。
  実装は `sdl3/environ/app.cpp` の `GetDataPathDirectory` / `InitDataPath`

## 関連

- steamdev リポジトリ: https://github.com/wamsoft/steamdev
  (`deckbuild/README.md` = ビルド環境詳細・依存ライブラリ不足時の対処ガイド、
  `docs/WORKFLOW.md` = Steam Deck 実機でのデプロイ/デバッグ運用)
- umbrella `deckproject.toml` — krkrz_dev の build/stage/deploy 定義 (実運用例)
- krkrz_linux: https://github.com/wamsoft/krkrz_linux — Linux 版の配布パッケージを作る外枠
