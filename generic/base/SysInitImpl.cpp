//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// System Initialization and Uninitialization
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <string>

#include <chrono>
#include <thread>

#include "SysInitImpl.h"
#include "StorageIntf.h"
#include "StorageImpl.h"
#include "MsgImpl.h"
#include "GraphicsLoaderIntf.h"
#include "DebugIntf.h"
#include "LogIntf.h"
#include "ThreadIntf.h"             // TVPDrawStatsLogEnabled
#include "tjsLex.h"
#include "LayerIntf.h"
#include "Random.h"
#include "XP3Archive.h"
#include "ScriptMgnIntf.h"
#include "XP3Archive.h"
#include "BinaryStream.h"
#include "Application.h"
#include "Exception.h"
#include "TickCount.h"
#include "CharacterSet.h"


//---------------------------------------------------------------------------
// global data
//---------------------------------------------------------------------------
tjs_string TVPNativeProjectDir;
tjs_string TVPNativeDataPath;
bool TVPProjectDirSelected = false;

//---------------------------------------------------------------------------
// CPU feature flags
//
// Win32 (= win32/environ/DetectCPU.cpp) ビルドではそちらが TVPCPUType を定義する。
// それ以外 (Linux/macOS/Android などの Generic / SDL3 ビルド) では本ファイルで
// 0 初期化したものを実体として持ち、TVPAfterSystemInit から共通の
// `TVPInitCPUFeatures()` を呼んで適切な値を入れる。
//
// 旧実装は ARM で「無条件に NEON フラグを立てる」「Linux/macOS の x86_64 では
// 何もしない」という個別ロジックを SysInit 内に直書きしていたが、検出ロジックは
// `common/visual/cpu_detect.cpp` の `TVPInitCPUFeatures()` に集約済み。
//---------------------------------------------------------------------------
#include "cpu_detect.h"
#include "tvpgl_ia32_intf.h"  // TVP_CPU_HAS_* マクロ
#if !defined(_WIN32)
extern "C" {
    tjs_uint32 TVPCPUType = 0;
}
#endif
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// System security options
//---------------------------------------------------------------------------
// system security options are held inside the executable, where
// signature checker will refer. This enables the signature checker
// (or other security modules like XP3 encryption module) to check
// the changes which is not intended by the contents author.
const static char TVPSystemSecurityOptions[] =
"-- TVPSystemSecurityOptions disablemsgmap(0):forcedataxp3(0):acceptfilenameargument(0):disableapplock(0) --";
//---------------------------------------------------------------------------
int GetSystemSecurityOption(const char *name)
{
	size_t namelen = TJS_nstrlen(name);
	const char *p = TJS_nstrstr(TVPSystemSecurityOptions, name);
	if(!p) return 0;
	if(p[namelen] == '(' && p[namelen + 2] == ')')
		return p[namelen+1] - '0';
	return 0;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
void TVPDumpHWException()
{
	// dummy
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// random generator initializer
//---------------------------------------------------------------------------
static void TVPInitRandomGenerator()
{
	// initialize random generator
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tjs_uint32));
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPInitializeBaseSystems
//---------------------------------------------------------------------------
#include "BitmapBitsAlloc.h"
#include "BinaryStreamBuffer.h"
#include "SoundAllocator.h"
#include "MemoryStatPeriodicDump.h"
#include "MemoryOverlay.h"
#include "PadOverlay.h"
void TVPInitializeBaseSystems()
{
	// set system archive delimiter
	tTJSVariant v;
	if(TVPGetCommandLine(TJS_W("-arcdelim"), &v))
		TVPArchiveDelimiter = ttstr(v)[0];

	// load message map file
	if (false) {
		// XXX セキュリティ的に問題
		bool load_msgmap = GetSystemSecurityOption("disablemsgmap") == 0;

		if(load_msgmap)
		{
			tjs_string name_msgmap = Application->AppPath() + TJS_W("msgmap.tjs");
			if (TVPIsExistentStorage(name_msgmap))
				TVPExecuteStorage(name_msgmap, NULL, false, TJS_W(""));
		}
	}

	// Bitmap / File アロケータをここで初期化する。
	// これ以降に TVPGetCommandLine 経由で参照されるオプション値は config.cf
	// 反映済みなので、Application->Create*Allocator() の中で
	// `-bitmapheapsize` `-fileheapsize` 等を読み取れる。
	tTVPBitmapBitsAlloc::InitializeAllocator();
	TVPInitializeFileAllocator();
	TVPInitializeSoundAllocator();

	// メモリ stats 周期ダンプ + 終了時ダンプ
	// (-memstatinterval=N / -memstatonexit=1 が効くようになる)。
	TVPInitializeMemoryStatPeriodicDump();

	// オーバレイ用サンプラ thread 起動 (SetEnabled で実際の収集が始まる)。
	TVPMemoryOverlay::Initialize();

	// -padoverlay=1 で起動時から PadOverlay を ON にする。
	TVPInitializePadOverlay();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPSetProjectPath( const ttstr& path ) 
{
	if( TVPProjectDirSelected == false ) {
		TVPProjectDirSelected = true;
		TVPProjectDir = TVPNormalizeStorageName(path);
		TVPSetCurrentDirectory(TVPProjectDir);
		TVPNativeProjectDir = path.AsStdString();
		TVPAddImportantLog( TVPFormatMessage(TVPInfoSelectedProjectDirectory, TVPProjectDir) );
	}
}

//---------------------------------------------------------------------------
// system initializer / uninitializer
//---------------------------------------------------------------------------
static tjs_uint64 TVPTotalPhysMemory = 0;
static void TVPInitProgramArgumentsAndDataPath(bool stop_after_datapath_got);
void TVPBeforeSystemInit()
{
	TVPInitProgramArgumentsAndDataPath(false); // ensure command line

	// randomize
	TVPInitRandomGenerator();

	// memory usage
	{
		TVPTotalPhysMemory = Application->GetTotalPhysMemory();

		ttstr memstr( to_tjs_string(TVPTotalPhysMemory).c_str() );
		TVPAddImportantLog( TVPFormatMessage(TVPInfoTotalPhysicalMemory, memstr) );

		tTJSVariant opt;
		if(TVPGetCommandLine(TJS_W("-memusage"), &opt))
		{
			ttstr str(opt);
			if(str == TJS_W("low"))
				TVPTotalPhysMemory = 0; // assumes zero
		}

		if(TVPTotalPhysMemory <= 36*1024*1024)
		{
			// very very low memory, forcing to assume zero memory
			TVPTotalPhysMemory = 0;
		}

		if(TVPTotalPhysMemory < 48*1024*1024ULL)
		{
			// extra low memory
			if(TJSObjectHashBitsLimit > 0)
				TJSObjectHashBitsLimit = 0;
			TVPSegmentCacheLimit = 0;
			TVPFreeUnusedLayerCache = true; // in LayerIntf.cpp
		}
		else if(TVPTotalPhysMemory < 64*1024*1024)
		{
			// low memory
			if(TJSObjectHashBitsLimit > 4)
				TJSObjectHashBitsLimit = 4;
		}
	}

	/**
	// ディレクトリチェック
	// 1. config.cf を読み、そこで指定されたフォルダから読み込む(未実装)
	// 2. data.xp3 があればそれを対象にする
	// 3. data/startup.tjs あればそれを対象にする
	tjs_char buf[MAX_PATH];
	bool selected = false;

	if (Application->ExistentFile(TJS_W("data.xp3"))) {
		TJS_strcpy(buf, TJS_W("data.xp3>"));
		tjs_int buflen = TJS_strlen(buf);
		buf[buflen] = TVPArchiveDelimiter, buf[buflen+1] = 0;
		selected = true;
	}
	*/

	TVPSetProjectPath(Application->ProjectPath());
}

//---------------------------------------------------------------------------
static void TVPDumpOptions();

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
extern "C" {
	extern void TVPGL_IA32_Init();
}
extern void TVPGL_SSE2_Init();
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
extern void TVPGL_NEON_Init();
#endif

//---------------------------------------------------------------------------
static tjs_uint TVPTimeBeginPeriodRes = 0;
//---------------------------------------------------------------------------
void TVPAfterSystemInit()
{
	// ensure datapath directory
	TVPEnsureDataPathDirectory();

	TVPAllocGraphicCacheOnHeap = false; // always false since beta 20

	// determine maximum graphic cache limit
	tTJSVariant opt;
	tjs_int64 limitmb = -1;
	if(TVPGetCommandLine(TJS_W("-gclim"), &opt))
	{
		ttstr str(opt);
		if(str == TJS_W("auto"))
			limitmb = -1;
		else
			limitmb = opt.AsInteger();
	}


	// BitmapAllocator pool が有効なら、その capacity を ImageCache 上限として
	// 採用する (StorageCache が FileAllocator capacity と揃えているのと同じ理屈;
	// 1e413695 参照)。ImageCache に積まれる Bitmap は最終的にこの pool から
	// 確保されるので、pool 容量を超えて cache を許す意味はなく、逆に物理
	// メモリ÷10 + 512MB cap で頭打ちにすると pool を広げても cache が
	// それに追従しない。
	// BasicAllocator (raw malloc, capacity=SIZE_MAX) の場合のみ、従来の
	// 物理メモリ算出にフォールバックする。
	const char *limitSource = (limitmb == -1) ? "auto" : "-gclim";
	tjs_uint64 poolCap = 0;
	{
		iTVPMemoryAllocator *bmpAlloc = tTVPBitmapBitsAlloc::GetAllocator();
		if(bmpAlloc) {
			size_t cap = bmpAlloc->capacity();
			if(cap != SIZE_MAX) poolCap = cap;
		}
	}
	if(limitmb == -1)
	{
		if(poolCap > 0) {
			TVPGraphicCacheSystemLimit = poolCap;
			limitSource = "BitmapPool";
		} else {
			if(TVPTotalPhysMemory <= 32*1024*1024)
				TVPGraphicCacheSystemLimit = 0;
			else if(TVPTotalPhysMemory <= 48*1024*1024)
				TVPGraphicCacheSystemLimit = 0;
			else if(TVPTotalPhysMemory <= 64*1024*1024)
				TVPGraphicCacheSystemLimit = 0;
			else if(TVPTotalPhysMemory <= 96*1024*1024)
				TVPGraphicCacheSystemLimit = 4;
			else if(TVPTotalPhysMemory <= 128*1024*1024)
				TVPGraphicCacheSystemLimit = 8;
			else if(TVPTotalPhysMemory <= 192*1024*1024)
				TVPGraphicCacheSystemLimit = 12;
			else if(TVPTotalPhysMemory <= 256*1024*1024)
				TVPGraphicCacheSystemLimit = 20;
			else if(TVPTotalPhysMemory <= 512*1024*1024)
				TVPGraphicCacheSystemLimit = 40;
			else
				TVPGraphicCacheSystemLimit = tjs_uint64(TVPTotalPhysMemory / (1024*1024*10));	// cachemem = physmem / 10
			TVPGraphicCacheSystemLimit *= 1024*1024;
			// BasicAllocator 経路のレガシー 512MB cap (32bit 想定)
			if( TVPGraphicCacheSystemLimit >= 512*1024*1024 )
				TVPGraphicCacheSystemLimit = 512*1024*1024;
		}
	}
	else
	{
		TVPGraphicCacheSystemLimit = limitmb * 1024*1024;
		// -gclim 指定でも pool cap を超えた値は意味がないので頭打ち
		if(poolCap > 0 && TVPGraphicCacheSystemLimit > poolCap) {
			TVPGraphicCacheSystemLimit = poolCap;
			limitSource = "-gclim (capped by BitmapPool)";
		}
	}

	// SystemLimit が決まったらデフォルトで cache を有効化。
	// (これを入れる前は System.graphicCacheLimit = -1 を呼ばないと cache が
	//  無効のままで Bitmap.load 等の cache push が空回りしていた)
	if(TVPGraphicCacheSystemLimit > 0) {
		TVPSetGraphicCacheLimit(TVPGraphicCacheSystemLimit);
		TVPLOG_INFO("ImageCache enabled: limit={}MB (physmem={}MB, source={})",
		            TVPGetGraphicCacheLimit() / (1024*1024),
		            TVPTotalPhysMemory / (1024*1024),
		            limitSource);
	} else {
		TVPLOG_INFO("ImageCache disabled (physmem={}MB too small)",
		            TVPTotalPhysMemory / (1024*1024));
	}

	if(TVPTotalPhysMemory <= 64*1024*1024)
		TVPSetFontCacheForLowMem();


	// SDL3/GL バックエンドの既定は gsotNone。
	// InternalComplete2 の更新領域 8 行帯分割 (gsotSimple) は GDI 旧経路向けの
	// CPU キャッシュ最適化で、SDL3 では「帯の数 (例: 1080/8=135) ぶんの合成 +
	// テクスチャ更新発行」のオーバーヘッドが支配的になり全面動画再生等で激しく
	// 低速化する (実測 9fps→57fps)。スレッドプールが大ブリットを自前で分割するため
	// 帯分割は不要。下の -gsplit オプションがあればそちらが優先。
	TVPGraphicSplitOperationType = gsotNone;

	// check TVPGraphicSplitOperation option
	if(TVPGetCommandLine(TJS_W("-gsplit"), &opt))
	{
		ttstr str(opt);
		if(str == TJS_W("no"))
			TVPGraphicSplitOperationType = gsotNone;
		else if(str == TJS_W("int"))
			TVPGraphicSplitOperationType = gsotInterlace;
		else if(str == TJS_W("yes") || str == TJS_W("simple"))
			TVPGraphicSplitOperationType = gsotSimple;
		else if(str == TJS_W("bidi"))
			TVPGraphicSplitOperationType = gsotBiDirection;

	}

	// check TVPDefaultHoldAlpha option
	if(TVPGetCommandLine(TJS_W("-holdalpha"), &opt))
	{
		ttstr str(opt);
		if(str == TJS_W("yes") || str == TJS_W("true"))
			TVPDefaultHoldAlpha = true;
		else
			TVPDefaultHoldAlpha = false;
	}

	// check TVPJPEGFastLoad option
	if(TVPGetCommandLine(TJS_W("-jpegdec"), &opt)) // this specifies precision for JPEG decoding
	{
		ttstr str(opt);
		if(str == TJS_W("normal"))
			TVPJPEGLoadPrecision = jlpMedium;
		else if(str == TJS_W("low"))
			TVPJPEGLoadPrecision = jlpLow;
		else if(str == TJS_W("high"))
			TVPJPEGLoadPrecision = jlpHigh;

	}

	// dump option
	TVPDumpOptions();

	// CPU 機能を検出して TVPCPUType をセットする (portable entry)。
	// x86 では cpuid + (MSVC では SEH 経由 / GCC/Clang では __builtin_cpu_supports
	// 経由の) AVX OS-support 検査、ARM64 では baseline 必須として NEON/ASIMD 立て、
	// ARMv7 Linux/Android では HWCAP 経由検出。詳細は cpu_detect.h コメント参照。
	TVPInitCPUFeatures();
	TVPApplyCPUFeatureOverrides();

	// 検出した CPU 機能に応じて画像処理 SIMD ルーチンを wire-up する。
	// 各 _Init は内部で TVPCPUType の対応ビットを見て自分が動くべきか判定する。
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
	TVPGL_IA32_Init();
	TVPGL_SSE2_Init();    // 内部で SSE2 / AVX2 を見て chain
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
	TVPGL_NEON_Init();    // 内部で TVP_CPU_HAS_ARM_NEON を見て dispatch
#endif

	// 検出 + override + dispatch 後の SIMD 有効状況を 1 行サマリで残す。
	// Win32 では TVPDetectCPU() が per-CPU 詳細を別途吐くが、Generic / SDL3
	// (Linux / macOS / Android / iOS など) にはそれが無いので、NEON / SSE2 等が
	// どう wire-up されたかをここで明示する。NEON がランタイムで有効になっている
	// ことを Android logcat から目視確認できるようにするのが主目的。
	{
		std::string feats;
		auto append_flag = [&feats](bool on, const char *name) {
			if (!on) return;
			if (!feats.empty()) feats += " ";
			feats += name;
		};
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
		append_flag(TVPCPUType & TVP_CPU_HAS_MMX,   "MMX");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSE,   "SSE");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSE2,  "SSE2");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSE3,  "SSE3");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSSE3, "SSSE3");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSE41, "SSE4.1");
		append_flag(TVPCPUType & TVP_CPU_HAS_SSE42, "SSE4.2");
		append_flag(TVPCPUType & TVP_CPU_HAS_AVX,   "AVX");
		append_flag(TVPCPUType & TVP_CPU_HAS_AVX2,  "AVX2");
		append_flag(TVPCPUType & TVP_CPU_HAS_FMA3,  "FMA3");
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
		append_flag(TVPCPUType & TVP_CPU_HAS_ARM_NEON,    "NEON");
		append_flag(TVPCPUType & TVP_CPU_HAS_ARM64_ASIMD, "ASIMD");
#endif
		if (feats.empty()) feats = "(none)";
		TVPLOG_INFO("CPU SIMD: {} (TVPCPUType=0x{:08x})",
		            feats, (unsigned)TVPCPUType);
	}

	// timer precision
	tjs_uint prectick = 1;
	if(TVPGetCommandLine(TJS_W("-timerprec"), &opt))
	{
		ttstr str(opt);
		if(str == TJS_W("high")) prectick = 1;
		if(str == TJS_W("higher")) prectick = 5;
		if(str == TJS_W("normal")) prectick = 10;
	}

        // draw thread num
        tjs_int drawThreadNum = Application->DrawThreadNum();
        if (TVPGetCommandLine(TJS_W("-drawthread"), &opt)) {
          ttstr str(opt);
          if (str == TJS_W("auto"))
            drawThreadNum = 0;
          else
            drawThreadNum = (tjs_int)opt;
        }
        TVPDrawThreadNum = drawThreadNum;

        // DrawStats log output (KRKRZ_DRAW_STATS=ON ビルド時のみ意味あり)。
        // TJS の System.setDrawStatsLog(true) と同じものを起動オプションで
        // 切り替えるためのフック。Android のように TJS hook を仕込みづらい
        // 環境で config.cf 経由で有効化したいケース用。
        if (TVPGetCommandLine(TJS_W("-drawstatslog"), &opt)) {
            ttstr str(opt);
            if (str == TJS_W("yes") || str == TJS_W("true") || str == TJS_W("1"))
                TVPDrawStatsLogEnabled = true;
            else
                TVPDrawStatsLogEnabled = false;
        }
#if 0

	TVPPushEnvironNoise(&TVPCPUType, sizeof(TVPCPUType));

	// set LFH
	if(TVPGetCommandLine(TJS_W("-uselfh"), &opt)) {
		ttstr str(opt);
		if(str == TJS_W("yes") || str == TJS_W("true")) {
			ULONG HeapInformation = 2;
			BOOL lfhenable = ::HeapSetInformation( GetProcessHeap(), HeapCompatibilityInformation, &HeapInformation, sizeof(HeapInformation) );
			if( lfhenable ) {
				TVPAddLog( TJS_W("(info) Enable LFH") );
			} else {
				TVPAddLog( TJS_W("(info) Cannot Enable LFH") );
			}
		}
	}
#endif
}
//---------------------------------------------------------------------------

void TVPBeforeSystemUninit()
{
	// TVPDumpHWException(); // dump cached hw exceptoin
}
//---------------------------------------------------------------------------

void TVPAfterSystemUninit()
{
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
bool TVPTerminateOnWindowClose = true;
bool TVPTerminateOnNoWindowStartup = true;
//---------------------------------------------------------------------------
void TVPTerminateAsync(int code)
{
	// do "A"synchronous temination of application
	TVPCallDeliverAllEventsOnIdle();
	Application->Terminate(code);
	TVPCallDeliverAllEventsOnIdle();
}
//---------------------------------------------------------------------------
void TVPTerminateSync(int code)
{
	// do synchronous temination of application (never return)
	TVPSystemUninit();
	Application->Exit(code);
}
//---------------------------------------------------------------------------
void TVPMainWindowClosed()
{
	// called from WindowIntf.cpp, caused by closing all window.
	if( TVPTerminateOnWindowClose) TVPTerminateAsync();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// GetCommandLine
//---------------------------------------------------------------------------
static std::vector<std::string> * TVPGetEmbeddedOptions()
{
	tjs_string path = Application->ResourcePath() + TJS_W("config.cf");
	return TVPReadLines(path.c_str());
}

//---------------------------------------------------------------------------
static std::vector<std::string> * TVPGetConfigFileOptions(const tjs_string& filename)
{
	// load .cf file
	tjs_string errmsg;
	if(!TVPCheckExistentLocalFile(filename.c_str()))
		errmsg = (const tjs_char*)TVPFileNotFound;

	std::vector<std::string> * ret = NULL; // new std::vector<std::string>();
	if(errmsg == TJS_W(""))
	{
		try
		{
			ret = TVPReadLines(filename.c_str());
		}
		catch(Exception & e)
		{
			errmsg = e.what();
		}
		catch(...)
		{
			delete ret;
			throw;
		}
	}

	if(errmsg != TJS_W(""))
		TVPAddImportantLog( TVPFormatMessage(TVPInfoLoadingConfigurationFileFailed, filename.c_str(), errmsg.c_str()) );
	else
		TVPAddImportantLog( TVPFormatMessage(TVPInfoLoadingConfigurationFileSucceeded, filename.c_str()) );

	return ret;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------

static ttstr TVPParseCommandLineOne(const ttstr &i)
{
	// value is specified
	const tjs_char *p, *o;
	p = o = i.c_str();
	p = TJS_strchr(p, '=');

	if(p == NULL) { return i + TJS_W("=yes"); }

	p++;

	ttstr optname(o, p - o);

	if(*p == TJS_W('\'') || *p == TJS_W('\"'))
	{
		// as an escaped string
		tTJSVariant v;
		TJSParseString(v, &p);

		return optname + ttstr(v);
	}
	else
	{
		// as a string
		return optname + p;
	}
}
//---------------------------------------------------------------------------
std::vector <ttstr> TVPProgramArguments;
static bool TVPProgramArgumentsInit = false;
static tjs_int TVPCommandLineArgumentGeneration = 0;
static bool TVPDataPathDirectoryEnsured = false;
//---------------------------------------------------------------------------
tjs_int TVPGetCommandLineArgumentGeneration() { return TVPCommandLineArgumentGeneration; }
//---------------------------------------------------------------------------
void TVPEnsureDataPathDirectory()
{
	if(!TVPDataPathDirectoryEnsured)
	{
		TVPDataPathDirectoryEnsured = true;
		// ensure data path existence
		if(!TVPCheckExistentLocalFolder(TVPNativeDataPath.c_str()))
		{
			if(TVPCreateFolders(TVPNativeDataPath.c_str()))
				TVPAddImportantLog( TVPFormatMessage( TVPInfoDataPathDoesNotExistTryingToMakeIt, (const tjs_char*)TVPOk ) );
			else
				TVPAddImportantLog( TVPFormatMessage( TVPInfoDataPathDoesNotExistTryingToMakeIt, (const tjs_char*)TVPFaild ) );
		}
	}
}
//---------------------------------------------------------------------------
static void PushAllCommandlineArguments()
{
	// store arguments given by commandline to "TVPProgramArguments"
	bool acceptfilenameargument = GetSystemSecurityOption("acceptfilenameargument") != 0;

	bool argument_stopped = false;
	if(acceptfilenameargument) argument_stopped = true;
	int file_argument_count = 0;
	for(tjs_int i = 1; i< Application->GetArgumentCount(); i++)
	{
		const tjs_char *arg = Application->GetArgument(i);
		if(argument_stopped)
		{
			ttstr arg_name_and_value = TJS_W("-arg") + ttstr(file_argument_count) + TJS_W("=")
				+ ttstr(arg);
			file_argument_count++;
			TVPProgramArguments.push_back(arg_name_and_value);
		}
		else
		{
			if(arg[0] == '-')
			{
				if(arg[1] == '-' && arg[2] == 0)
				{
					// argument stopper
					argument_stopped = true;
				}
				else
				{
					ttstr value(arg);
					if(!TJS_strchr(value.c_str(), TJS_W('=')))
						value += TJS_W("=yes");
					TVPProgramArguments.push_back(TVPParseCommandLineOne(value));
				}
			}
		}
	}
}

//---------------------------------------------------------------------------
static void PushConfigFileOptions(const std::vector<std::string> * options)
{
	if(!options) return;
	for(unsigned int j = 0; j < options->size(); j++)
	{
		if( (*options)[j].c_str()[0] != ';') // unless comment
			TVPProgramArguments.push_back(
			TVPParseCommandLineOne(TJS_W("-") + ttstr((*options)[j].c_str())));
	}
}
//---------------------------------------------------------------------------
static void TVPInitProgramArgumentsAndDataPath(bool stop_after_datapath_got)
{
	if(!TVPProgramArgumentsInit)
	{
		TVPProgramArgumentsInit = true;


		// find options from self executable image
		const int num_option_layers = 3;
		std::vector<std::string> * options[num_option_layers];
		for(int i = 0; i < num_option_layers; i++) options[i] = NULL;
		try
		{
			// read embedded options and default configuration file
			options[0] = TVPGetEmbeddedOptions();
			options[1] = TVPGetConfigFileOptions( Application->AppPath() + TJS_W("config.cf") );

			// at this point, we need to push all exsting known options
			// to be able to see datapath
			PushAllCommandlineArguments();
			PushConfigFileOptions(options[1]); // has more priority
			PushConfigFileOptions(options[0]); // has lesser priority

			TVPNativeDataPath = Application->InitDataPath();

			if(stop_after_datapath_got) return;

			// read per-user configuration file
			// TVPNativeDataPath がローカルパスでないとこれだと開けない
			if (false) {
				ttstr fname  = TVPNativeDataPath;
				fname += TVPChopStorageExt(TVPExtractStorageName(ttstr(Application->ExePath().c_str())));
				fname += TJS_W(".cfu");	
				options[2] = TVPGetConfigFileOptions(fname.c_str());
			}

			// push each options into option stock
			// we need to clear TVPProgramArguments first because of the
			// option priority order.
			TVPProgramArguments.clear();
			PushAllCommandlineArguments();
			PushConfigFileOptions(options[2]); // has more priority
			PushConfigFileOptions(options[1]); // has more priority
			PushConfigFileOptions(options[0]); // has lesser priority
		} catch(...) {
			for(int i = 0; i < num_option_layers; i++)
				if(options[i]) delete options[i];
			throw;
		}
		for(int i = 0; i < num_option_layers; i++)
			if(options[i]) delete options[i];


		// set data path
		TVPDataPath = TVPNormalizeStorageName(TVPNativeDataPath);
		TVPAddImportantLog( TVPFormatMessage( TVPInfoDataPath, TVPDataPath) );

		// set log output directory
		TVPSetLogLocation(Application->LogPath());

		// increment TVPCommandLineArgumentGeneration
		TVPCommandLineArgumentGeneration++;
	}
}
//---------------------------------------------------------------------------
static void TVPDumpOptions()
{
	std::vector<ttstr>::const_iterator i;
 	ttstr options( TVPInfoSpecifiedOptionEarlierItemHasMorePriority );
	if(TVPProgramArguments.size())
	{
		for(i = TVPProgramArguments.begin(); i != TVPProgramArguments.end(); i++)
		{
			options += TJS_W(" ");
			options += *i;
		}
	}
	else
	{
		options += (const tjs_char*)TVPNone;
	}
	TVPAddImportantLog(options);
}
//---------------------------------------------------------------------------
bool TVPGetCommandLine(const tjs_char * name, tTJSVariant *value)
{
	TVPInitProgramArgumentsAndDataPath(false);

	tjs_int namelen = TJS_strlen(name);
	std::vector<ttstr>::const_iterator i;
	for(i = TVPProgramArguments.begin(); i != TVPProgramArguments.end(); i++)
	{
		if(!TJS_strncmp(i->c_str(), name, namelen))
		{
			if(i->c_str()[namelen] == TJS_W('='))
			{
				// value is specified
				const tjs_char *p = i->c_str() + namelen + 1;
				if(value) *value = p;
				return true;
			}
			else if(i->c_str()[namelen] == 0)
			{
				// value is not specified
				if(value) *value = TJS_W("yes");
				return true;
			}
		}
	}
	return false;
}
//---------------------------------------------------------------------------
void TVPSetCommandLine(const tjs_char * name, const ttstr & value)
{
	TVPInitProgramArgumentsAndDataPath(false);

	tjs_int namelen = TJS_strlen(name);
	std::vector<ttstr>::iterator i;
	for(i = TVPProgramArguments.begin(); i != TVPProgramArguments.end(); i++)
	{
		if(!TJS_strncmp(i->c_str(), name, namelen))
		{
			if(i->c_str()[namelen] == TJS_W('=') || i->c_str()[namelen] == 0)
			{
				// value found
				*i = ttstr(i->c_str(), namelen) + TJS_W("=") + value;
				TVPCommandLineArgumentGeneration ++;
				if(TVPCommandLineArgumentGeneration == 0) TVPCommandLineArgumentGeneration = 1;
				return;
			}
		}
	}

	// value not found; insert argument into front
	TVPProgramArguments.insert(TVPProgramArguments.begin(), ttstr(name) + TJS_W("=") + value);
	TVPCommandLineArgumentGeneration ++;
	if(TVPCommandLineArgumentGeneration == 0) TVPCommandLineArgumentGeneration = 1;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPExecuteAsync
//---------------------------------------------------------------------------
static void TVPExecuteAsync( const tjs_string& progname)
{
#if 0
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWNORMAL;

	BOOL ret =
		CreateProcess(
			NULL,
			const_cast<LPTSTR>(progname.c_str()),
			NULL,
			NULL,
			FALSE,
			0,
			NULL,
			NULL,
			&si,
			&pi);

	if(ret)
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return;
	}

	throw Exception(ttstr(TVPExecutionFail).AsStdString());
#endif
	// TODO インテントを投げる実装にした方が良い
}
//---------------------------------------------------------------------------





