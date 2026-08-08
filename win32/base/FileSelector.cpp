//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// File Selector dialog box
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <cderr.h>
#include <commdlg.h>
#include <shobjidl.h>   // IFileOpenDialog (selectDirectory)
#include <shlobj.h>     // SHCreateItemFromParsingName
#include <thread>       // selectDirectory を STA スレッドで実行
#include <string>

#include "MsgIntf.h"
#include "StorageImpl.h"
#include "WindowImpl.h"
#include "SysInitIntf.h"
#include "DebugIntf.h"
#include "StorageIntf.h"  // TVPNormalizeStorageName / TVPGetLocalName
#include "FileSelector.h"

#include "TVPScreen.h"


//---------------------------------------------------------------------------
// TVPSelectFile related
//---------------------------------------------------------------------------
#define TVP_OLD_OFN_STRUCT_SIZE 76
//---------------------------------------------------------------------------
static tjs_int TVPLastScreenWidth = 0;
static tjs_int TVPLastScreenHeight = 0;
static tjs_int TVPLastOFNLeft = -30000;
static tjs_int TVPLastOFNTop = -30000;
static UINT_PTR APIENTRY TVPOFNHookProc(HWND hdlg, UINT uiMsg, WPARAM wParam,
	LPARAM lParam)
{
	if(uiMsg == WM_INITDIALOG)
	{
		int left, top;
		HWND parent = GetParent(hdlg);
		if((TVPLastOFNLeft == -30000 && TVPLastOFNTop == -30000) ||
			TVPLastScreenWidth != tTVPScreen::GetWidth() || TVPLastScreenHeight != tTVPScreen::GetHeight() )
		{
			// center the window
			RECT rect;
			GetWindowRect(parent, &rect);
			left = ((tTVPScreen::GetWidth() - rect.right + rect.left) / 2);
			top = ((tTVPScreen::GetHeight() - rect.bottom + rect.top) / 3);
		}
		else
		{
			// set last position
			left = TVPLastOFNLeft;
			top = TVPLastOFNTop;
		}

		TVPLastScreenWidth = tTVPScreen::GetWidth();
		TVPLastScreenHeight = tTVPScreen::GetHeight();

		SetWindowPos(parent, 0,
			left,
			top,
			0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
	}
	else if(uiMsg == WM_DESTROY ||
		(uiMsg == WM_NOTIFY && ((OFNOTIFY*)lParam)->hdr.code == CDN_FILEOK))
	{
		HWND parent = GetParent(hdlg);
		RECT rect;
		GetWindowRect(parent, &rect);
		TVPLastOFNLeft = rect.left;
		TVPLastOFNTop = rect.top;
	}
	return 0;
}
//---------------------------------------------------------------------------
static void TVPPushFilterPair(std::vector<tjs_string> &filters, tjs_string filter)
{
	tjs_string::size_type vpos = filter.find_first_of(TJS_W("|"));
	if( vpos != tjs_string::npos )
	{
		tjs_string name = filter.substr(0, vpos);
		tjs_string wild = filter.c_str() + vpos+1;
		filters.push_back(name);
		filters.push_back(wild);
	}
	else
	{
		filters.push_back(filter);
		filters.push_back(filter);
	}
}
//---------------------------------------------------------------------------
bool TVPSelectFile(iTJSDispatch2 *params)
{
	// show open dialog box
	// NOTE: currently this only shows ANSI version of file open dialog.
	tTJSVariant val;
	tjs_char* filter = NULL;
	tjs_char* filename = NULL;
	tjs_string initialdir;
	tjs_string title;
	tjs_string defaultext;
	BOOL result;

	try
	{
		// prepare OPENFILENAME structure

		OPENFILENAME ofn;
		memset(&ofn, 0, sizeof(ofn));
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = TVPGetModalWindowOwnerHandle();
		if( ofn.hwndOwner == INVALID_HANDLE_VALUE ) {
			ofn.hwndOwner = NULL;
		}
		ofn.hInstance = NULL;

		// set application window position to current window position
		

		// get filter
		ofn.lpstrFilter = NULL;

		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("filter"), 0,
			&val, params)))
		{
			std::vector<tjs_string> filterlist;
			if(val.Type() != tvtObject)
			{
				TVPPushFilterPair(filterlist, ttstr(val).AsStdString());
			}
			else
			{
				iTJSDispatch2 * array = val.AsObjectNoAddRef();
				tjs_int count;
				tTJSVariant tmp;
				if(TJS_SUCCEEDED(array->PropGet(TJS_MEMBERMUSTEXIST,
					TJS_W("count"), 0, &tmp, array)))
					count = tmp;
				else
					count = 0;

				for(tjs_int i = 0; i < count; i++)
				{
					if(TJS_SUCCEEDED(array->PropGetByNum(TJS_MEMBERMUSTEXIST,
						i, &tmp, array)))
					{
						TVPPushFilterPair(filterlist, ttstr(tmp).AsStdString());
					}
				}
			}

			// create filter buffer
			tjs_int bufsize = 2;
			for(std::vector<tjs_string>::iterator i = filterlist.begin(); i != filterlist.end(); i++)
			{
				bufsize += (tjs_int)(i->length() + 1);
			}

			filter = new tjs_char[bufsize];

			tjs_char* p = filter;
			for(std::vector<tjs_string>::iterator i = filterlist.begin(); i != filterlist.end(); i++)
			{
				TJS_strcpy(p, i->c_str());
				p += i->length() + 1;
			}
			*(p++) = 0;
			*(p++) = 0;

			ofn.lpstrFilter = (const wchar_t*)filter;
		}

		ofn.lpstrCustomFilter = NULL;
		ofn.nMaxCustFilter = 0;

		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("filterIndex"), 0, &val, params)))
			ofn.nFilterIndex = (tjs_int)val;
		else
			ofn.nFilterIndex = 0;

		// filenames
		filename = new tjs_char[MAX_PATH + 1];
 		filename[0] = 0;

		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("name"), 0, &val, params)))
		{
			ttstr lname(val);
			if(!lname.IsEmpty())
			{
				lname = TVPNormalizeStorageName(lname);
				TVPGetLocalName(lname);
				tjs_string name = lname.AsStdString();
				TJS_strncpy(filename, name.c_str(), MAX_PATH);
				filename[MAX_PATH] = 0;
			}
		}

		ofn.lpstrFile = (wchar_t*)filename;
		ofn.nMaxFile = MAX_PATH + 1;
		ofn.lpstrFileTitle = NULL;
		ofn.nMaxFileTitle = 0;

		// initial dir
		ofn.lpstrInitialDir = NULL;
		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("initialDir"), 0, &val, params)))
		{
			ttstr lname(val);
			if(!lname.IsEmpty())
			{
				lname = TVPNormalizeStorageName(lname);
				TVPGetLocalName(lname);
				initialdir = lname.AsStdString();
				ofn.lpstrInitialDir = (const wchar_t*)initialdir.c_str();
			}
		}
	
		// title
		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("title"), 0, &val, params)))
		{
			title = ttstr(val).AsStdString();
			ofn.lpstrTitle = (const wchar_t*)title.c_str();
		}
		else
		{
			ofn.lpstrTitle = NULL;
		}

		// flags
		bool issave = false;
		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("save"), 0, &val, params)))
			issave = val.operator bool();

		ofn.Flags = OFN_ENABLEHOOK|OFN_EXPLORER|OFN_NOCHANGEDIR|
			OFN_PATHMUSTEXIST|OFN_HIDEREADONLY|OFN_ENABLESIZING;


		if(!issave)
			ofn.Flags |= OFN_FILEMUSTEXIST;
		else
			ofn.Flags |= OFN_OVERWRITEPROMPT;

		// default extension
		if(TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("defaultExt"), 0, &val, params)))
		{
			defaultext = ttstr(val).AsStdString();
			ofn.lpstrDefExt = (const wchar_t*)defaultext.c_str();
		}
		else
		{
			ofn.lpstrDefExt = NULL;
		}

		// hook proc
		ofn.lpfnHook = TVPOFNHookProc;

		// show dialog box
		if(!issave)
			result = GetOpenFileName(&ofn);
		else
			result = GetSaveFileName(&ofn);


		if(!result && CommDlgExtendedError() == CDERR_STRUCTSIZE)
		{
			// for old windows
			// set lStructSize to old Windows' structure size
			ofn.lStructSize = TVP_OLD_OFN_STRUCT_SIZE;
			if(!issave)
				result = GetOpenFileName(&ofn);
			else
				result = GetSaveFileName(&ofn);
		}

		if(result)
		{
			// returns some informations

			// filter index
			val = (tjs_int)ofn.nFilterIndex;
			params->PropSet(TJS_MEMBERENSURE, TJS_W("filterIndex"), 0, &val, params);

			// file name
			val = TVPNormalizeStorageName(ttstr(filename));
			params->PropSet(TJS_MEMBERENSURE, TJS_W("name"), 0, &val, params);
		}

	}
	catch(...)
	{
		if(filter) delete [] filter;
		if(filename) delete [] filename;
		throw;
	}

	delete [] filter;
	delete [] filename;

	return 0!=result;
}
//---------------------------------------------------------------------------
// TVPSelectDirectory : フォルダ選択モーダルダイアログ (Storages.selectDirectory)
//   params は %[ name, title, window, rootDir ] の辞書。選択されると params["name"]
//   に正規化パスを書き戻し true を返す。キャンセル時は false。
//   モダンな IFileOpenDialog (FOS_PICKFOLDERS) を使用。
//---------------------------------------------------------------------------
bool TVPSelectDirectory(iTJSDispatch2 *params)
{
	tTJSVariant val;

	// タイトル
	std::wstring wtitle;
	if( TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("title"), 0, &val, params))
		&& val.Type() != tvtVoid )
		wtitle = (const wchar_t*)((ttstr)val).c_str();

	// 初期フォルダ (name 優先、無ければ rootDir)
	std::wstring winitial;
	if( TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("name"), 0, &val, params))
		&& val.Type() == tvtString && !val.NormalCompare(ttstr(TJS_W(""))) )
	{
		ttstr n = TVPNormalizeStorageName(val.AsStringNoAddRef());
		TVPGetLocalName(n);
		winitial = (const wchar_t*)n.c_str();
	}
	else if( TJS_SUCCEEDED(params->PropGet(TJS_MEMBERMUSTEXIST, TJS_W("rootDir"), 0, &val, params))
		&& val.Type() == tvtString && !val.NormalCompare(ttstr(TJS_W(""))) )
	{
		ttstr n = TVPNormalizeStorageName(val.AsStringNoAddRef());
		TVPGetLocalName(n);
		winitial = (const wchar_t*)n.c_str();
	}

	std::wstring resultPath;
	bool selected = false;

	// IFileOpenDialog::Show は STA を要求する。エンジンのメインスレッドは WIC / D3D 等
	// により MTA になっている場合があり、MTA スレッドで Show するとクロスアパートメント
	// でハングする。確実に動かすため専用の STA スレッドでダイアログを実行し join で待つ。
	// (owner は別スレッドになりデッドロック要因になるため Show には渡さない。main は
	//  join でブロック中なので実質モーダルになる)
	std::thread worker([&]() {
		HRESULT hrco = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		bool couninit = SUCCEEDED(hrco);

		IFileOpenDialog *dlg = NULL;
		if( SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&dlg))) && dlg )
		{
			DWORD opts = 0;
			dlg->GetOptions(&opts);
			dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

			if( !wtitle.empty() )
				dlg->SetTitle(wtitle.c_str());

			if( !winitial.empty() )
			{
				IShellItem *psi = NULL;
				if( SUCCEEDED(::SHCreateItemFromParsingName(winitial.c_str(),
					NULL, IID_PPV_ARGS(&psi))) && psi )
				{
					dlg->SetFolder(psi);
					psi->Release();
				}
			}

			if( SUCCEEDED(dlg->Show(NULL)) )
			{
				IShellItem *item = NULL;
				if( SUCCEEDED(dlg->GetResult(&item)) && item )
				{
					PWSTR pszPath = NULL;
					if( SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath )
					{
						resultPath = pszPath;
						selected = true;
						::CoTaskMemFree(pszPath);
					}
					item->Release();
				}
			}
			dlg->Release();
		}

		if( couninit ) ::CoUninitialize();
	});
	worker.join();

	if( selected )
	{
		ttstr path((const tjs_char*)resultPath.c_str());
		path = TVPNormalizeStorageName(path);
		tTJSVariant nv(path);
		params->PropSet(TJS_MEMBERENSURE, TJS_W("name"), NULL, &nv, params);
	}
	return selected;
}
//---------------------------------------------------------------------------






