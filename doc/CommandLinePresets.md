# コマンドライン プリセット / 引数再構成 (要望メモ)

status: **未着手 (要望・設計メモ)** — 2026-08 記録
関連: 起動引数の解決は `generic/base/SysInitImpl.cpp` の
`TVPInitProgramArgumentsAndDataPath()` / `PushAllCommandlineArguments()` /
`PushConfigFileOptions()`。

## 背景・要望

アプリ (ゲーム / ツール) ごとに、起動時のオプション組み合わせが増えて複雑化して
いる。例: threepp のブラウザ編集 UI は
`-readencoding=UTF-8 -sample -replweb=8899 -webui` を毎回並べる必要がある。
用途別 (通常起動 / 編集モード / エージェント検証 / リモート編集…) にオプション束が
違い、手打ち・ショートカット・バッチが散らばって管理しづらい。

**やりたいこと**: 「オプション合わせの本処理に入る前」に、
アプリ側の意図でコマンドライン引数を**再構成 (正規化・束の展開・別名解決)** できる
仕組み。1 個の分かりやすいスイッチ (例 `-profile=edit`) を、複数の実オプション束へ
展開できると良い。

## 既存機構 (これを土台にできる)

`TVPInitProgramArgumentsAndDataPath()` が起動時に一度だけ、以下を優先順に
`TVPProgramArguments` (平坦な `-name=value` リスト) へ積む。`TVPGetCommandLine()`
は先頭優先で線形検索する:

1. 実コマンドライン (`PushAllCommandlineArguments`) — **最優先**
2. per-user `.cfu` (`<datapath>/<exe名>.cfu`) — デスクトップのみ
3. `<exe名>.cf` (exe と同階層) — デスクトップのみ
4. `config.cf` (exe と同階層) — 全環境
5. embedded options (`resource://config_<tag>.cf` → `config.cf`) — 最下位

つまり「exe 同階層の `config.cf` に共通オプションを列挙」は既にできる。
デスクトップ (Win/Mac/Linux) では WINVER と同じ `<exe名>.cf` / `<exe名>.cfu` も
読む (2026-08 に generic 側を WINVER と揃えた。それ以前は `config.cf` のみで、
`.cfu` は `if (false)` で無効化されていた)。非デスクトップは従来どおり
`config.cf` のみ。

## 実現案

### 案A: プロファイル展開フック (推奨)
`PushAllCommandlineArguments()` の直後・各種オプション参照の前に、
`ExpandArgumentProfiles()` を一段挟む:

- `-profile=<name>` (複数可) を検出 → 対応するオプション束を `TVPProgramArguments`
  へ展開して積む (実引数より後 = 低優先。実引数で個別上書き可能)。
- プロファイル定義の置き場所は優先度順で複数対応:
  - `config.cf` 内の `[profile:<name>]` セクション、または
  - `profiles.cf` / embedded、または
  - コード内の組み込みプリセット表 (アプリが登録)
- 別名・非推奨オプションの正規化 (旧名 → 新名) も同じフックで吸収できる。

### 案B: アプリ側フック関数
エントリポイント (StartApplication) が本処理前に呼ぶ
`TVPRewriteProgramArguments(std::vector<ttstr>&)` を用意し、アプリ (プラグイン
含む) が引数列を自由に書き換えられるようにする。柔軟だが規約が緩くなる。

### 案C: `.cfu` のプロファイル化
per-user `.cfu` の読み込みはデスクトップで有効化済み (`<datapath>/<exe名>.cfu`)。
そこにプロファイル選択を書けるようにする。datapath がローカルパスとは限らない
環境向けに、datapath 非依存な既知パス (personal path 等) を候補に足す余地もある。

## 検討ポイント

- **優先順位**: 展開したプロファイル束は「実コマンドラインより低い」= 個別スイッチで
  上書きできる、が直感的。プロファイル同士の順序も定義が要る。
- **タイミング**: datapath 確定前に `-datapath` 等も再構成対象になりうる
  (現状 2 パス: datapath 取得用 → 本取得)。プロファイル展開をどちらのパスで
  効かせるか要整理。
- **生成カウンタ**: `TVPCommandLineArgumentGeneration` を再構成後にインクリメント
  すれば、後段の再読込に整合する。
- **可視化**: 既に `TVPDumpOptions()` が `-@options` 相当で最終引数列をログに出す。
  プロファイル展開結果もここに出れば検証しやすい。
- **セキュリティ**: `acceptfilenameargument` 等の既存ガードとの相互作用。
  プロファイルからの危険オプション注入を許すかどうか。

## 当面の回避策 (実装まで)

- exe 同階層の `config.cf` に共通オプションを書く (既存機能)。
- 用途別に別 exe 名のコピー + それぞれの `config.cf`、または起動バッチ/
  ショートカットを用意する。
- threepp 編集 UI の例: `config.cf` に `sample` `replweb=8899` `webui`
  `readencoding=UTF-8` を書いておけば、引数無しの二重起動で編集モードになる。
