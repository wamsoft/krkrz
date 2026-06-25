#include "tjsCommHead.h"
#include "CharacterSet.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#include "UtilStreams.h"
#include "SysInitIntf.h"

#include <string>
#include <map>
#include <set>
#include <vector>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>

#define MEDIA_NAME TJS_W("resource")

/**
 * Android 用のリソース参照用
 * assets フォルダ内のファイルが対象
 * パス別に初回アクセス時にファイル一覧をキャッシュ
 */

AAssetManager* assetManager = nullptr;

// 起動側 (Android Java 層) から渡されるアセットキャッシュフォルダ。
// BootstrapActivity が assets 内の *.xp3 等を `<filesDir>/assets/` に
// コピーした後の絶対パス (末尾 '/' 付き)。stdapp.cpp::InitPath() が
// xp3 / startup.tjs の検索基点として参照する。
// 未設定 (空文字) のときは legacy AssetManager 経路 (= appPath="/") に
// フォールバック。
std::string g_AndroidAssetCacheDir;

class AndroidResourceStream : public iTJSBinaryStream {
private:
    std::vector<tjs_uint8> Data;
    tjs_uint Position;

public:
    AndroidResourceStream(AAsset* asset) : Position(0) {
        if (asset) {
            size_t length = AAsset_getLength(asset);
            Data.resize(length);
            AAsset_read(asset, Data.data(), length);
            AAsset_close(asset);
        }
    }

    AndroidResourceStream(const std::vector<tjs_uint8>& data) : Data(data), Position(0) {
    }

    virtual ~AndroidResourceStream() {
    }

    virtual tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
        tjs_int64 pos = 0;
        switch (whence) {
        default:
        case TJS_BS_SEEK_SET: pos = offset;            break;
        case TJS_BS_SEEK_CUR: pos = offset + Position; break;
        case TJS_BS_SEEK_END: pos = offset + Data.size(); break;
        }
        if (pos < 0) pos = 0;
        else if (pos > (tjs_int64)Data.size()) pos = Data.size();
        Position = (tjs_uint)pos;
        return pos;
    }

    virtual tjs_uint TJS_INTF_METHOD Read(void *buffer, tjs_uint read_size) {
        if (Position + read_size > Data.size()) read_size = Data.size() - Position;
        if (read_size > 0) {
            memcpy(buffer, Data.data() + Position, read_size);
            Position += read_size;
        }
        return read_size;
    }

    virtual tjs_uint TJS_INTF_METHOD Write(const void *buffer, tjs_uint write_size) {
        TJS_eTJSError(TJSWriteError); return 0; 
    } // raise exception

    virtual void TJS_INTF_METHOD SetEndOfStorage() override {
        TJS_eTJSError(TJSWriteError);
    } // raise exception

	virtual tjs_uint64 TJS_INTF_METHOD GetSize() {
        return Data.size();
    }
};

class tTVPResourceStorageMedia : public iTVPStorageMedia
{
	typedef tTVPResourceStorageMedia Self;
	static Self *Instance;

    tjs_int RefCount;
    // パス別のファイルキャッシュ: dirPath -> ファイル名のセット
    std::map<std::string, std::set<std::string>> dirCache;
    
    // "./" で始まる場合は除去する共通処理
    static std::string RemoveLeadingDotSlash(const std::string& path) {
        if (path.size() >= 2 && path[0] == '.' && path[1] == '/') {
            return path.substr(2);
        }
        return path;
    }
    
    // ttstr から UTF8 パスを取得し、先頭の "./" を除去
    static std::string GetNormalizedPath(const ttstr& name) {
        std::string path;
        TVPUtf16ToUtf8(path, name.c_str());
        return RemoveLeadingDotSlash(path);
    }
    
    // パスからディレクトリ部分とファイル名部分を分離
    static void SplitPath(const std::string& fullPath, std::string& dirPath, std::string& fileName) {
        size_t lastSlash = fullPath.rfind('/');
        if (lastSlash != std::string::npos) {
            dirPath = fullPath.substr(0, lastSlash);
            fileName = fullPath.substr(lastSlash + 1);
        } else {
            dirPath = "";
            fileName = fullPath;
        }
    }
    
    // 指定ディレクトリのファイル一覧をキャッシュする（初回アクセス時のみ）
    void EnsureDirCached(const std::string& dirPath) {
        // 既にキャッシュ済みならスキップ
        if (dirCache.find(dirPath) != dirCache.end()) {
            return;
        }
        
        if (!assetManager) {
            TVPAddLog(TJS_W("AssetManager is not available"));
            dirCache[dirPath] = std::set<std::string>(); // 空キャッシュを登録
            return;
        }
        
        std::set<std::string> fileSet;
        AAssetDir* assetDir = AAssetManager_openDir(assetManager, dirPath.c_str());
        if (assetDir) {
            const char* filename;
            while ((filename = AAssetDir_getNextFileName(assetDir)) != nullptr) {
                fileSet.insert(filename);
            }
            AAssetDir_close(assetDir);
        }
        
        dirCache[dirPath] = std::move(fileSet);
        TVPAddLog(ttstr(TJS_W("Cached directory: ")) + ttstr(dirPath.c_str()) + 
                  ttstr(TJS_W(" (")) + ttstr((tjs_int)dirCache[dirPath].size()) + ttstr(TJS_W(" files)")));
    }
    
    // キャッシュからファイルの存在を確認
    bool ExistsInCache(const std::string& fullPath) {
        std::string dirPath, fileName;
        SplitPath(fullPath, dirPath, fileName);
        
        // ディレクトリをキャッシュ
        EnsureDirCached(dirPath);
        
        auto dirIt = dirCache.find(dirPath);
        if (dirIt != dirCache.end()) {
            return dirIt->second.find(fileName) != dirIt->second.end();
        }
        
        return false;
    }
    
    AAsset* FindAsset(const ttstr &name) {
        if (!assetManager) return nullptr;
        
        std::string resourceName = GetNormalizedPath(name);
        
        // キャッシュで存在確認
        if (ExistsInCache(resourceName)) {
            return AAssetManager_open(assetManager, resourceName.c_str(), AASSET_MODE_BUFFER);
        }
        
        return nullptr;
    }
        
    static void SetAssetManager(AAssetManager* manager) {
        assetManager = manager;
        if (Instance) {
            Instance->dirCache.clear(); // キャッシュをクリア
        }
    }

public:
	tTVPResourceStorageMedia() : RefCount(1) {
    }

    ~tTVPResourceStorageMedia() {
    }

	void TJS_INTF_METHOD  AddRef (void) override { ++RefCount; }
	void TJS_INTF_METHOD  Release(void) override {
		if (RefCount == 1) delete this;
		else --RefCount;
	}

	//--------------------------------------------------------------

	virtual void TJS_INTF_METHOD GetName(ttstr &name) override { name = MEDIA_NAME; }
	virtual void TJS_INTF_METHOD NormalizeDomainName(ttstr &name) override {}
	virtual void TJS_INTF_METHOD NormalizePathName  (ttstr &name) override {}

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {
        if (!assetManager) return false;
        
        std::string resourceName = GetNormalizedPath(name);
        
        // キャッシュで存在確認
        return ExistsInCache(resourceName);
	}

    virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) override {
		if ((flags & TJS_BS_ACCESS_MASK) != TJS_BS_READ) return nullptr;
		
        AAsset* asset = FindAsset(name);
        if (!asset) return nullptr;
        
        iTJSBinaryStream *stream = new AndroidResourceStream(asset);
        return stream;
	}

    virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) override {
        std::string dirPath = GetNormalizedPath(name);
        
        // 末尾の "/" を除去
        while (!dirPath.empty() && dirPath.back() == '/') {
            dirPath.pop_back();
        }
        
        // ディレクトリをキャッシュ
        EnsureDirCached(dirPath);
        
        // キャッシュからファイル一覧を取得
        auto dirIt = dirCache.find(dirPath);
        if (dirIt != dirCache.end()) {
            for (const auto& fileName : dirIt->second) {
                tjs_string filename;
                TVPUtf8ToUtf16(filename, fileName.c_str());
                lister->Add(ttstr(filename.c_str()));
            }
        }
    }

    virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { name.Clear(); } // no local

	//--------------------------------------------------------------

	static void Load() {
		if(!Instance) {
			Instance = new Self();
			TVPRegisterStorageMedia(Instance);
		}
	}
	static void Unload() {
		if (Instance) {
			TVPUnregisterStorageMedia(Instance);
			Instance->Release();
			Instance = nullptr;
		}
	}
	
	// Android用のパブリックAPI - JNI側から呼び出される
	static void SetAssetManagerInstance(AAssetManager* manager) {
        SetAssetManager(manager);
    }
};

tTVPResourceStorageMedia * tTVPResourceStorageMedia::Instance = nullptr;

static tTVPAtStart AtStart(TVP_ATSTART_PRI_PREPARE, tTVPResourceStorageMedia::Load);
static tTVPAtExit  AtExit(TVP_ATEXIT_PRI_PREPARE, tTVPResourceStorageMedia::Unload);

// JNI用のC関数エクスポート
extern "C" {
    JNIEXPORT void JNICALL
    Java_jp_wamsoft_krkrz_KrkrzActivity_setAssetManager(JNIEnv *env, jobject thiz, jobject asset_manager) {
        AAssetManager* mgr = AAssetManager_fromJava(env, asset_manager);
        tTVPResourceStorageMedia::SetAssetManagerInstance(mgr);
    }

    // BootstrapActivity が assets を内部ストレージにコピーした後に呼ぶ。
    // 末尾は '/' 付きで渡される想定。stdapp.cpp::InitPath() がこの値を
    // xp3 / startup.tjs の検索基点として使う。
    JNIEXPORT void JNICALL
    Java_jp_wamsoft_krkrz_KrkrzActivity_setAssetCacheDir(JNIEnv *env, jobject thiz, jstring path) {
        if (!path) {
            g_AndroidAssetCacheDir.clear();
            return;
        }
        const char *utf = env->GetStringUTFChars(path, nullptr);
        if (utf) {
            g_AndroidAssetCacheDir = utf;
            env->ReleaseStringUTFChars(path, utf);
            // 末尾スラッシュ補正 (Java 側で付けて渡すが念のため)
            if (!g_AndroidAssetCacheDir.empty() && g_AndroidAssetCacheDir.back() != '/') {
                g_AndroidAssetCacheDir += '/';
            }
        }
    }
}
