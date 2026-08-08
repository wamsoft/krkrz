//---------------------------------------------------------------------------
// System.inputString 用 テキスト入力モーダルダイアログ (WINVER)
//   .rc リソースを持たずに、メモリ上の DLGTEMPLATE を組み立てて
//   DialogBoxIndirectParam で表示する (自己完結)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include <windows.h>
#include <vector>
#include <string>

#include "InputDialog.h"
#include "WindowImpl.h"   // TVPGetModalWindowOwnerHandle

//---------------------------------------------------------------------------
namespace {

#define IDC_INPUT_EDIT 1000

struct tInputDlgData {
	const wchar_t *def;
	std::wstring   result;
};

// --- DLGTEMPLATE メモリ組み立て補助 (WORD/DWORD/WSTR, DWORD アライン) ---
void PushW(std::vector<BYTE>& b, WORD w) { b.push_back((BYTE)(w & 0xff)); b.push_back((BYTE)((w >> 8) & 0xff)); }
void PushDW(std::vector<BYTE>& b, DWORD d) { PushW(b, (WORD)(d & 0xffff)); PushW(b, (WORD)((d >> 16) & 0xffff)); }
void PushWStr(std::vector<BYTE>& b, const wchar_t* s) { if(s){ for(; *s; ++s) PushW(b, (WORD)*s);} PushW(b, 0); }
void AlignDword(std::vector<BYTE>& b) { while (b.size() & 3) b.push_back(0); }

// 1 コントロール分の DLGITEMTEMPLATE を追加
void PushItem(std::vector<BYTE>& b, DWORD style, short x, short y, short cx, short cy,
	WORD id, WORD atom, const wchar_t* title)
{
	AlignDword(b);
	PushDW(b, style);
	PushDW(b, 0);          // exStyle
	PushW(b, (WORD)x); PushW(b, (WORD)y); PushW(b, (WORD)cx); PushW(b, (WORD)cy);
	PushW(b, id);
	PushW(b, 0xFFFF); PushW(b, atom);   // class (atom)
	PushWStr(b, title);                 // title
	PushW(b, 0);                        // creation data (none)
}

INT_PTR CALLBACK InputDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG: {
		tInputDlgData* d = (tInputDlgData*)lp;
		SetWindowLongPtr(hdlg, GWLP_USERDATA, (LONG_PTR)d);
		HWND edit = GetDlgItem(hdlg, IDC_INPUT_EDIT);
		if (edit) {
			// 初期値を設定し全選択・フォーカス
			::SetFocus(edit);
			::SendMessageW(edit, EM_SETSEL, 0, -1);
		}
		return FALSE; // 自前でフォーカス設定したので FALSE
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK: {
			tInputDlgData* d = (tInputDlgData*)GetWindowLongPtr(hdlg, GWLP_USERDATA);
			HWND edit = GetDlgItem(hdlg, IDC_INPUT_EDIT);
			if (d && edit) {
				int len = ::GetWindowTextLengthW(edit);
				std::wstring buf((size_t)len + 1, L'\0');
				::GetWindowTextW(edit, &buf[0], len + 1);
				buf.resize((size_t)len);
				d->result = buf;
			}
			::EndDialog(hdlg, 1);
			return TRUE;
		}
		case IDCANCEL:
			::EndDialog(hdlg, 0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

} // anonymous

//---------------------------------------------------------------------------
bool TVPInputString(const ttstr &caption, const ttstr &prompt,
	const ttstr &def, ttstr &result)
{
	HWND owner = TVPGetModalWindowOwnerHandle();
	if (owner == INVALID_HANDLE_VALUE) owner = NULL;

	const wchar_t* wcaption = (const wchar_t*)caption.c_str();
	const wchar_t* wprompt  = (const wchar_t*)prompt.c_str();
	const wchar_t* wdef     = (const wchar_t*)def.c_str();

	// --- DLGTEMPLATE 本体 ---
	std::vector<BYTE> t;
	DWORD style = DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
	PushDW(t, style);
	PushDW(t, 0);        // exStyle
	PushW(t, 4);         // cdit (item count)
	PushW(t, 0); PushW(t, 0);        // x, y
	PushW(t, 260); PushW(t, 74);     // cx, cy (dialog units)
	PushW(t, 0);         // menu (none)
	PushW(t, 0);         // class (default)
	PushWStr(t, wcaption); // title
	// DS_SETFONT: point size + face
	PushW(t, 9);
	PushWStr(t, L"MS Shell Dlg");

	// items
	// 1. STATIC (prompt)
	PushItem(t, WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 7, 246, 24, (WORD)-1, 0x0082, wprompt);
	// 2. EDIT (input, 初期値=def)
	PushItem(t, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
		7, 34, 246, 14, IDC_INPUT_EDIT, 0x0081, wdef);
	// 3. OK
	PushItem(t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
		142, 54, 52, 14, IDOK, 0x0080, L"OK");
	// 4. Cancel
	PushItem(t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		200, 54, 52, 14, IDCANCEL, 0x0080, L"\x30ad\x30e3\x30f3\x30bb\x30eb"); // "キャンセル"

	tInputDlgData data;
	data.def = wdef;

	INT_PTR r = ::DialogBoxIndirectParamW(GetModuleHandle(NULL),
		(LPCDLGTEMPLATEW)t.data(), owner, (DLGPROC)InputDlgProc, (LPARAM)&data);

	if (r == 1) {
		result = ttstr((const tjs_char*)data.result.c_str());
		return true;
	}
	return false; // キャンセル / エラー
}
//---------------------------------------------------------------------------
