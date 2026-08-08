//---------------------------------------------------------------------------
// TJS Variant → JSON テキスト (UTF-8) シリアライザ (VariantJsonUtil.h 参照)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "VariantJsonUtil.h"
#include "tjsArray.h"
#include "tjsDictionary.h"
#include "MsgIntf.h"
#include "CharacterSet.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <utility>

namespace {

void AppendJsonEscapedString(const ttstr& s, std::string& out)
{
	std::string utf8;
	tjs_string ts(s.c_str());
	TVPUtf16ToUtf8(utf8, ts);
	out += '"';
	for (char cc : utf8) {
		unsigned char c = static_cast<unsigned char>(cc);
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b";  break;
		case '\f': out += "\\f";  break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += cc;
			}
			break;
		}
	}
	out += '"';
}

void SerializeVariant(const tTJSVariant& v, std::string& out,
	std::vector<iTJSDispatch2*>& stack);

// Dictionary の全メンバを (key, value) で収集する EnumMembers コールバック。
// param[0]=メンバ名, param[1]=flags, param[2]=値。
struct tDictMemberCollector : public tTJSDispatch
{
	std::vector<std::pair<ttstr, tTJSVariant>> Members;

	tjs_error TJS_INTF_METHOD FuncCall(tjs_uint32 /*flag*/,
		const tjs_char* /*membername*/, tjs_uint32* /*hint*/,
		tTJSVariant* result, tjs_int numparams,
		tTJSVariant** param, iTJSDispatch2* /*objthis*/)
	{
		if (numparams < 3) return TJS_E_BADPARAMCOUNT;
		tjs_uint32 flags = (tjs_int)*param[1];
		if (!(flags & TJS_HIDDENMEMBER)) {
			Members.emplace_back(ttstr(*param[0]), *param[2]);
		}
		if (result) *result = (tjs_int)1;
		return TJS_S_OK;
	}
};

void SerializeObject(iTJSDispatch2* obj, std::string& out,
	std::vector<iTJSDispatch2*>& stack)
{
	if (!obj) { out += "null"; return; }

	for (iTJSDispatch2* p : stack) {
		if (p == obj) {
			TVPThrowExceptionMessage(TVPCannotSerializeCyclicReferenceToJSON);
		}
	}

	// Array
	tTJSArrayNI* arrayni = nullptr;
	if (TJS_SUCCEEDED(obj->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			TJSGetArrayClassID(), (iTJSNativeInstance**)&arrayni)) && arrayni) {
		stack.push_back(obj);
		out += '[';
		bool first = true;
		for (const tTJSVariant& item : arrayni->Items) {
			if (!first) out += ',';
			first = false;
			SerializeVariant(item, out, stack);
		}
		out += ']';
		stack.pop_back();
		return;
	}

	// Dictionary
	iTJSNativeInstance* dicni = nullptr;
	if (TJS_SUCCEEDED(obj->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			TJSGetDictionaryClassID(), &dicni)) && dicni) {
		tDictMemberCollector collector;
		tTJSVariantClosure clo(&collector, nullptr);
		obj->EnumMembers(TJS_IGNOREPROP, &clo, obj);
		stack.push_back(obj);
		out += '{';
		bool first = true;
		for (const auto& kv : collector.Members) {
			if (!first) out += ',';
			first = false;
			AppendJsonEscapedString(kv.first, out);
			out += ':';
			SerializeVariant(kv.second, out, stack);
		}
		out += '}';
		stack.pop_back();
		return;
	}

	TVPThrowExceptionMessage(TVPCannotSerializeObjectToJSONOnlyDictionaryArraySupported);
}

void SerializeVariant(const tTJSVariant& v, std::string& out,
	std::vector<iTJSDispatch2*>& stack)
{
	switch (v.Type()) {
	case tvtVoid:
		out += "null";
		break;
	case tvtInteger: {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%lld", (long long)v.AsInteger());
		out += buf;
		break;
	}
	case tvtReal: {
		double d = (double)v.AsReal();
		if (!std::isfinite(d)) {
			TVPThrowExceptionMessage(TVPCannotSerializeNonFiniteRealInfNanToJSON);
		}
		// 往復可能な最短表現: まず %.15g、 復元値がずれるときだけ %.17g
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.15g", d);
		if (std::strtod(buf, nullptr) != d) {
			std::snprintf(buf, sizeof(buf), "%.17g", d);
		}
		out += buf;
		break;
	}
	case tvtString:
		AppendJsonEscapedString(ttstr(v.AsStringNoAddRef()), out);
		break;
	case tvtObject:
		SerializeObject(v.AsObjectNoAddRef(), out, stack);
		break;
	default: // tvtOctet
		TVPThrowExceptionMessage(TVPCannotSerializeOctetToJSON);
	}
}

} // anonymous

void TVPVariantToJsonUtf8(const tTJSVariant& v, std::string& out)
{
	std::vector<iTJSDispatch2*> stack;
	SerializeVariant(v, out, stack);
}
