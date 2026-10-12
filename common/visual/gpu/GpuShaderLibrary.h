//---------------------------------------------------------------------------
// 事前変換済みシェーダー (gpushaderc.py が作るパック .kzgs) の管理
//
// ShaderProgram(vs, fs) はソースのハッシュでパックを引く。見つからなければ
// ソースを収集ディレクトリへ書き出して例外にする (開発時は書き出されたものを
// gpushaderc.py pack にかけてパックへ足す)。
//---------------------------------------------------------------------------
#ifndef GpuShaderLibraryH
#define GpuShaderLibraryH

#include "GpuDevice.h"
#include <map>
#include <string>

namespace TVPGpu {

//! (vs + "\0" + fs) の FNV-1a 64bit。テキストは BOM を除き CRLF を LF にそろえてから (UTF-8)
uint64_t ShaderProgramKey( const std::string& vsUtf8, const std::string& fsUtf8 );
//! BOM 除去と CRLF→LF
std::string NormalizeShaderText( const std::string& src );

class ShaderLibrary {
public:
	static ShaderLibrary& Instance();

	//! パック (メモリ上) を登録する。同じキー・同じ形式は後から登録したものが勝つ
	bool AddPack( const uint8_t* data, size_t size, const char* label );
	//! デバイスが受け付ける形式のものを探す。見つからなければ false
	bool Find( uint64_t key, ProgramBinary& out ) const;
	//! 起動オプション・既定の場所からユーザーのパックを読む (初回のみ)
	void LoadUserPacks();
	//! 組み込みシェーダー (shaders/builtin.json) のソース。program は "default" など
	static const char* BuiltinSource( const char* program, ShaderStage stage );
	//! 見つからなかったソースを書き出す (収集ディレクトリが設定されていれば)。書いた場所を返す
	std::string CollectMissing( uint64_t key, const std::string& vs, const std::string& fs );

private:
	struct Entry {
		ShaderFormat format;
		const uint8_t* vs; uint32_t vsSize;
		const uint8_t* fs; uint32_t fsSize;
		const char* refl; uint32_t reflSize;
	};
	std::map<uint64_t, std::vector<Entry>> Entries;   // キーごとに形式違いを持てる
	std::vector<std::vector<uint8_t>> OwnedPacks;   // ファイルから読んだパックの実体
	bool UserPacksLoaded = false;
};

} // namespace TVPGpu

#endif
