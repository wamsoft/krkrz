/**
 * SDL用の resource:// インターフェース
 * 相対パスファイルでアクセスすると各環境のリソースデータにアクセスできる
 */

#include "tjsCommHead.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "SysInitIntf.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_storage.h>

#define MEDIA_NAME TJS_W("resource")

// FileImpl.cpp で定義・SDLのファイルパスからオープン
extern iTJSBinaryStream *CreateStreamFromSDL(const char *path, const tjs_uint32 flags);

//---------------------------------------------------------------------------

class SDLResourceMedia : public iTVPStorageMedia
{
	static SDLResourceMedia *Instance;
	tjs_int RefCount;
	
public:
	SDLResourceMedia() : RefCount(1) {
		// リソース部一覧テスト
		const char *path = SDL_GetBasePath();
		SDL_EnumerateDirectory(path, [](void *userdata, const char *dirname, const char *fname) {
			TVPAddLog(ttstr(TJS_W("Found resource: ")) + ttstr(fname));
			return SDL_ENUM_CONTINUE;
        }, nullptr);
	}

    ~SDLResourceMedia() {
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

    void getName(const ttstr &name, std::string &fname) {
		// ドメイン名とそれ以降を分離・ドメイン部は無視
		const tjs_char *p = name.c_str();
		const tjs_char *q;
		if ((q = TJS_strchr(p, '/'))) {
    	    TVPUtf16ToUtf8(fname, q+1); // skip "/"
			TVPAddLog(ttstr(TJS_W("Resource path: ")) + ttstr(fname.c_str()));
		} else {
			TVPThrowExceptionMessage(TJS_W("invalid path:%1"), name);
		}
    }

	virtual bool TJS_INTF_METHOD CheckExistentStorage(const ttstr &name) override {
		std::string path;
		getName(name, path);
		SDL_PathInfo info;
        return SDL_GetPathInfo(path.c_str(), &info) && info.type != SDL_PATHTYPE_NONE;
	}

    virtual iTJSBinaryStream * TJS_INTF_METHOD Open(const ttstr & name, tjs_uint32 flags) override {
		std::string path;
		getName(name, path);
		tjs_uint32 access = flags & TJS_BS_ACCESS_MASK;
        if (access == TJS_BS_READ) {
			return CreateStreamFromSDL(path.c_str(), flags);
        }

        TVPThrowExceptionMessage(TJS_W("Failed to open storage:readonly media"));
	}

    virtual void TJS_INTF_METHOD GetListAt(const ttstr &name, iTVPStorageLister * lister) override {
		std::string path;
		getName(name, path);
		SDL_EnumerateDirectory(path.c_str(), [](void *userdata, const char *dirname, const char *fname) {
			iTVPStorageLister * lister = static_cast<iTVPStorageLister *>(userdata);
			tjs_string tjs_filename;
            TVPUtf8ToUtf16(tjs_filename, fname);
			lister->Add(tjs_filename.c_str());
			return SDL_ENUM_CONTINUE;
        }, lister);
    }

    virtual void TJS_INTF_METHOD GetLocallyAccessibleName(ttstr &name) override { 
		name = "";
    }

	static void Load() {
		if(!Instance) {
			Instance = new SDLResourceMedia();
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
};

SDLResourceMedia* SDLResourceMedia::Instance = nullptr;

static tTVPAtStart AtStart(TVP_ATSTART_PRI_PREPARE, SDLResourceMedia::Load);
static tTVPAtExit  AtExit(TVP_ATEXIT_PRI_PREPARE, SDLResourceMedia::Unload);
