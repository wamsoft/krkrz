//---------------------------------------------------------------------------
// 実行ファイルに結合された XP3 アーカイブの探索
//
// リリース時に exe の後ろへ xp3 を付け足すと、本体は «自分自身» をアーカイブとして
// 開ける (データの探索順は data.xp3 → (data.exe) → 自分自身 → data フォルダ)。
// 付け足したデータは実行ファイルのイメージ (PE はセクション、ELF はセグメントと
// セクション) の後ろにあるので、ヘッダからイメージの終わりを求めて、そこから先だけを
// 16 バイト境界で探す。以前は先頭から全体を調べていたため、結合していない exe でも
// 起動のたびに exe 全体を読んでいた。
//
// ストレージ層に依存しない (SDL 版はストレージの準備前にプロジェクトの場所を決めるため)。
// 読み込みは呼び出し側が渡す関数で行う。
//---------------------------------------------------------------------------
#ifndef ExecutableOverlayH
#define ExecutableOverlayH

#include <cstddef>
#include <cstdint>
#include <functional>

/// pos から最大 len バイト読み、読めたバイト数を返す
using tTVPReadAtFunc = std::function<size_t(uint64_t pos, void *buf, size_t len)>;

enum class tTVPExecutableKind { None, PE, ELF };

/// 先頭の数バイトから実行ファイルの種類を判定する (PE = "MZ"、ELF = "\x7fELF")
tTVPExecutableKind TVPGetExecutableKind(const uint8_t *head, size_t len);

/// イメージの終わり (= 後ろに付け足されたデータの始まり) のファイル上の位置。
/// ヘッダが読めない・壊れているときは 0 (呼び出し側は先頭から探す)
uint64_t TVPGetExecutableImageEnd(const tTVPReadAtFunc &read, uint64_t fileSize);

/// from 以降 (16 バイト境界) で XP3 のヘッダを探す。見つかれば offset に位置を入れて true
bool TVPFindXP3MarkFrom(const tTVPReadAtFunc &read, uint64_t from, uint64_t fileSize, uint64_t &offset);

/// 実行ファイルに結合された XP3 を探す (種類の判定 → イメージの終わりから探索)。
/// 実行ファイルでなければ false
bool TVPFindXP3InExecutable(const tTVPReadAtFunc &read, uint64_t fileSize, uint64_t &offset);

#endif
