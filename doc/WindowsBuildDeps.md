# ⚠ 日本語の Visual Studio で ninja を叩くとヘッダ依存が記録されない

status: **原因特定・対処済み (2026-09-26)**
対象: Windows + Ninja (CMake Presets の `*-windows*` 全部)

## 症状

**ヘッダだけ直してビルドしても、そのヘッダを使っている .cpp が再コンパイルされない。**
オブジェクトが古いまま残り、翻訳単位ごとにクラスの定義が食い違う (ODR 違反) と、
**仮想関数呼び出しが別の関数へ飛ぶ**。

実際に踏んだ例 (SDL ビルドが起動しない):

```
InitAudioSystem → InitMiniAudio → TVPGetCommandLine
  → TVPInitProgramArgumentsAndDataPath → TVPGetEmbeddedOptions
  → std::u16string::append → _Xlen_string → 未処理の C++ 例外 → 0xC0000409
```

`TVPGetEmbeddedOptions` の中で

```cpp
const std::vector<tjs_string> &tags = Application->GetPlatformTags();
```

が **`Application` 自身のアドレス** (= vtable ポインタの位置) を返していた。
`tags.size()` は `576460752303405785` になり、その中身を連結しようとして
「文字列が長すぎる」で死んでいた。

`GetPlatformTags` を呼んだ翻訳単位と、`Application` の実体を作った翻訳単位とで
**クラスの仮想関数table の並びが違った**ために、別のスロットを呼んでいた。

## 原因

`CMakeFiles/rules.ninja` の

```
msvc_deps_prefix = Note: including file:
```

は**英語**だが、日本語の VS から `cl` を動かすと

```
注: インクルード ファイル:  ...
```

と**日本語で出る**ので、ninja が `/showIncludes` の出力を解釈できず
**ヘッダ依存を 1 つも記録しない**。

CMake は接頭辞を検出するとき**自分で `VSLANG=1033` を立てて**測るので、
検出結果は常に英語になる。ビルド時のシェルが日本語のままだと食い違う。

### 見分け方

```
ninja -f build-Release.ninja -t deps <obj への相対パス>
  → "#deps 0" なら記録されていない (正常なら数十〜数百)
```

ビルドログに `注: インクルード ファイル:` の行が**大量に出ていたら**それも印。
正常なら ninja が食べるので出てこない。

## 対処

**ビルドする環境で `VSLANG=1033` を立てる。**

- `CMakePresets.json` の `windows` プリセットに
  `"environment": { "VSLANG": "1033" }` を入れた (configure と、それを継ぐビルドに効く)
- シェルから直接 `ninja` を叩くときは `set VSLANG=1033` を忘れないこと

```bat
call vcvars64.bat
set VSLANG=1033
cd build\x64-windows-win
ninja -f build-Release.ninja krkrz64
```

## ⚠ 接頭辞は build ディレクトリごとに焼かれている

検出結果は `build/<preset>/CMakeFiles/<CMake版>-msvc<n>/CMakeCXXCompiler.cmake` の
`CMAKE_CXX_CL_SHOWINCLUDES_PREFIX` に**焼き込まれていて、再 configure しても測り直さない**
(`cmake -U CMAKE_CL_SHOWINCLUDES_PREFIX .` でも消えない)。

実際、手元では 2 つの build ディレクトリで値が食い違っていた:

| build ディレクトリ | 焼かれていた接頭辞 |
|---|---|
| `x64-windows` (SDL) | `Note: including file: ` (英語) |
| `x64-windows-win` (WINVER) | `メモ: インクルード ファイル: ` (日本語) |

**英語に揃えるには、コンパイラ検出のキャッシュごと消してから configure する**:

```
rm -rf build/<preset>/CMakeFiles/*-msvc*
set VSLANG=1033
cmake .
findstr /C:"msvc_deps_prefix" CMakeFilesules.ninja   ← 英語になったか確認
```

## ⚠ 一度壊れた build ディレクトリは掃除が要る

依存が記録されていなかった間に積み上がった古いオブジェクトは、
`VSLANG` を直しただけでは**そのまま**残る (ninja はタイムスタンプ上「最新」と見る)。

```
rm -rf build/<preset>/core/CMakeFiles/krkrz64.dir
```

を消してから全部ビルドし直す。2026-09-26 に `x64-windows` と `x64-windows-win` の
両方で実施した。

## 直ったことの確かめ方

```
ninja -t deps <obj>              → #deps が数十〜数百になる
touch src/core/common/visual/WindowIntf.h
ninja -f build-Release.ninja krkrz64   → 依存する 32 本が再コンパイルされる (実測)
```
