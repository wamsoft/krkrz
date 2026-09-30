

#ifndef __IME_CONTROL_H__
#define __IME_CONTROL_H__

#include <imm.h>
#include "TVPSysFont.h"

class ImeControl {
public:
	enum {
		ModeDisable,
		ModeClose,
		ModeOpen,
		ModeDontCare,
		ModeSAlpha,
		ModeAlpha,
		ModeHira,
		ModeSKata,
		ModeKata,
		ModeChinese,
		ModeSHanguel,
		ModeHanguel,
	};

private:
	HWND hWnd_;
	HIMC hOldImc_;
	bool default_open_;
	int mode_;

public:
	ImeControl( HWND hWnd ) {
		hWnd_ = hWnd;
		hOldImc_ = INVALID_HANDLE_VALUE;
		mode_ = ModeDontCare;
		default_open_ = IsOpen();
	}
	~ImeControl() {
	}
	bool IsOpen() {
		if( hOldImc_ != INVALID_HANDLE_VALUE ) return false;

		HIMC hImc = ::ImmGetContext(hWnd_);
		BOOL result = ::ImmGetOpenStatus(hImc);
		::ImmReleaseContext(hWnd_,hImc);
		return 0!=result;
	}
	void Open() {
		Enable();
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmSetOpenStatus(hImc,TRUE);
		::ImmReleaseContext(hWnd_,hImc);
	}
	void Close() {
		Enable();
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmSetOpenStatus(hImc,FALSE);
		::ImmReleaseContext(hWnd_,hImc);
	}
	bool IsEnableThisLocale() {
		HKL hKl = ::GetKeyboardLayout(0);
		return 0!=::ImmIsIME(hKl);
	}
	/**
	 ウィンドウに入力コンテキストが結び付いているか。
	 切り離されていると ImmSetOpenStatus は成功したように見えて何も起きず、
	 ユーザの 半角/全角 キーも効かない (IME が完全に無効な状態)。
	 windowEx の Window.resetImeContext(false) 等、 本体外から切られうる。
	 */
	bool HasContext() {
		HIMC hImc = ::ImmGetContext(hWnd_);
		if( hImc == 0 ) return false;
		::ImmReleaseContext(hWnd_,hImc);
		return true;
	}
	/**
	 既定の入力コンテキストを結び直す。
	 @return 元々切り離されていて実際に結び直したなら真 (巻き戻しの要否)
	 */
	bool AttachDefaultContext() {
		if( HasContext() ) return false;
		::ImmAssociateContextEx( hWnd_, NULL, IACE_DEFAULT );
		return true;
	}
	/** AttachDefaultContext() の巻き戻し (入力コンテキストを切り離す) */
	void DetachContext() {
		::ImmAssociateContextEx( hWnd_, NULL, IACE_IGNORENOCONTEXT );
	}
	/** 診断用: この ImeControl 自身が Disable() で切っているか */
	bool IsDisabledBySelf() const { return hOldImc_ != INVALID_HANDLE_VALUE; }
	/** 診断用: 現在の変換モード。 コンテキストが無ければ偽を返し 0 を入れる */
	bool GetConversion( tjs_uint32 &conversion, tjs_uint32 &sentence ) {
		conversion = sentence = 0;
		HIMC hImc = ::ImmGetContext(hWnd_);
		if( hImc == 0 ) return false;
		DWORD c = 0, s = 0;
		BOOL r = ::ImmGetConversionStatus( hImc, &c, &s );
		::ImmReleaseContext(hWnd_,hImc);
		conversion = (tjs_uint32)c;
		sentence = (tjs_uint32)s;
		return r != 0;
	}
	// ImmSetStatusWindowPos 関数を呼び出すと、アプリケーションに IMN_SETSTATUSWINDOWPOS メッセージが送信されます。
	void SetStatusPosition( int x, int y ) {
		POINT pt = {x,y};
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmSetStatusWindowPos( hImc, &pt );
		::ImmReleaseContext(hWnd_,hImc);
	}
	void GetStatusPosition( int &x, int &y ) {
		POINT pt = {0,0};
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmGetStatusWindowPos( hImc, &pt );
		::ImmReleaseContext(hWnd_,hImc);
		x = pt.x;
		y = pt.y;
	}
	/**
	 フォーカスを失うときなどに、 自分が外した入力コンテキストを戻す。

	 ⚠ 以前は Open() を呼んでいた (= Enable() + ImmSetOpenStatus(TRUE))。
	    「戻す」に IME を開くことまでは含まれないうえ、 AcquireImeControl() の
	    先頭からも呼ばれるので、 ModeDisable を実際に使うとフォーカスが動く
	    たびに IME が開いてしまう。 ModeDisable は長らく未使用 (imDisable が
	    ModeClose に潰されていた) で表に出ていなかった。
	 */
	void Reset() {
		if( mode_ == ModeDisable ) {
			Enable();
			mode_ = ModeDontCare;
		}
	}
	/**
	 このスレッドのIMEを無効にする
	 */
	void Disable() {
		if( hOldImc_ == INVALID_HANDLE_VALUE ) {
			hOldImc_ = ::ImmAssociateContext(hWnd_,0);
		}
	}
	void Enable() {
		if( hOldImc_ != INVALID_HANDLE_VALUE ) {
			::ImmAssociateContext(hWnd_,hOldImc_);
			hOldImc_ = INVALID_HANDLE_VALUE;
		}
	}
	// この関数を呼び出すと、アプリケーションに IMN_SETCOMPOSITIONFONT メッセージが送信されます。
	void SetCompositionFont( tTVPSysFont* font ) {
		LOGFONT logfont={0};
		font->GetFont(&logfont);
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmSetCompositionFont( hImc, &logfont );
		::ImmReleaseContext(hWnd_,hImc);
	}
	/**
	 変換ウィンドウ (未確定文字列) と変換候補ウィンドウを入力欄へ寄せる。
	 rcArea = テキスト領域、 ptCurrentPos = キャレット位置。 候補窓は CFS_EXCLUDE
	 でテキスト領域を避ける (入力中の文字を隠さない)。 SDL3 の
	 IME_SetTextInputArea と同じ考え方で、 変種間で見え方を揃えている。
	 座標はウィンドウクライアント座標 (px)。
	 @param cursor テキスト領域左端からのキャレットの相対 x
	 */
	void SetTextInputArea( int x, int y, int w, int h, int cursor ) {
		HIMC hImc = ::ImmGetContext(hWnd_);
		if( hImc == 0 ) return;
		COMPOSITIONFORM cof = {};
		cof.dwStyle = CFS_RECT;
		cof.ptCurrentPos.x = x + cursor;
		cof.ptCurrentPos.y = y;
		cof.rcArea.left = x;
		cof.rcArea.top = y;
		cof.rcArea.right = x + w;
		cof.rcArea.bottom = y + h;
		::ImmSetCompositionWindow( hImc, &cof );

		CANDIDATEFORM caf = {};
		caf.dwIndex = 0;
		caf.dwStyle = CFS_EXCLUDE;
		caf.ptCurrentPos.x = x + cursor;
		caf.ptCurrentPos.y = y;
		caf.rcArea.left = x;
		caf.rcArea.top = y;
		caf.rcArea.right = x + w;
		caf.rcArea.bottom = y + h;
		::ImmSetCandidateWindow( hImc, &caf );

		::ImmReleaseContext(hWnd_,hImc);
	}
	void SetCompositionWindow( int x, int y ) {
		COMPOSITIONFORM pos;
		pos.dwStyle = CFS_POINT;
		pos.ptCurrentPos.x = x;
		pos.ptCurrentPos.y = y;
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmSetCompositionWindow( hImc, &pos );
		::ImmReleaseContext(hWnd_,hImc);
	}
	void GetCompositionWindow( int &x, int &y ) {
		COMPOSITIONFORM pos = {0};
		pos.dwStyle = CFS_POINT;
		HIMC hImc = ::ImmGetContext(hWnd_);
		::ImmGetCompositionWindow( hImc, &pos );
		::ImmReleaseContext(hWnd_,hImc);
		x = pos.ptCurrentPos.x;
		y = pos.ptCurrentPos.y;
	}
	/**
	 * conversion : 入力モードの値を指定します。
	 * 		IME_CMODE_ALPHANUMERIC(0x0000)	英数モード
	 * 		IME_CMODE_NATIVE(0x0001)		対応言語入力(ON)・英数入力(OFF) モード
	 * 		IME_CMODE_CHINESE
	 * 		IME_CMODE_HANGEUL
	 * 		IME_CMODE_JAPANESE でも定義してある
	 * 		IME_CMODE_KATAKANA(0x0002)		カタカナ(ON)・ひらがな(OFF) モード
	 * 		IME_CMODE_FULLSHAPE(0x0008)		全角モード
	 * 		IME_CMODE_ROMAN(0x0010)			ローマ字モード
	 * 		IME_CMODE_CHARCODE(0x0020)		キャラクタ入力モード
	 * 		IME_CMODE_HANJACONVERT(0x0040)	ハングル文字変換モード
	 * 		IME_CMODE_SOFTKBD(0x0080)		ソフトキーボードモード
	 * 		IME_CMODE_NOCONVERSION(0x0100)	無変換モード
	 * 		IME_CMODE_EUDC(0x0200)			EUD変換モード
	 * 		IME_CMODE_SYMBOL(0x0400)		シンボルモード
	 * sentence : 変換モードの値を指定します。
	 * 		IME_SMODE_NONE(0x0000)			無変換
	 * 		IME_SMODE_PLURALCLAUSE(0x0001)	複合語優先
	 * 		IME_SMODE_SINGLECONVERT(0x0002)	単変換
	 * 		IME_SMODE_AUTOMATIC(0x0004)		自動変換
	 * 		IME_SMODE_PHRASEPREDICT(0x0008)	連文節変換
	 */
	/*
	bool SetConversionStatus( int conversion, int sentence ) {
		return 0!=::ImmSetConversionStatus( hImc_, conversion, sentence );
	}
	*/
	/**
	 * @param mode : 
ModeDisable を指定すると、IMEは無効になります。IMEを使用した入力はできませんし、ユーザの操作でもIMEを有効にすることはできません。 : Disable
ModeClose を指定すると、IMEは無効になります。imDisableと異なり、ユーザの操作でIMEを有効にすることができます。 : Close
ModeOpen を指定すると、IMEは有効になります。 : Open
ModeDontCare を指定すると、IMEの有効/無効の状態は、前の状態を引き継ぎます。ユーザの操作によってIMEを有効にしたり無効にしたりすることができます。日本語入力においては、半角/全角文字をユーザに自由に入力させる場合の一般的なモードです。
ModeSAlpha を指定すると、IMEは有効になり、半角アルファベット入力モードになります。 : IME_CMODE_ALPHANUMERIC
ModeAlpha を指定すると、IMEは有効になり、全角アルファベット入力モードになります。 : IME_CMODE_FULLSHAPE
ModeHira を指定すると、IMEは有効になり、ひらがな入力モードになります。
ModeSKata を指定すると、IMEは有効になり、半角カタカナ入力モードになります。 : IME_CMODE_KATAKANA
ModeKata を指定すると、IMEは有効になり、全角カタカナ入力モードになります。 : IME_CMODE_KATAKANA IME_CMODE_NATIVE
ModeChinese を指定すると、IMEは有効になり、2バイト中国語入力を受け付けるモードになります。日本語環境では使用できません。 : IME_CMODE_CHINESE
ModeSHanguel を指定すると、IMEは有効になり、1バイト韓国語入力を受け付けるモードになります。日本語環境では使用できません。 : IME_CMODE_HANJACONVERT
ModeHanguel を指定すると、IMEは有効になり、2バイト韓国語入力を受け付けるモードになります。日本語環境では使用できません。 : IME_CMODE_HANGEUL
	 */
	void SetIme( int mode ) {
		mode_ = mode;
		HIMC hImc = ::ImmGetContext(hWnd_);
		DWORD conversion, sentence;
		::ImmGetConversionStatus( hImc, &conversion, &sentence );
		switch( mode ) {
		case ModeDisable:
			if( hOldImc_ == INVALID_HANDLE_VALUE ) {
				hOldImc_ = ::ImmAssociateContext(hWnd_,0);
			}
			break;
		case ModeClose:
			::ImmSetOpenStatus(hImc,FALSE);
			break;
		case ModeOpen:
			::ImmSetOpenStatus(hImc,TRUE);
			break;
		case ModeDontCare:
			break;
		case ModeSAlpha:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_ALPHANUMERIC, sentence );
			break;
		case ModeAlpha:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_ALPHANUMERIC | IME_CMODE_FULLSHAPE, sentence );
			break;
		case ModeHira:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, sentence );
			break;
		case ModeSKata:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE | IME_CMODE_KATAKANA, sentence );
			break;
		case ModeKata:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE | IME_CMODE_KATAKANA | IME_CMODE_FULLSHAPE, sentence );
			break;
		case ModeChinese:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, sentence );
			break;
		case ModeSHanguel:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE, sentence );
			break;
		case ModeHanguel:
			::ImmSetOpenStatus(hImc,TRUE);
			::ImmSetConversionStatus( hImc, IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE, sentence );
			break;
		}
		::ImmReleaseContext(hWnd_,hImc);
	}
};


#endif // __IME_CONTROL_H__
