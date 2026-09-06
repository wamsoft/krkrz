//---------------------------------------------------------------------------
// 最上位ホットキーフック (System.registerHotKey) — 詳細は HotKeyIntf.h
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "HotKeyIntf.h"
#include "tvpinputdefs.h"
#include "SysInitIntf.h"
#include "MsgIntf.h"
#include "DebugIntf.h"

#include <vector>

//---------------------------------------------------------------------------
namespace {

const tjs_uint32 TVP_HOTKEY_MOD_MASK = TVP_SS_SHIFT | TVP_SS_ALT | TVP_SS_CTRL;

struct tTVPHotKeyEntry
{
	tjs_uint Key;
	tjs_uint32 Mods;
	tTJSVariantClosure Closure; // AddRef 保持
	bool Held;                  // down を消費し、対応する up 待ちの間 true
};

std::vector<tTVPHotKeyEntry> TVPHotKeyVector;

void TVPDestroyHotKeyVector()
{
	for(auto& e : TVPHotKeyVector) e.Closure.Release();
	TVPHotKeyVector.clear();
}

tTVPAtExit TVPDestroyHotKeyVectorAtExit
	(TVP_ATEXIT_PRI_PREPARE, TVPDestroyHotKeyVector);

} // namespace
//---------------------------------------------------------------------------
void TVPRegisterHotKey(tjs_uint key, tjs_uint32 mods, const tTJSVariantClosure& clo)
{
	mods &= TVP_HOTKEY_MOD_MASK;
	for(auto& e : TVPHotKeyVector)
	{
		if(e.Key == key && e.Mods == mods)
		{
			// 同一キーへの再登録は差し替え
			e.Closure.Release();
			e.Closure = clo;
			e.Closure.AddRef();
			return;
		}
	}
	tTVPHotKeyEntry ent;
	ent.Key = key;
	ent.Mods = mods;
	ent.Closure = clo;
	ent.Closure.AddRef();
	ent.Held = false;
	TVPHotKeyVector.push_back(ent);
}
//---------------------------------------------------------------------------
bool TVPUnregisterHotKey(tjs_uint key, tjs_uint32 mods)
{
	mods &= TVP_HOTKEY_MOD_MASK;
	for(auto i = TVPHotKeyVector.begin(); i != TVPHotKeyVector.end(); ++i)
	{
		if(i->Key == key && i->Mods == mods)
		{
			i->Closure.Release();
			TVPHotKeyVector.erase(i);
			return true;
		}
	}
	return false;
}
//---------------------------------------------------------------------------
bool TVPProcessHotKey(tjs_uint key, tjs_uint32 shift, bool down, bool repeat)
{
	if(TVPHotKeyVector.empty()) return false;

	if(!down)
	{
		// up は key のみで照合する。 修飾キーが先に離されていても (Alt+Enter で
		// Alt を先に離す等)、 消費した down に対応する up は必ず消費して、
		// 片割れのキーイベントを入力レイヤへ漏らさない。
		for(auto& e : TVPHotKeyVector)
		{
			if(e.Key == key && e.Held)
			{
				e.Held = false;
				return true;
			}
		}
		return false;
	}

	if(repeat)
	{
		// 押しっぱなしのリピートは、 消費中のキーなら黙って消費するだけで
		// コールバックの再発火はしない。
		for(auto& e : TVPHotKeyVector)
			if(e.Key == key && e.Held) return true;
		return false;
	}

	tjs_uint32 mods = shift & TVP_HOTKEY_MOD_MASK;
	for(auto& e : TVPHotKeyVector)
	{
		if(e.Key != key || e.Mods != mods) continue;

		// モーダルポンプの中からも呼ばれるので同期呼び出し (Elements の
		// onAction と同じ扱い)。 例外でポンプを壊さないようログに留める。
		// コールバックが false を返したら「今回は処理しない」= イベントを
		// 消費せず通常の dispatch へ流す (状況依存のホットキー用)。
		bool consumed = true;
		try
		{
			tTJSVariant vkey((tjs_int)key), vshift((tjs_int)shift);
			tTJSVariant* params[] = { &vkey, &vshift };
			tTJSVariant res;
			e.Closure.FuncCall(0, nullptr, nullptr, &res, 2, params, nullptr);
			if(res.Type() != tvtVoid) consumed = res.operator bool();
		}
		catch(eTJS& err)
		{
			TVPAddImportantLog(ttstr(TJS_W("hotkey: callback error: ")) + err.GetMessage());
		}
		e.Held = consumed;
		return consumed;
	}
	return false;
}
//---------------------------------------------------------------------------
