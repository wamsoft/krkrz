//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Video Overlay support implementation
//---------------------------------------------------------------------------


#include "tjsCommHead.h"

#include <algorithm>
#include "MsgImpl.h"
#include "VideoOvlImpl.h"
#include "DebugIntf.h"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "SysInitIntf.h"
#include "StorageImpl.h"
#include "krmovie.h"
#include "PluginImpl.h"
#include "WaveImpl.h"  // for DirectSound attenuate <-> TVP volume
#include <evcode.h>

#include "Application.h"
#include "BitmapInfomation.h"
#include "VideoPresenterD3D.h"
#include "LayerBitmapIntf.h"

//---------------------------------------------------------------------------
// Track V-A: krmovie は exe へ静的統合されたので、そのエクスポート関数を直接
// リンクして呼ぶ (LoadLibrary/GetProcAddress を経由しない)。krflash.dll は従来通り
// DLL ロードなので tTVPVideoModule(name) 経路を残す。
extern "C" {
	void    __stdcall GetAPIVersion( DWORD *ver );
	void    __stdcall GetVideoOverlayObject( HWND, IStream*, const tjs_char*, const tjs_char*, unsigned __int64, iTVPVideoOverlay** );
	void    __stdcall GetVideoLayerObject( HWND, IStream*, const tjs_char*, const tjs_char*, unsigned __int64, iTVPVideoOverlay** );
	void    __stdcall GetMFVideoOverlayObject( HWND, IStream*, const tjs_char*, const tjs_char*, unsigned __int64, iTVPVideoOverlay** );
	HRESULT __stdcall V2Link( iTVPFunctionExporter *exporter );
	HRESULT __stdcall V2Unlink();
	// Track V-E HW: IMFMediaEngine バックエンドのファクトリ (engineDevice=ID3D11Device*)。
	void    __stdcall GetMediaEngineVideoObject( HWND, IStream*, const tjs_char*, const tjs_char*, unsigned __int64, void*, iTVPVideoOverlay**, iTVPVideoPresenter** );
}

class tTVPVideoModule
{
	tTVPPluginHolder *Holder;
	HMODULE Handle;
	tGetAPIVersion procGetAPIVersion;
	tGetVideoOverlayObject procGetVideoOverlayObject;
	tGetVideoOverlayObject procGetVideoLayerObject; // krmovie.dll only
	tGetVideoOverlayObject procGetMFVideoOverlayObject; // krmovie.dll only
	tTVPV2LinkProc procV2Link;
	tTVPV2UnlinkProc procV2Unlink;

public:
	tTVPVideoModule();               // 静的統合された krmovie を直接束ねる
	tTVPVideoModule(const ttstr & name); // DLL ロード (krflash.dll 用)
	~tTVPVideoModule();

	void GetAPIVersion(DWORD *version) { procGetAPIVersion(version); }
	void GetVideoOverlayObject(HWND callbackwin, IStream *stream,
		const wchar_t * streamname, const wchar_t *type, unsigned __int64 size,
		iTVPVideoOverlay **out)
	{
		procGetVideoOverlayObject(callbackwin, stream, streamname, type, size, out);
	}
	void GetVideoLayerObject(HWND callbackwin, IStream *stream,
		const wchar_t * streamname, const wchar_t *type, unsigned __int64 size,
		iTVPVideoOverlay **out)
	{
		procGetVideoLayerObject(callbackwin, stream, streamname, type, size, out);
	}
	void GetMFVideoOverlayObject(HWND callbackwin, IStream *stream,
		const wchar_t * streamname, const wchar_t *type, unsigned __int64 size,
		iTVPVideoOverlay **out)
	{
		procGetMFVideoOverlayObject(callbackwin, stream, streamname, type, size, out);
	}
};
static tTVPVideoModule *TVPMovieVideoModule = NULL;
static tTVPVideoModule *TVPFlashVideoModule = NULL;
static void TVPUnloadKrMovie();
//---------------------------------------------------------------------------
// 静的統合された krmovie を直接束ねる (LoadLibrary 不要)。
tTVPVideoModule::tTVPVideoModule()
{
	Holder = NULL;
	Handle = NULL;
	procGetVideoOverlayObject   = (tGetVideoOverlayObject)&::GetVideoOverlayObject;
	procGetVideoLayerObject     = (tGetVideoOverlayObject)&::GetVideoLayerObject;
	procGetMFVideoOverlayObject = (tGetVideoOverlayObject)&::GetMFVideoOverlayObject;
	procGetAPIVersion           = (tGetAPIVersion)&::GetAPIVersion;
	procV2Link                  = (tTVPV2LinkProc)&::V2Link;
	procV2Unlink                = (tTVPV2UnlinkProc)&::V2Unlink;

	DWORD version;
	procGetAPIVersion(&version);
	if(version != TVP_KRMOVIE_VER)
		TVPThrowExceptionMessage(TVPInvalidKrMovieDLL);

	procV2Link(TVPGetFunctionExporter()); // link functions used by tp_stub
}
//---------------------------------------------------------------------------
tTVPVideoModule::tTVPVideoModule(const ttstr &name)
{
	Holder = new tTVPPluginHolder(name);
	Handle = LoadLibrary((const wchar_t*)Holder->GetLocalName().AsStdString().c_str());
	if(!Handle)
	{
		delete Holder;
		TVPThrowExceptionMessage(TVPCannotLoadKrMovieDLL);
	}

	try
	{
		procGetVideoOverlayObject = (tGetVideoOverlayObject)
			GetProcAddress(Handle, "GetVideoOverlayObject");

		procGetVideoLayerObject = (tGetVideoOverlayObject)
			GetProcAddress(Handle, "GetVideoLayerObject");

		procGetMFVideoOverlayObject = (tGetVideoOverlayObject)
			GetProcAddress(Handle, "GetMFVideoOverlayObject");

		procGetAPIVersion = (tGetAPIVersion)
			GetProcAddress(Handle, "GetAPIVersion");

		procV2Link = (tTVPV2LinkProc)
			GetProcAddress(Handle, "V2Link");

		procV2Unlink = (tTVPV2UnlinkProc)
			GetProcAddress(Handle, "V2Unlink");

		if(!procGetAPIVersion)
			TVPThrowExceptionMessage(TVPInvalidKrMovieDLL);

		DWORD version;
		procGetAPIVersion(&version);
		if(version != TVP_KRMOVIE_VER)
			TVPThrowExceptionMessage(TVPInvalidKrMovieDLL);

		procV2Link(TVPGetFunctionExporter()); // link functions used by tp_stub
	}
	catch(...)
	{
		FreeLibrary(Handle);
		delete Holder;
		throw;
	}
}
//---------------------------------------------------------------------------
tTVPVideoModule::~tTVPVideoModule()
{
	procV2Unlink();
	if(Handle) FreeLibrary(Handle); // 静的統合 (krmovie) 時は Handle=NULL
	if(Holder) delete Holder;
}
//---------------------------------------------------------------------------
static tTVPVideoModule * TVPGetMovieVideoModule()
{
	if(TVPMovieVideoModule == NULL)
		TVPMovieVideoModule = new tTVPVideoModule(); // 静的統合 krmovie

	return TVPMovieVideoModule;
}
//---------------------------------------------------------------------------
static tTVPVideoModule * TVPGetFlashVideoModule()
{
	if(TVPFlashVideoModule == NULL)
		TVPFlashVideoModule = new tTVPVideoModule("krflash.dll");

	return TVPFlashVideoModule;
}
//---------------------------------------------------------------------------
static std::vector<tTJSNI_VideoOverlay *> TVPVideoOverlayVector;
//---------------------------------------------------------------------------
static void TVPAddVideOverlay(tTJSNI_VideoOverlay *ovl)
{
	TVPVideoOverlayVector.push_back(ovl);
}
//---------------------------------------------------------------------------
static void TVPRemoveVideoOverlay(tTJSNI_VideoOverlay *ovl)
{
	std::vector<tTJSNI_VideoOverlay*>::iterator i;
	i = std::find(TVPVideoOverlayVector.begin(), TVPVideoOverlayVector.end(), ovl);
	if(i != TVPVideoOverlayVector.end())
		TVPVideoOverlayVector.erase(i);
}
//---------------------------------------------------------------------------
static void TVPShutdownVideoOverlay()
{
	// shutdown all overlay object and release krmovie.dll / krflash.dll
	std::vector<tTJSNI_VideoOverlay*>::iterator i;
	for(i = TVPVideoOverlayVector.begin(); i != TVPVideoOverlayVector.end(); i++)
	{
		(*i)->Shutdown();
	}

	if(TVPMovieVideoModule) delete TVPMovieVideoModule, TVPMovieVideoModule = NULL;
	if(TVPFlashVideoModule) delete TVPFlashVideoModule, TVPFlashVideoModule = NULL;
}
static tTVPAtExit TVPShutdownVideoOverlayAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPShutdownVideoOverlay);
//---------------------------------------------------------------------------
// Track V-E HW: MF-native 形式か (IMFMediaEngine で HW デコード可能)。webm/mpg/avi は除外。
static bool TVPIsMediaEngineFormat( const ttstr &ext )
{
	return ext == TJS_W(".mp4") || ext == TJS_W(".m4v") || ext == TJS_W(".mov")
		|| ext == TJS_W(".wmv") || ext == TJS_W(".asf");
}
//---------------------------------------------------------------------------
// HW 動画 (IMFMediaEngine) を使うか。既定 true。-mediaengine=no/off/false/0 で無効化。
static bool TVPUseMediaEngine()
{
	static int cached = -1; // -1 未評価 / 0 無効 / 1 有効
	if( cached < 0 )
	{
		cached = 1;
		tTJSVariant val;
		if( TVPGetCommandLine( TJS_W("-mediaengine"), &val ) )
		{
			ttstr s = val; s.ToLowerCase();
			if( s == TJS_W("no") || s == TJS_W("off") || s == TJS_W("false") || s == TJS_W("0") )
				cached = 0;
		}
	}
	return cached != 0;
}
//---------------------------------------------------------------------------




//---------------------------------------------------------------------------
// tTJSNI_VideoOverlay
//---------------------------------------------------------------------------
tTJSNI_VideoOverlay::tTJSNI_VideoOverlay()
: EventQueue(this,&tTJSNI_VideoOverlay::WndProc)
{
	VideoOverlay = NULL;
	Rect.left = 0;
	Rect.top = 0;
	Rect.right = 320;
	Rect.bottom = 240;
	Visible = false;
	OwnerWindow = NULL;
	LocalTempStorageHolder = NULL;

	EventQueue.Allocate();

	Layer1 = NULL;
	Layer2 = NULL;
	Mode = vomOverlay;
	Loop = false;
	IsPrepare = false;
	SegLoopStartFrame = -1;
	SegLoopEndFrame = -1;
	IsEventPast = false;
	EventFrame = -1;

	Bitmap[0] = Bitmap[1] = NULL;
	BmpBits[0] = BmpBits[1] = NULL;

	UsePresenter = false;
	HWMode = false;
	ActivePresenter = static_cast<iTVPVideoPresenter*>(this);
	PresenterHost = NULL;
	PresenterRegistered = false;
	HasFrame = false;
	MovieAlpha = 1.0;
	VideoBlit = NULL;
	MixerBlit = NULL;
	MixerBitmap = NULL;
	MixerRect = tTVPRect(0, 0, 0, 0);
	MixerAlpha = 1.0;
}
//---------------------------------------------------------------------------
tjs_error TJS_INTF_METHOD
tTJSNI_VideoOverlay::Construct(tjs_int numparams, tTJSVariant **param,
		iTJSDispatch2 *tjs_obj)
{
	tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
	if(TJS_FAILED(hr)) return hr;

	return TJS_S_OK;
}
//---------------------------------------------------------------------------
void TJS_INTF_METHOD tTJSNI_VideoOverlay::Invalidate()
{
	inherited::Invalidate();

	Close();

	EventQueue.Deallocate();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Open(const ttstr &_name)
{
	// open

	// first, close
	Close();


	// check window
	if(!Window) TVPThrowExceptionMessage(TVPWindowAlreadyMissing);

	// open target storage
	ttstr name(_name);
	ttstr param;

	const tjs_char * param_pos;
	int param_pos_ind;
	param_pos = TJS_strchr(name.c_str(), TJS_W('?'));
	param_pos_ind = (int)(param_pos - name.c_str());
	if(param_pos != NULL)
	{
		param = param_pos;
		name = ttstr(name, param_pos_ind);
	}

	IStream *istream = NULL;
	long size;
	bool flash;
	ttstr ext = TVPExtractStorageExt(name).c_str();
	ext.ToLowerCase();

	tTVPVideoModule *mod = NULL;
	if(ext == TJS_W(".swf"))
	{
		// shockwave flash movie
		flash = true;

		// load krflash.dll
		mod = TVPGetFlashVideoModule();

		// prepare local storage
		if(LocalTempStorageHolder)
			delete LocalTempStorageHolder, LocalTempStorageHolder = NULL;

		// find local name
		ttstr placed = TVPSearchPlacedPath(name);

		// open and hold
		LocalTempStorageHolder =
			new tTVPLocalTempStorageHolder(placed);
	}
	else
	{
		flash = false;

		// load krmovie.dll
		mod = TVPGetMovieVideoModule();

		// prepate IStream
		iTJSBinaryStream *stream0 = NULL;
		try
		{
			stream0 = TVPCreateStream(name);
			size = (long)stream0->GetSize();
		}
		catch(...)
		{
			if(stream0) delete stream0;
			throw;
		}

		istream = new tTVPIStreamAdapter(stream0);
	}

	// 'istream' is an IStream instance at this point

	// create video overlay object
	try
	{
		if(flash)
		{
			mod->GetVideoOverlayObject(EventQueue.GetOwner(),
				NULL, (const wchar_t*)(LocalTempStorageHolder->GetLocalName() + param).c_str(),
				(const wchar_t*)ext.c_str(), 0, &VideoOverlay);
		}
		else
		{
			// overlay 系。DrawDevice が presenter host を公開していれば、レイヤと同じ
			// buffer 出力オブジェクト (GetVideoLayerObject) を作り、本体 D3D11 バック
			// バッファへ presenter 経由で全画面合成する (UsePresenter)。host が無ければ
			// 従来の子ウィンドウ present (GetVideoOverlayObject) にフォールバックする。
			bool isOverlay = ( Mode != vomLayer );
			if( isOverlay )
				PresenterHost = QueryPresenterHost();

			// HW 経路 (IMFMediaEngine): MF-native 形式 (mp4/wmv/asf/…) で host 有り + 既定有効
			// (-mediaengine=no で無効化) + engine device 取得可 の overlay。webm/mpg は MF
			// デコーダが無いので対象外 (CPU 経路)。
			// ★vomMixer 指定時は HW を使わず CPU presenter へ。HW 経路は mixer 追加画像を
			//   描画しない (動画側オブジェクトが presenter を持つため) 一方、CPU presenter は
			//   mixer を確実に合成できる。よって「mixer が要る」明示指定 = vomMixer は CPU 固定。
			if( isOverlay && Mode != vomMixer && PresenterHost && TVPUseMediaEngine() && TVPIsMediaEngineFormat(ext) )
			{
				void* dev = QueryD3D11Device();
				if( dev )
				{
					iTVPVideoPresenter* hwpres = NULL;
					GetMediaEngineVideoObject( EventQueue.GetOwner(), istream,
						(const wchar_t*)name.c_str(), (const wchar_t*)ext.c_str(),
						size, dev, &VideoOverlay, &hwpres );
					if( VideoOverlay && hwpres )
					{
						HWMode = true;
						UsePresenter = true;
						ActivePresenter = hwpres;
					}
					else if( VideoOverlay )
					{
						VideoOverlay->Release(); VideoOverlay = NULL;
					}
				}
			}

			// HW 不使用/失敗 → CPU 経路 (presenter buffer 出力 or 子ウィンドウ present)。
			if( !VideoOverlay )
			{
				UsePresenter = isOverlay && ( PresenterHost != NULL );
				ActivePresenter = static_cast<iTVPVideoPresenter*>(this);
				if( Mode == vomLayer || UsePresenter )
					mod->GetVideoLayerObject(EventQueue.GetOwner(),
						istream, (const wchar_t*)name.c_str(), (const wchar_t*)ext.c_str(),
						size, &VideoOverlay);
				else
					mod->GetVideoOverlayObject(EventQueue.GetOwner(),
						istream, (const wchar_t*)name.c_str(), (const wchar_t*)ext.c_str(),
						size, &VideoOverlay);
			}
		}

		if( ( ( Mode == vomLayer ) || UsePresenter ) && !HWMode )
		{	// buffer 出力: レイヤ / presenter バッファへ動画フレームを書く (HW は自前テクスチャ)
			long	width, height;
			long	bufsize;
			VideoOverlay->GetVideoSize( &width, &height );

			if( width <= 0 || height <= 0 )
				TVPThrowExceptionMessage(TVPErrorInKrMovieDLL, (const tjs_char*)TVPInvalidVideoSize);

			bufsize = width * height * 4;
			if( Bitmap[0] != NULL )
				delete Bitmap[0];
			if( Bitmap[1] != NULL )
				delete Bitmap[1];
			Bitmap[0] = new tTVPBaseBitmap( width, height, 32 );
			Bitmap[1] = new tTVPBaseBitmap( width, height, 32 );

			BmpBits[0] = static_cast<BYTE*>(Bitmap[0]->GetBitmap()->GetScanLine( Bitmap[0]->GetBitmap()->GetHeight()-1 ));
			BmpBits[1] = static_cast<BYTE*>(Bitmap[1]->GetBitmap()->GetScanLine( Bitmap[1]->GetBitmap()->GetHeight()-1 ));

			VideoOverlay->SetVideoBuffer( BmpBits[0], BmpBits[1], bufsize );
		}
		else
		{	// 子ウィンドウ present (従来 overlay / flash)
			ResetOverlayParams();
		}
	}
	catch(...)
	{
		if(istream) istream->Release();
		Close();
		throw;
	}
	if(istream) istream->Release();

	// set Status
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Stop);
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Close()
{
	// close
	// presenter 経路: 登録解除してから GPU リソースを解放する
	UnregisterPresenter();
	if( VideoBlit ) { delete VideoBlit; VideoBlit = NULL; }
	if( MixerBlit ) { delete MixerBlit; MixerBlit = NULL; }
	if( MixerBitmap ) { delete MixerBitmap; MixerBitmap = NULL; }
	UsePresenter = false;
	HWMode = false;
	ActivePresenter = static_cast<iTVPVideoPresenter*>(this);
	PresenterHost = NULL;
	HasFrame = false;

	// release VideoOverlay object
	if(VideoOverlay)
	{
		VideoOverlay->Release(), VideoOverlay = NULL;
		::SetFocus(Window->GetWindowHandle());
	}
	if(LocalTempStorageHolder)
		delete LocalTempStorageHolder, LocalTempStorageHolder = NULL;
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);

	if( Bitmap[0] )
		delete Bitmap[0];
	if( Bitmap[1] )
		delete Bitmap[1];

	Bitmap[0] = Bitmap[1] = NULL;
	BmpBits[0] = BmpBits[1] = NULL;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Shutdown()
{
	// shutdown the system
	// this functions closes the overlay object, but must not fire any events.
	bool c = CanDeliverEvents;
	UnregisterPresenter(); // presenter 経路: DrawDevice からの登録を外す
	ClearWndProcMessages();
	SetStatus(tTVPVideoOverlayStatus::Unload);
	try
	{
		if(VideoOverlay) VideoOverlay->Release(), VideoOverlay = NULL;
	}
	catch(...)
	{
		CanDeliverEvents = c;
		throw;
	}
	CanDeliverEvents = c;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Disconnect()
{
	// disconnect the object
	Shutdown();

	Window = NULL;
}
//---------------------------------------------------------------------------
// Track V-E: overlay 動画 presenter 経路
//---------------------------------------------------------------------------
// overlay/mixer の矩形 (プライマリレイヤ座標 0..Src) を、DrawDevice が動画を描く
// クライアント矩形 (DestRect) の座標系へスケールする。
static tTVPRect TVPMapOverlayRectToClient(const tTVPVideoPresenterContext &ctx, const tTVPRect &r)
{
	int sw = ctx.SrcWidth  > 0 ? ctx.SrcWidth  : 1;
	int sh = ctx.SrcHeight > 0 ? ctx.SrcHeight : 1;
	double dw = (double)ctx.DestRect.get_width();
	double dh = (double)ctx.DestRect.get_height();
	tTVPRect o;
	o.left   = ctx.DestRect.left + (tjs_int)( r.left   * dw / sw + 0.5 );
	o.top    = ctx.DestRect.top  + (tjs_int)( r.top    * dh / sh + 0.5 );
	o.right  = ctx.DestRect.left + (tjs_int)( r.right  * dw / sw + 0.5 );
	o.bottom = ctx.DestRect.top  + (tjs_int)( r.bottom * dh / sh + 0.5 );
	return o;
}
//---------------------------------------------------------------------------
iTVPVideoPresenterHost * tTJSNI_VideoOverlay::QueryPresenterHost()
{
	// Window が持つ DrawDevice の TJS オブジェクトから、登録口 (iTVPVideoPresenterHost)
	// のポインタを規定プロパティ "videoPresenterHost" として取得する。プロパティが無い/
	// 0 の描画デバイス (OGL/Null/custom 等) は非対応 → 子ウィンドウ present へフォールバック。
	if( !Window ) return NULL;
	const tTJSVariant & ddobj = Window->GetDrawDeviceObject();
	if( ddobj.Type() != tvtObject ) return NULL;
	tTJSVariantClosure clo = ddobj.AsObjectClosureNoAddRef();
	if( clo.Object == NULL ) return NULL;
	tTJSVariant val;
	if( TJS_FAILED( clo.PropGet(0, TJS_W("videoPresenterHost"), NULL, &val, NULL) ) )
		return NULL;
	tjs_int64 p = (tjs_int64)val;
	if( p == 0 ) return NULL;
	return reinterpret_cast<iTVPVideoPresenterHost*>((intptr_t)p);
}
//---------------------------------------------------------------------------
void * tTJSNI_VideoOverlay::QueryD3D11Device()
{
	// HW 動画 (MediaEngine) が束ねる engine の ID3D11Device を DrawDevice の TJS プロパティ
	// "d3d11Device" から取得する (無ければ null → HW 不可)。
	if( !Window ) return NULL;
	const tTJSVariant & ddobj = Window->GetDrawDeviceObject();
	if( ddobj.Type() != tvtObject ) return NULL;
	tTJSVariantClosure clo = ddobj.AsObjectClosureNoAddRef();
	if( clo.Object == NULL ) return NULL;
	tTJSVariant val;
	if( TJS_FAILED( clo.PropGet(0, TJS_W("d3d11Device"), NULL, &val, NULL) ) )
		return NULL;
	tjs_int64 p = (tjs_int64)val;
	return reinterpret_cast<void*>((intptr_t)p);
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::RegisterPresenter()
{
	if( UsePresenter && PresenterHost && ActivePresenter && !PresenterRegistered )
	{
		PresenterHost->AddVideoPresenter( ActivePresenter );
		PresenterRegistered = true;
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::UnregisterPresenter()
{
	if( PresenterRegistered && PresenterHost && ActivePresenter )
		PresenterHost->RemoveVideoPresenter( ActivePresenter );
	PresenterRegistered = false;
}
//---------------------------------------------------------------------------
bool TJS_INTF_METHOD tTJSNI_VideoOverlay::RenderVideoFrame( const tTVPVideoPresenterContext & ctx )
{
	// DrawDevice の Show() (描画スレッド) から毎フレーム呼ばれる。最新フレームを engine の
	// D3D11 テクスチャへ上げ、ゲーム画面領域 (DestRect) 全面へ描く (動画が画面を覆う前提)。
	if( !UsePresenter || !VideoOverlay || !Visible || !HasFrame ) return false;

	BYTE *buff = NULL;
	VideoOverlay->GetFrontBuffer( &buff );
	if( !buff ) return false;
	tTVPBaseBitmap *bmp = ( buff == BmpBits[1] ) ? Bitmap[1] : Bitmap[0];
	if( !bmp ) return false;
	tTVPBitmap *raw = bmp->GetBitmap();
	if( !raw ) return false;

	int w = (int)raw->GetWidth();
	int h = (int)raw->GetHeight();
	if( w <= 0 || h <= 0 ) return false;
	// レイヤバッファはボトムアップ格納。視覚的 top 行 (ScanLine 0) と符号付きピッチを求める。
	const BYTE *top = (const BYTE*)raw->GetScanLine(0);
	int pitch = w * 4;
	if( h > 1 )
		pitch = (int)( (const BYTE*)raw->GetScanLine(1) - top );

	if( !VideoBlit ) VideoBlit = new tTVPVideoPresenterD3D();
	VideoBlit->Render( ctx, top, pitch, w, h, ctx.DestRect, (float)MovieAlpha );

	// mixer 追加画像 (旧 setMixingLayer の後継)。動画の上へアルファ合成で重ねる。
	if( MixerBitmap )
	{
		tTVPBitmap *mraw = MixerBitmap->GetBitmap();
		if( mraw )
		{
			int mw = (int)mraw->GetWidth();
			int mh = (int)mraw->GetHeight();
			if( mw > 0 && mh > 0 )
			{
				const BYTE *mtop = (const BYTE*)mraw->GetScanLine(0);
				int mpitch = mw * 4;
				if( mh > 1 )
					mpitch = (int)( (const BYTE*)mraw->GetScanLine(1) - mtop );
				tTVPRect mdst = TVPMapOverlayRectToClient( ctx, MixerRect );
				if( !MixerBlit ) MixerBlit = new tTVPVideoPresenterD3D();
				MixerBlit->Render( ctx, mtop, mpitch, mw, mh, mdst, (float)MixerAlpha );
			}
		}
	}

	return true;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Play()
{
	// start playing
	if(VideoOverlay)
	{
		VideoOverlay->Play();
		ClearWndProcMessages();
		RegisterPresenter(); // presenter 経路: 稼働開始を DrawDevice に登録
		// Track V-D で EVR 撤去。旧 vomMFEVR は EVR の非同期 state 通知に頼って
		// SetStatus を抑止していたが、統合後は overlay と同じく同期 SetStatus する。
		SetStatus(tTVPVideoOverlayStatus::Play);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Stop()
{
	// stop playing
	if(VideoOverlay)
	{
		VideoOverlay->Stop();
		ClearWndProcMessages();
		UnregisterPresenter(); // presenter 経路: 停止で登録解除 (以後 present しない)
		SetStatus(tTVPVideoOverlayStatus::Stop);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::Pause()
{
	// pause playing
	if(VideoOverlay)
	{
		VideoOverlay->Pause();
//		ClearWndProcMessages();
		SetStatus(tTVPVideoOverlayStatus::Pause);
	}
}
void tTJSNI_VideoOverlay::Rewind()
{
	// rewind playing
	if(VideoOverlay)
	{
		VideoOverlay->Rewind();
		ClearWndProcMessages();

		if( EventFrame >= 0 && IsEventPast )
			IsEventPast = false;
	}
}
void tTJSNI_VideoOverlay::Prepare()
{	// prepare movie
	if( VideoOverlay && (Mode == vomLayer) )
	{
		Pause();
		Rewind();
		IsPrepare = true;
		Play();
	}
}
void tTJSNI_VideoOverlay::SetSegmentLoop( int comeFrame, int goFrame )
{
	SegLoopStartFrame = comeFrame;
	SegLoopEndFrame = goFrame;
}
void tTJSNI_VideoOverlay::SetPeriodEvent( int eventFrame )
{
	EventFrame = eventFrame;

	if( eventFrame <= GetFrame() )
		IsEventPast = true;
	else
		IsEventPast = false;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectangleToVideoOverlay()
{
	// set Rectangle to video overlay
	if(VideoOverlay && OwnerWindow)
	{
		tjs_int ofsx, ofsy;
		Window->GetVideoOffset(ofsx, ofsy);
		tjs_int l = Rect.left;
		tjs_int t = Rect.top;
		tjs_int r = Rect.right;
		tjs_int b = Rect.bottom;
		TVPAddLog(TJS_W("Video zoom: (") + ttstr(l) + TJS_W(",") + ttstr(t) + TJS_W(")-(") +
			ttstr(r) + TJS_W(",") + ttstr(b) + TJS_W(") ->"));
		Window->ZoomRectangle(l, t, r, b);
		TVPAddLog(TJS_W("(") + ttstr(l) + TJS_W(",") + ttstr(t) + TJS_W(")-(") +
			ttstr(r) + TJS_W(",") + ttstr(b) + TJS_W(")"));
		RECT rect = {l + ofsx, t + ofsy, r + ofsx, b + ofsy};
		VideoOverlay->SetRect(&rect);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetPosition(tjs_int left, tjs_int top)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetPosition( left, top );
		if( Layer2 != NULL ) Layer2->SetPosition( left, top );
	}
	else
	{
		Rect.set_offsets(left, top);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetSize(tjs_int width, tjs_int height)
{
	if( Mode == vomLayer ) return;

	Rect.set_size(width, height);
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetBounds(const tTVPRect & rect)
{
	if( Mode == vomLayer ) return;

	Rect = rect;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetLeft(tjs_int l)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetLeft( l );
		if( Layer2 != NULL ) Layer2->SetLeft( l );
	}
	else
	{
		Rect.set_offsets(l, Rect.top);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTop(tjs_int t)
{
	if( Mode == vomLayer )
	{
		if( Layer1 != NULL ) Layer1->SetTop( t );
		if( Layer2 != NULL ) Layer2->SetTop( t );
	}
	else
	{
		Rect.set_offsets(Rect.left, t);
		SetRectangleToVideoOverlay();
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetWidth(tjs_int w)
{
	if( Mode == vomLayer ) return;

	Rect.right = Rect.left + w;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetHeight(tjs_int h)
{
	if( Mode == vomLayer ) return;

	Rect.bottom = Rect.top + h;
	SetRectangleToVideoOverlay();
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetVisible(bool b)
{
	Visible = b;
	if(VideoOverlay)
	{
		if( Mode == vomLayer )
		{
			if( Layer1 != NULL ) Layer1->SetVisible( Visible );
			if( Layer2 != NULL ) Layer2->SetVisible( Visible );
		}
		else
		{
			VideoOverlay->SetVisible(Visible);
		}
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ResetOverlayParams()
{
	// retrieve new window information from owner window and
	// set video owner window / message drain window.
	// also sets rectangle and visible state.
	if(VideoOverlay && Window && (Mode == vomOverlay || Mode == vomMixer || Mode == vomMFEVR) )
	{
		OwnerWindow = Window->GetWindowHandle();
		VideoOverlay->SetWindow(OwnerWindow);

		VideoOverlay->SetMessageDrainWindow(Window->GetSurfaceWindowHandle());

		// set Rectangle
		SetRectangleToVideoOverlay();

		// set Visible
		VideoOverlay->SetVisible(Visible);
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::DetachVideoOverlay()
{
	if(VideoOverlay && Window && (Mode == vomOverlay || Mode == vomMixer || Mode == vomMFEVR) )
	{
		VideoOverlay->SetWindow(NULL);
		VideoOverlay->SetMessageDrainWindow(EventQueue.GetOwner());
			// once set to util window
	}
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetRectOffset(tjs_int ofsx, tjs_int ofsy)
{
	if(VideoOverlay)
	{
		RECT r = {Rect.left + ofsx, Rect.top + ofsy,
			Rect.right + ofsx, Rect.bottom + ofsy};
		VideoOverlay->SetRect(&r);
	}
}
//---------------------------------------------------------------------------
//void __fastcall tTJSNI_VideoOverlay::WndProc(Messages::TMessage &Msg)
void tTJSNI_VideoOverlay::WndProc( NativeEvent& ev )
{
	// EventQueue's message procedure
	if(VideoOverlay)
	{
		switch(ev.Message) {
		case WM_GRAPHNOTIFY:
		{
			long evcode;
			LONG_PTR p1, p2;
			bool got;
			do {
				VideoOverlay->GetEvent(&evcode, &p1, &p2, &got);
				if( got == false)
					return;

				switch( evcode )
				{
					case EC_COMPLETE:
						if( Status == tTVPVideoOverlayStatus::Play )
						{
							if( Loop )
							{
								Rewind();
								FirePeriodEvent(perLoop); // fire period event by loop rewind
							}
							else
							{
								// Graph manager seems not to complete playing
								// at this point (rewinding the movie at the event
								// handler called asynchronously from SetStatusAsync
								// makes continuing playing, but the graph seems to
								// be unstable).
								// We manually stop the manager anyway.
								VideoOverlay->Stop();
								SetStatusAsync(tTVPVideoOverlayStatus::Stop); // All data has been rendered
							}
						}
						break;
					case EC_UPDATE:
						if( Mode == vomLayer && Status == tTVPVideoOverlayStatus::Play )
						{
							int		curFrame = (int)p1;
							if( Layer1 == NULL && Layer2 == NULL )	// nothing to do.
								return;

							// 2フレーム以上差があるときはGetFrame() を現在のフレームとする
							int frame = GetFrame();
							if( (frame+1) < curFrame || (frame-1) > curFrame )
								curFrame = frame;

							if( (!IsPrepare) && (SegLoopEndFrame > 0) && (frame >= SegLoopEndFrame) ) {
								SetFrame( SegLoopStartFrame > 0 ? SegLoopStartFrame : 0 );
								FirePeriodEvent(perSegLoop); // fire period event by segment loop rewind
								return; // Updateを行わない
							}

							// get video image size
							long	width, height;
							VideoOverlay->GetVideoSize( &width, &height );

							tTJSNI_BaseLayer	*l1 = Layer1;
							tTJSNI_BaseLayer	*l2 = Layer2;

							// Check layer image size
							if( l1 != NULL )
							{
								if( (long)l1->GetImageWidth() != width || (long)l1->GetImageHeight() != height )
									l1->SetImageSize( width, height );
								if( (long)l1->GetWidth() != width || (long)l1->GetHeight() != height )
									l1->SetSize( width, height );
							}
							if( l2 != NULL )
							{
								if( (long)l2->GetImageWidth() != width || (long)l2->GetImageHeight() != height )
									l2->SetImageSize( width, height );
								if( (long)l2->GetWidth() != width || (long)l2->GetHeight() != height )
									l2->SetSize( width, height );
							}
							BYTE *buff;
							VideoOverlay->GetFrontBuffer( &buff );
							if( buff == BmpBits[0] )
							{
								if( l1 ) l1->AssignMainImage( Bitmap[0] );
								if( l2 ) l2->AssignMainImage( Bitmap[0] );
							}
							else	// 0じゃなかったら、1とみなす。
							{
								if( l1 ) l1->AssignMainImage( Bitmap[1] );
								if( l2 ) l2->AssignMainImage( Bitmap[1] );
							}
							if( l1 ) l1->Update();
							if( l2 ) l2->Update();
							FireFrameUpdateEvent( curFrame );

							// ! Prepare mode ?
							if( !IsPrepare )
							{
								// Send period event ?
								if( EventFrame >= 0 && !IsEventPast && curFrame >= EventFrame )
								{
									EventFrame = -1;
									FirePeriodEvent(perPeriod); // fire period event by setPeriodEvent()
								}
							}
							else
							{	// Prepare mode
								FirePeriodEvent(perPrepare); // fire period event by prepare()
								Pause();
								Rewind();
								IsPrepare = false;
							}
						}
						else if( UsePresenter && Status == tTVPVideoOverlayStatus::Play )
						{
							// presenter 経路 (buffer 出力の overlay)。実際の描画は DrawDevice の
							// Show() から RenderVideoFrame で pull されるので、ここでは新フレーム
							// 有りを記録し、フレーム/期間/セグメントループのイベントを発火する。
							int curFrame = (int)p1;
							int frame = GetFrame();
							if( (frame+1) < curFrame || (frame-1) > curFrame )
								curFrame = frame;
							if( (!IsPrepare) && (SegLoopEndFrame > 0) && (frame >= SegLoopEndFrame) ) {
								SetFrame( SegLoopStartFrame > 0 ? SegLoopStartFrame : 0 );
								FirePeriodEvent(perSegLoop);
								return;
							}
							HasFrame = true;
							FireFrameUpdateEvent( curFrame );
							if( EventFrame >= 0 && !IsEventPast && curFrame >= EventFrame )
							{
								EventFrame = -1;
								FirePeriodEvent(perPeriod);
							}
						}
						else if( Mode == vomMixer && Status == tTVPVideoOverlayStatus::Play )
						{
							int frame = GetFrame();
							if( (!IsPrepare) && (SegLoopEndFrame > 0) && (frame >= SegLoopEndFrame) ) {
								SetFrame( SegLoopStartFrame > 0 ? SegLoopStartFrame : 0 );
								FirePeriodEvent(perSegLoop); // fire period event by segment loop rewind
								return;
							}
							VideoOverlay->PresentVideoImage();
							FireFrameUpdateEvent( frame );
							// Send period event ?
							if( EventFrame >= 0 && !IsEventPast && frame >= EventFrame )
							{
								EventFrame = -1;
								FirePeriodEvent(perPeriod); // fire period event by setPeriodEvent()
							}
						}
						break;
				}
				VideoOverlay->FreeEventParams( evcode, p1, p2 );
			} while( got );
			return;
		}
		case WM_CALLBACKCMD:
		{
			// wparam : command
			// lparam : argument
			FireCallbackCommand((tjs_char*)ev.WParam, (tjs_char*)ev.LParam);
			return;
		}
		case WM_STATE_CHANGE:
			{
				switch( ev.WParam ) {
				case vsStopped:
					SetStatusAsync( tTVPVideoOverlayStatus::Stop );
					break;
				case vsPlaying:
					SetStatusAsync( tTVPVideoOverlayStatus::Play );
					break;
				case vsPaused:
					SetStatusAsync( tTVPVideoOverlayStatus::Pause );
					break;
				case vsReady:
					SetStatusAsync( tTVPVideoOverlayStatus::Ready );
					break;
				case vsEnded:
					if( Status == tTVPVideoOverlayStatus::Play )
					{
						if( Loop )
						{
							VideoOverlay->Play();
							FirePeriodEvent(perLoop); // fire period event by loop rewind
						}
						else
						{
							VideoOverlay->Stop();
							SetStatusAsync(tTVPVideoOverlayStatus::Stop); // All data has been rendered
						}
					}
					break;
				}
				return;
			}
		}
	}

	EventQueue.HandlerDefault(ev);
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::SetTimePosition( tjs_uint64 p )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetPosition( p );
	}
}
tjs_uint64 tTJSNI_VideoOverlay::GetTimePosition()
{
	tjs_uint64	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetPosition( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SetFrame( tjs_int f )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetFrame( f );

		if( EventFrame >= f && IsEventPast )
			IsEventPast = false;
	}
}
tjs_int tTJSNI_VideoOverlay::GetFrame()
{
	tjs_int	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetFrame( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SetStopFrame( tjs_int f )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetStopFrame( f );
	}
}
void tTJSNI_VideoOverlay::SetDefaultStopFrame()
{
	if(VideoOverlay)
	{
		VideoOverlay->SetDefaultStopFrame();
	}
}
tjs_int tTJSNI_VideoOverlay::GetStopFrame()
{
	tjs_int	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetStopFrame( &result );
	}
	return result;
}
tjs_real tTJSNI_VideoOverlay::GetFPS()
{
	tjs_real	result = 0.0;
	if(VideoOverlay)
	{
		VideoOverlay->GetFPS( &result );
	}
	return result;
}
tjs_int tTJSNI_VideoOverlay::GetNumberOfFrame()
{
	tjs_int	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfFrame( &result );
	}
	return result;
}
tjs_int64 tTJSNI_VideoOverlay::GetTotalTime()
{
	tjs_int64	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetTotalTime( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SetLoop( bool b )
{
	Loop = b;
}
void tTJSNI_VideoOverlay::SetLayer1( tTJSNI_BaseLayer *l )
{
	Layer1 = l;
}
void tTJSNI_VideoOverlay::SetLayer2( tTJSNI_BaseLayer *l )
{
	Layer2 = l;
}
void tTJSNI_VideoOverlay::SetMode( tTVPVideoOverlayMode m )
{
	// ビデオオープン後のモード変更は禁止
	if( !VideoOverlay )
	{
		Mode = m;
	}
}

tjs_real tTJSNI_VideoOverlay::GetPlayRate()
{
	tjs_real	result = 0.0;
	if(VideoOverlay)
	{
		VideoOverlay->GetPlayRate( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SetPlayRate(tjs_real r)
{
	if(VideoOverlay)
	{
		VideoOverlay->SetPlayRate( r );
	}
}

tjs_int tTJSNI_VideoOverlay::GetAudioBalance()
{
	long	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetAudioBalance( &result );
	}
	return TVPDSAttenuateToPan( result );
}
void tTJSNI_VideoOverlay::SetAudioBalance(tjs_int b)
{
	if(VideoOverlay)
	{
		VideoOverlay->SetAudioBalance( TVPPanToDSAttenuate( b ) );
	}
}
tjs_int tTJSNI_VideoOverlay::GetAudioVolume()
{
	long	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetAudioVolume( &result );
	}
	return TVPDSAttenuateToVolume( result );
}
void tTJSNI_VideoOverlay::SetAudioVolume(tjs_int b)
{
	if(VideoOverlay)
	{
		VideoOverlay->SetAudioVolume( TVPVolumeToDSAttenuate( b ) );
	}
}
tjs_uint tTJSNI_VideoOverlay::GetNumberOfAudioStream()
{
	unsigned long	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfAudioStream( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SelectAudioStream(tjs_uint n)
{
	if(VideoOverlay)
	{
		VideoOverlay->SelectAudioStream( n );
	}
}
tjs_int tTJSNI_VideoOverlay::GetEnabledAudioStream()
{
	long		result = -1;
	if(VideoOverlay)
	{
		VideoOverlay->GetEnableAudioStreamNum( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::DisableAudioStream()
{
	if(VideoOverlay)
	{
		VideoOverlay->DisableAudioStream();
	}
}

tjs_uint tTJSNI_VideoOverlay::GetNumberOfVideoStream()
{
	unsigned long	result = 0;
	if(VideoOverlay)
	{
		VideoOverlay->GetNumberOfVideoStream( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SelectVideoStream(tjs_uint n)
{
	if(VideoOverlay)
	{
		VideoOverlay->SelectVideoStream( n );
	}
}
tjs_int tTJSNI_VideoOverlay::GetEnabledVideoStream()
{
	long		result = -1;
	if(VideoOverlay)
	{
		VideoOverlay->GetEnableVideoStreamNum( &result );
	}
	return result;
}
void tTJSNI_VideoOverlay::SetMixingLayer( tTJSNI_BaseLayer *l )
{
	if( UsePresenter )
	{
		// presenter 経路: レイヤ画像のスナップショット (COW) を保持し、RenderVideoFrame で
		// 動画の上へ描く。矩形はプライマリレイヤ座標で保持し描画時にクライアントへスケール。
		if( MixerBitmap ) { delete MixerBitmap; MixerBitmap = NULL; }
		if( l && l->GetVisible() )
		{
			tTVPBaseBitmap *src = l->GetMainImage();
			if( src )
			{
				MixerBitmap = new tTVPBaseBitmap( *src );
				MixerRect.left   = l->GetLeft() + l->GetImageLeft();
				MixerRect.top    = l->GetTop()  + l->GetImageTop();
				MixerRect.right  = MixerRect.left + l->GetImageWidth();
				MixerRect.bottom = MixerRect.top  + l->GetImageHeight();
				MixerAlpha = (tjs_real)l->GetOpacity() / 255.0;
			}
		}
		return;
	}
	if(VideoOverlay)
	{
		if( l )
		{
			if( l->GetVisible() )
			{
				float	alpha = static_cast<float>(l->GetOpacity()) / 255.0f;
				RECT	dest;
				dest.left = l->GetLeft() + l->GetImageLeft();
				dest.top = l->GetTop() + l->GetImageTop();
				dest.right = dest.left + l->GetImageWidth();
				dest.bottom = dest.top + l->GetImageHeight();

				// tTVPBaseBitmap->tTVPBitmap
				tTVPBitmap *bmp = l->GetMainImage()->GetBitmap();
				if( bmp )
				{
					const BitmapInfomation* bmpinfo = bmp->GetBitmapInfomation();

					// 自前でDCを作る
					HDC hdc;
					HDC			ref = GetDC(0);
					HBITMAP		myDIB = CreateDIBitmap( ref, bmpinfo->GetBITMAPINFOHEADER(), CBM_INIT, bmp->GetBits(), bmpinfo->GetBITMAPINFO(), bmp->Is8bit() ? DIB_PAL_COLORS : DIB_RGB_COLORS );
					hdc = CreateCompatibleDC( NULL );
					HGDIOBJ		hOldBmp = SelectObject( hdc, myDIB );

					VideoOverlay->SetMixingBitmap( hdc, &dest, alpha );

					SelectObject( hdc, hOldBmp );
					DeleteObject( myDIB );
					DeleteDC( hdc );
				}
			}
			else
			{
				VideoOverlay->ResetMixingBitmap();
			}
		}
		else
		{
			VideoOverlay->ResetMixingBitmap();
		}
	}
}
void tTJSNI_VideoOverlay::ResetMixingBitmap()
{
	if( UsePresenter )
	{
		if( MixerBitmap ) { delete MixerBitmap; MixerBitmap = NULL; }
		return;
	}
	if(VideoOverlay)
	{
		VideoOverlay->ResetMixingBitmap();
	}
}
void tTJSNI_VideoOverlay::SetMixingMovieAlpha( tjs_real a )
{
	MovieAlpha = a; // presenter 経路の overlay 全体アルファ
	if(VideoOverlay)
	{
		VideoOverlay->SetMixingMovieAlpha( static_cast<float>(a) );
	}
}
tjs_real tTJSNI_VideoOverlay::GetMixingMovieAlpha()
{
	float	ret = 0.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetMixingMovieAlpha( &ret );
	}
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetMixingMovieBGColor( tjs_uint col )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetMixingMovieBGColor( col );
	}
}
tjs_uint tTJSNI_VideoOverlay::GetMixingMovieBGColor()
{
	unsigned long	ret;
	if(VideoOverlay)
	{
		VideoOverlay->GetMixingMovieBGColor( &ret );
	}
	return static_cast<tjs_uint>(ret);
}



tjs_real tTJSNI_VideoOverlay::GetContrastRangeMin()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastRangeMin( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastRangeMax()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastRangeMax( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastDefaultValue()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastDefaultValue( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrastStepSize()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetContrastStepSize( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetContrast()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetContrast( &ret );
	}
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetContrast( tjs_real v )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetContrast( static_cast<float>(v) );
	}
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMin()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessRangeMin( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessRangeMax()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessRangeMax( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessDefaultValue()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessDefaultValue( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightnessStepSize()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightnessStepSize( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetBrightness()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetBrightness( &ret );
	}
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetBrightness( tjs_real v )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetBrightness( static_cast<float>(v) );
	}
}

tjs_real tTJSNI_VideoOverlay::GetHueRangeMin()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetHueRangeMin( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueRangeMax()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetHueRangeMax( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueDefaultValue()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetHueDefaultValue( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHueStepSize()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetHueStepSize( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetHue()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetHue( &ret );
	}
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetHue( tjs_real v )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetHue( static_cast<float>(v) );
	}
}

tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMin()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationRangeMin( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationRangeMax()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationRangeMax( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationDefaultValue()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationDefaultValue( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturationStepSize()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturationStepSize( &ret );
	}
	return static_cast<tjs_real>(ret);
}
tjs_real tTJSNI_VideoOverlay::GetSaturation()
{
	float ret = -1.0f;
	if(VideoOverlay)
	{
		VideoOverlay->GetSaturation( &ret );
	}
	return static_cast<tjs_real>(ret);
}
void tTJSNI_VideoOverlay::SetSaturation( tjs_real v )
{
	if(VideoOverlay)
	{
		VideoOverlay->SetSaturation( static_cast<float>(v) );
	}
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalWidth()
{
	// retrieve original (coded in the video stream) width size
	if(!VideoOverlay) return 0;

	long	width, height;
	VideoOverlay->GetVideoSize( &width, &height );

	return (tjs_int)width;
}
//---------------------------------------------------------------------------
tjs_int tTJSNI_VideoOverlay::GetOriginalHeight()
{
	// retrieve original (coded in the video stream) height size

	long	width, height;
	VideoOverlay->GetVideoSize( &width, &height );

	return (tjs_int)height;
}
//---------------------------------------------------------------------------
void tTJSNI_VideoOverlay::ClearWndProcMessages()
{
	// clear WndProc's message queue
	MSG msg;
	while(PeekMessage(&msg, EventQueue.GetOwner(), WM_GRAPHNOTIFY, WM_GRAPHNOTIFY+2, PM_REMOVE))
	{
		if(VideoOverlay)
		{
			long evcode;
			LONG_PTR p1, p2;
			bool got;
			VideoOverlay->GetEvent(&evcode, &p1, &p2, &got); // dummy call
			if( got )
				VideoOverlay->FreeEventParams( evcode, p1, p2 );
		}
	}
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
// tTJSNC_VideoOverlay::CreateNativeInstance : returns proper instance object
//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_VideoOverlay::CreateNativeInstance()
{
	return new tTJSNI_VideoOverlay();
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
// TVPCreateNativeClass_VideoOverlay
//---------------------------------------------------------------------------
tTJSNativeClass * TVPCreateNativeClass_VideoOverlay()
{
	return new tTJSNC_VideoOverlay();
}
//---------------------------------------------------------------------------

