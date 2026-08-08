//---------------------------------------------------------------------------
// UserConfig オプション記述 JSON パーサとマージ機構 (プラットフォーム共通)
//
// 元実装は win32/msg/ReadOptionDesc.cpp に内蔵されていたもの。SDL3 build でも
// 同じ JSON フォーマットを読むため共通化した。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "ReadOptionDesc.h"
#include "DebugIntf.h"
#include "CharacterSet.h"

#include <picojson/picojson.h>
#include <map>
#include <string>

namespace {

class OptionDescReader {
	static const char* CATEGORY;
	static const char* OPTIONS;
	static const char* CAPTION;
	static const char* DESCRIPTION;
	static const char* NAME;
	static const char* TYPE;
	static const char* LENGTH;
	static const char* VALUE;
	static const char* VALUES;
	static const char* DESC;
	static const char* DEFAULT;
	static const char* USER;

	static bool GetBoolean(const char* name, const picojson::object& obj) {
		auto v = obj.find(std::string(name));
		if (v != obj.end()) {
			const picojson::value& val = v->second;
			if (val.is<bool>()) return val.get<bool>();
		}
		return false;
	}
	static double GetNumber(const char* name, const picojson::object& obj) {
		auto v = obj.find(std::string(name));
		if (v != obj.end()) {
			const picojson::value& val = v->second;
			// PICOJSON_USE_INT64 ビルドでは整数リテラルは int64_t で保持され
			// is<double>() が false になるので両方を見る
			if (val.is<double>()) return val.get<double>();
			if (val.is<int64_t>()) return (double)val.get<int64_t>();
		}
		return 0;
	}
	static void RetriveString(tjs_string& out, const picojson::object& obj, const char* name) {
		auto v = obj.find(std::string(name));
		if (v != obj.end()) {
			const picojson::value& val = v->second;
			if (val.is<std::string>()) {
				TVPUtf8ToUtf16(out, val.get<std::string>());
			} else if (val.is<double>()) {
				int vi = (int)val.get<double>();
				out = to_tjs_string(vi);
			} else if (val.is<int64_t>()) {
				// PICOJSON_USE_INT64 ビルドの整数リテラル
				int vi = (int)val.get<int64_t>();
				out = to_tjs_string(vi);
			} else if (val.is<bool>()) {
				out = val.get<bool>() ? TJS_W("true") : TJS_W("false");
			}
		}
	}
	void ParseOption(tTVPCommandOption& opt, const picojson::object& option);
	void ParseValue(tTVPCommandOptionsValue& val, const picojson::object& value);
public:
	tTVPCommandOptionList* Parse(const picojson::value& v);
};

const char* OptionDescReader::CATEGORY    = "category";
const char* OptionDescReader::OPTIONS     = "options";
const char* OptionDescReader::CAPTION     = "caption";
const char* OptionDescReader::DESCRIPTION = "description";
const char* OptionDescReader::NAME        = "name";
const char* OptionDescReader::TYPE        = "type";
const char* OptionDescReader::LENGTH      = "length";
const char* OptionDescReader::VALUE       = "value";
const char* OptionDescReader::VALUES      = "values";
const char* OptionDescReader::DESC        = "desc";
const char* OptionDescReader::DEFAULT     = "default";
const char* OptionDescReader::USER        = "user";

void OptionDescReader::ParseValue(tTVPCommandOptionsValue& val, const picojson::object& value) {
	RetriveString(val.Value, value, VALUE);
	RetriveString(val.Description, value, DESC);
	val.IsDefault = GetBoolean(DEFAULT, value);
}

void OptionDescReader::ParseOption(tTVPCommandOption& opt, const picojson::object& option) {
	RetriveString(opt.Caption, option, CAPTION);
	RetriveString(opt.Description, option, DESCRIPTION);
	RetriveString(opt.Name, option, NAME);
	tjs_string type;
	RetriveString(type, option, TYPE);
	opt.Length = 0;
	if (type == tjs_string(TJS_W("select"))) {
		opt.Type = tTVPCommandOption::VT_Select;
	} else if (type == tjs_string(TJS_W("string"))) {
		opt.Type = tTVPCommandOption::VT_String;
		opt.Length = (tjs_int)GetNumber(LENGTH, option);
		RetriveString(opt.Value, option, VALUE);
	} else {
		opt.Type = tTVPCommandOption::VT_Unknown;
	}
	opt.User = GetBoolean(USER, option);
	auto v = option.find(std::string(VALUES));
	if (v != option.end()) {
		const picojson::value& val = v->second;
		if (val.is<picojson::array>()) {
			const picojson::array& values = val.get<picojson::array>();
			size_t count = values.size();
			opt.Values.resize(count);
			for (size_t i = 0; i < count; i++) {
				tTVPCommandOptionsValue& value = opt.Values[i];
				const picojson::value& jval = values[i];
				if (jval.is<picojson::object>()) {
					ParseValue(value, jval.get<picojson::object>());
				}
			}
		}
	}
}

tTVPCommandOptionList* OptionDescReader::Parse(const picojson::value& v) {
	tTVPCommandOptionList* result = nullptr;
	if (v.is<picojson::array>()) {
		result = new tTVPCommandOptionList();
		const picojson::array& categories = v.get<picojson::array>();
		size_t count = categories.size();
		result->Categories.resize(count);
		for (size_t i = 0; i < count; i++) {
			tTVPCommandOptionCategory& optioncat = result->Categories[i];
			const picojson::value& category = categories[i];
			if (category.is<picojson::object>()) {
				const picojson::object& cat = category.get<picojson::object>();
				RetriveString(optioncat.Name, cat, CATEGORY);
				auto opt = cat.find(OPTIONS);
				if (opt != cat.end()) {
					const picojson::value& options = opt->second;
					if (options.is<picojson::array>()) {
						const picojson::array& optionarray = options.get<picojson::array>();
						size_t optcount = optionarray.size();
						optioncat.Options.resize(optcount);
						for (size_t j = 0; j < optcount; j++) {
							tTVPCommandOption& toption = optioncat.Options[j];
							const picojson::value& option = optionarray[j];
							if (option.is<picojson::object>()) {
								ParseOption(toption, option.get<picojson::object>());
							}
						}
					}
				}
			}
		}
	}
	return result;
}

} // anonymous

//---------------------------------------------------------------------------
// 公開 API
//---------------------------------------------------------------------------
tTVPCommandOptionList* TVPParseCommandDescJson(const char* buf, size_t size)
{
	if (buf == nullptr || size == 0) return nullptr;
	picojson::value v;
	std::string errorstr;
	picojson::parse(v, buf, buf + size, &errorstr);
	if (!errorstr.empty()) {
		tjs_string errmessage;
		if (TVPUtf8ToUtf16(errmessage, errorstr)) {
			TVPAddImportantLog(errmessage.c_str());
		}
		return nullptr;
	}
	OptionDescReader reader;
	return reader.Parse(v);
}

void TVPMargeCommandDesc(tTVPCommandOptionList& dest, const tTVPCommandOptionList& src)
{
	tjs_uint count = (tjs_uint)src.Categories.size();
	std::vector<tjs_uint> addcat;
	addcat.reserve(count);
	for (tjs_uint i = 0; i < count; i++) {
		const tTVPCommandOptionCategory& srccat = src.Categories[i];
		tjs_uint dcnt = (tjs_uint)dest.Categories.size();
		bool found = false;
		for (tjs_uint j = 0; j < dcnt; j++) {
			tTVPCommandOptionCategory& dstcat = dest.Categories[j];
			if (dstcat.Name == srccat.Name) {
				found = true;
				tjs_uint optcount = (tjs_uint)srccat.Options.size();
				dstcat.Options.reserve(dstcat.Options.size() + optcount);
				for (tjs_uint k = 0; k < optcount; k++) {
					dstcat.Options.push_back(srccat.Options[k]);
				}
				break;
			}
		}
		if (!found) addcat.push_back(i);
	}
	for (auto idx : addcat) {
		dest.Categories.push_back(src.Categories[idx]);
	}
}
