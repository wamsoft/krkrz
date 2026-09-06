#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
吉里吉里Z メッセージ定義の生成器 (旧 gentext.pl / gentext_generic.pl の Python 置換)。

- 標準ライブラリのみ。Excel / Win32::OLE / Perl 不要。
- 源(source of truth): common/msg/text/messages.csv
    列: section(tjs|tvp|tvp_win32), id, ja, en, chs, cht, opt(空|CRLF|ANSI)
    ja/en/chs/cht は「普通の文字列」。制御文字は \\n \\r \\t トークン、\\\\ で literal backslash。
    引用符はそのまま "(CSV 標準クォート)。cht (繁体字) 未記入は chs へフォールバック。
- 生成物:
    common/tjs2/tjsErrorInc.h                 TJS_MSG_DECL_NULL(...)   (section=tjs, opt無)
    common/msg/MsgIntfInc.h                   TVP_MSG_DECL_NULL(...)   (section=tvp, opt無)
    win32/msg/MsgImpl.h                        TVP_MSG_DECL_NULL(...)   (section=tvp_win32, opt無)
    resource/messages.json / -en / -chs / -cht 位置配列 JSON (プレーン。&quot; 等の HTML エンティティは廃止)
    win32/vcproj/string_table_{jp,en,chs}.rc   Win32 STRINGTABLE (UTF-16LE+BOM+CRLF)
    win32/vcproj/string_table_resource.h       #define IDS_...
    generic/msg/MsgLoad.cpp                    JSON 配列からロード
    win32/msg/MsgLoad.cpp                      .rc から LoadString でロード

使い方:
    python gen_messages.py            # 全生成物を出力
    python gen_messages.py --check    # 生成せず、既存生成物との差分を報告(検証用)
"""
import csv, json, os, re, sys, argparse, difflib

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.normpath(os.path.join(HERE, "..", "..", ".."))   # src/core
CSV_PATH = os.path.join(HERE, "messages.csv")

GEN_NOTE = "// generated from gen_messages.py messages.csv"

# ---------------------------------------------------------------------------
# 源の読み込み
# ---------------------------------------------------------------------------
def load_rows():
    rows = []
    with open(CSV_PATH, encoding="utf-8", newline="") as f:
        for d in csv.DictReader(f):
            rows.append({
                "section": d["section"].strip(),
                "id":  d["id"],
                "ja":  d["ja"],
                "en":  d["en"],
                "chs": d["chs"],
                "cht": d.get("cht") or d["chs"],   # cht 未記入は chs へフォールバック
                "opt": (d.get("opt") or "").strip(),
                # flags: "nodecl" = decl(extern/定義) を生成しない。
                #   共通ヘッダ common/msg/MsgIntf.h で手管理宣言済みの id 用
                #   (生成 decl と二重定義になるため抑止)。
                "flags": set(x for x in (d.get("flags") or "").split() if x),
            })
    return rows

def res_id_of(mid, opt):
    s = mid
    s = re.sub(r"^(TVP)", r"\1_", s)
    s = re.sub(r"^(TJS)", r"\1_", s)
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", s)
    s = s.upper()
    if opt:
        s += "_" + opt
    return "IDS_" + s

def enum_of(res_id):
    return "NUM_" + res_id[len("IDS_"):]

# ---------------------------------------------------------------------------
# エスケープ (canonical=普通の文字列。各ターゲットで正しく再エスケープ)
# ---------------------------------------------------------------------------
def decode_tokens(s):
    """CSV フィールドの \\n \\r \\t \\\\ トークンを実文字へ (それ以外の \\x は素通し)。"""
    out = []; i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "n": out.append("\n"); i += 2; continue
            if n == "r": out.append("\r"); i += 2; continue
            if n == "t": out.append("\t"); i += 2; continue
            if n == "\\": out.append("\\"); i += 2; continue
            out.append("\\"); out.append(n); i += 2; continue
        out.append(c); i += 1
    return "".join(out)

def json_str(s):
    """JSON 文字列リテラル (両端の " 込み)。プレーン化された正しい JSON。"""
    return json.dumps(decode_tokens(s), ensure_ascii=False)

def rc_str(s):
    """Win32 RC 文字列の中身。" -> ""。\\n \\r \\t トークンはそのまま(RC が解釈)。"""
    return s.replace('"', '""')

# ---------------------------------------------------------------------------
# 生成
# ---------------------------------------------------------------------------
def build_outputs(rows):
    res_ids = [res_id_of(r["id"], r["opt"]) for r in rows]
    enums   = [enum_of(rid) for rid in res_ids]
    maxlen  = max([24] + [len(rid) + 1 for rid in res_ids])

    out = {}

    # --- decl headers ---
    eh = [GEN_NOTE, "#ifndef __TJS_ERROR_INC_H__", "#define __TJS_ERROR_INC_H__"]
    mh = [GEN_NOTE, "#ifndef __MSG_INTF_INC_H__", "#define __MSG_INTF_INC_H__"]
    mwh = [GEN_NOTE, "#ifndef MsgImplH", "#define MsgImplH", "",
           '#include "tjsMessage.h"', '#include "MsgIntf.h"', "",
           "#ifndef TVP_MSG_DECL",
           "\t#define TVP_MSG_DECL(name, msg) extern tTJSMessageHolder name;",
           "\t#define TVP_MSG_DECL_NULL(name) extern tTJSMessageHolder name;",
           "#endif",
           "//---------------------------------------------------------------------------",
           "// Message Strings",
           "//---------------------------------------------------------------------------"]
    for i, r in enumerate(rows):
        if not r["opt"] and "nodecl" not in r["flags"]:
            # decl の出力先は section 列で決める (CSV 行順・末尾追記に依存しない)。
            if r["section"] == "tjs":
                eh.append("TJS_MSG_DECL_NULL(%s)" % r["id"])
            elif r["section"] == "tvp":
                mh.append("TVP_MSG_DECL_NULL(%s)" % r["id"])
            else:  # tvp_win32
                mwh.append("TVP_MSG_DECL_NULL(%s)" % r["id"])
    eh.append("#endif")
    mh.append("#endif")
    mwh.append("#endif")
    out["common/tjs2/tjsErrorInc.h"] = ("\n".join(eh) + "\n", "utf-8-lf")
    out["common/msg/MsgIntfInc.h"]   = ("\n".join(mh) + "\n", "utf-8-lf")
    out["win32/msg/MsgImpl.h"]       = ("\n".join(mwh) + "\n", "utf-8-lf")

    # --- messages*.json (位置配列, プレーン) ---
    def json_array(field):
        lines = ["["]
        for i, r in enumerate(rows):
            comma = "" if i == len(rows) - 1 else ","
            lines.append("        %s%s" % (json_str(r[field]), comma))
        lines.append("]")
        return "\n".join(lines) + "\n"
    out["resource/messages.json"]     = (json_array("ja"),  "utf-8-lf")
    out["resource/messages-en.json"]  = (json_array("en"),  "utf-8-lf")
    out["resource/messages-chs.json"] = (json_array("chs"), "utf-8-lf")  # 旧生成器の en 誤書込みを修正
    out["resource/messages-cht.json"] = (json_array("cht"), "utf-8-lf")

    # --- string_table_*.rc (UTF-16LE+BOM+CRLF) ---
    def rc_file(field):
        lines = ["STRINGTABLE", "BEGIN"]
        for i, r in enumerate(rows):
            pad = " " * (maxlen - len(res_ids[i]))
            lines.append('    %s%s"%s"' % (res_ids[i], pad, rc_str(r[field])))
        lines.append("END")
        return "\r\n".join(lines) + "\r\n"
    out["win32/vcproj/string_table_jp.rc"]  = (rc_file("ja"),  "utf-16le-bom")
    out["win32/vcproj/string_table_en.rc"]  = (rc_file("en"),  "utf-16le-bom")
    out["win32/vcproj/string_table_chs.rc"] = (rc_file("chs"), "utf-16le-bom")

    # --- string_table_resource.h ---
    rh = [GEN_NOTE, "#ifndef __STRING_TABLE_RESOURCE_H__", "#define __STRING_TABLE_RESOURCE_H__"]
    for i, rid in enumerate(res_ids):
        pad = " " * (maxlen - len(rid))
        rh.append("#define %s%s%d" % (rid, pad, i + 10000))
    rh.append("#endif")
    out["win32/vcproj/string_table_resource.h"] = ("\n".join(rh) + "\n", "utf-8-lf")

    # --- generic/msg/MsgLoad.cpp (JSON) ---
    g = [GEN_NOTE,
         '#include "tjsCommHead.h"', '#include "tjsError.h"', '#include "MsgImpl.h"',
         '#include "SysInitIntf.h"', '#include "MsgLoad.h"', '#include "CharacterSet.h"',
         "#include <stdexcept>", "#include <string>", "",
         "static bool IS_LOAD_MESSAGE = false;", "",
         "static tjs_string conv(const std::string &in) {",
         "\ttjs_string ret;", "\tTVPUtf8ToUtf16(ret, in);", "\treturn ret;", "}", "",
         "enum {"]
    for e in enums:
        g.append("\t%s," % e)
    g += ["\tNUM_MESSAGE_MAX", "};",
          "void TVPLoadMessage( picojson::array &array ) {",
          "\tif( IS_LOAD_MESSAGE ) return;",
          "\tif( array.size() < NUM_MESSAGE_MAX ) {",
          "\t\tthrow std::runtime_error(",
          "\t\t\t\"messages.json is outdated: \" + std::to_string(array.size()) +",
          "\t\t\t\" entries, engine expects \" + std::to_string((size_t)NUM_MESSAGE_MAX) +",
          "\t\t\t\" (regenerate with common/msg/text/gen_messages.py)\" );",
          "\t}",
          "\tIS_LOAD_MESSAGE = true;", "",
          "\tconst tjs_char* mes;", "\ttjs_uint length;"]
    for i, r in enumerate(rows):
        g.append("\t%s.AssignMessage( conv(array[%s].get<std::string>()).c_str() );" % (r["id"], enums[i]))
    g += ["", "}"]
    out["generic/msg/MsgLoad.cpp"] = ("\n".join(g) + "\n", "utf-8-lf")

    # --- win32/msg/MsgLoad.cpp (.rc / LoadString) ---
    mesmaxlen = 1024
    w = [GEN_NOTE,
         '#include "tjsCommHead.h"', '#include "tjsError.h"', '#include "MsgImpl.h"',
         '#include "SysInitIntf.h"', '#include "string_table_resource.h"', "",
         "static bool IS_LOAD_MESSAGE = false;",
         "static const int MAX_MESSAGE_LENGTH = %d;" % mesmaxlen,
         "enum {"]
    for e in enums:
        w.append("\t%s," % e)
    w += ["\tNUM_MESSAGE_MAX", "};",
          "const tjs_char* RESOURCE_MESSAGE[NUM_MESSAGE_MAX];",
          "const int RESOURCE_IDS[NUM_MESSAGE_MAX] = {"]
    for rid in res_ids:
        w.append("\t%s," % rid)
    w += ["};",
          "void TVPLoadMessage() {",
          "\tif( IS_LOAD_MESSAGE ) return;", "\tIS_LOAD_MESSAGE = true;",
          "\twchar_t buffer[MAX_MESSAGE_LENGTH];",
          "\tHINSTANCE hInstance = ::GetModuleHandle(0);",
          "\tfor( int i = 0; i < NUM_MESSAGE_MAX; i++ ) {",
          "\t\tint len = ::LoadString( hInstance, RESOURCE_IDS[i], buffer, MAX_MESSAGE_LENGTH );",
          "\t\tif( len <= 0 ) {",
          '\t\t\tTVPThrowExceptionMessage( TJS_W("Message Load Error!") );',
          "\t\t}",
          "\t\twchar_t* work = new wchar_t[len+1];",
          "\t\twcscpy_s( work, len+1, buffer );",
          "\t\tRESOURCE_MESSAGE[i] = (tjs_char*)work;",
          "\t}"]
    is_opt = False
    for i, r in enumerate(rows):
        e = enums[i]
        if r["opt"] == "CRLF":
            w.append("#ifdef TJS_TEXT_OUT_CRLF")
            w.append("\t%s.AssignMessage( RESOURCE_MESSAGE[%s] );" % (r["id"], e))
            w.append("#else")
            is_opt = True
        elif r["opt"] == "ANSI":
            w.append("#ifdef TVP_TEXT_READ_ANSI_MBCS")
            w.append("\t%s.AssignMessage( RESOURCE_MESSAGE[%s] );" % (r["id"], e))
            w.append("#else")
            is_opt = True
        else:
            w.append("\t%s.AssignMessage( RESOURCE_MESSAGE[%s] );" % (r["id"], e))
            if is_opt:
                w.append("#endif")
                is_opt = False
    w += ["}",
          "const tjs_char* TVPGetMessage( tjs_int id ) {",
          "\tif( id >= 0 && id < NUM_MESSAGE_MAX ) {",
          "\t\treturn RESOURCE_MESSAGE[id];",
          "\t} else {",
          "\t\treturn NULL;",
          "\t}",
          "}",
          "static void TVPFreeMessages() {",
          "\tfor( int i = 0; i < NUM_MESSAGE_MAX; i++ ) {",
          "\t\tdelete[] RESOURCE_MESSAGE[i];",
          "\t}",
          "}",
          "static tTVPAtExit",
          "\tTVPUninitMessageLoad(TVP_ATEXIT_PRI_RELEASE, TVPFreeMessages);"]
    out["win32/msg/MsgLoad.cpp"] = ("\n".join(w) + "\n", "utf-8-lf")

    return out

def encode(content, enc):
    if enc == "utf-8-lf":
        return content.encode("utf-8")
    if enc == "utf-16le-bom":
        return b"\xff\xfe" + content.encode("utf-16-le")
    raise ValueError(enc)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="生成せず既存との差分を報告")
    args = ap.parse_args()
    rows = load_rows()
    outputs = build_outputs(rows)
    print("rows=%d, outputs=%d" % (len(rows), len(outputs)))
    changed = 0
    for rel, (content, enc) in sorted(outputs.items()):
        path = os.path.join(CORE, rel)
        data = encode(content, enc)
        old = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                old = f.read()
        if old == data:
            status = "same"
        else:
            status = "DIFF" if old is not None else "new"
            changed += 1
        print("  [%4s] %s" % (status, rel))
        if not args.check:
            with open(path, "wb") as f:
                f.write(data)
    print(("生成完了" if not args.check else "check のみ") + " / 変更 %d 件" % changed)

if __name__ == "__main__":
    main()
