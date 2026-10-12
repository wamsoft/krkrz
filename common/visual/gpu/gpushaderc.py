#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gpushaderc.py — Canvas (SDL_GPU 版) 用シェーダー変換ツール

TJS の ShaderProgram に渡す GLSL (GLSL ES 1.00 / 3.00 流) を、SDL_GPU の SPIR-V
規約に合う GLSL 450 へ書き換えてから SPIR-V にし、リフレクション情報と一緒に
パックファイル (.kzgs) にまとめる。実行時はソースのハッシュでパックを引く。

  書き換え:
    - attribute / varying / gl_FragColor / texture2D 等 → in / out / 出力変数 / texture
    - ばらの uniform → stage ごとの uniform ブロック (std140)
        頂点: layout(set=1, binding=0)   フラグメント: layout(set=3, binding=0)
    - sampler2D → 頂点 set=0 / フラグメント set=2 の binding を宣言順に
    - attribute / varying → layout(location=N) (varying は頂点側の宣言順で揃える)

  必要なもの: glslangValidator と spirv-cross (Vulkan SDK の Bin、または PATH)。

使い方:
  gpushaderc.py pack   -o out.kzgs  [--spv-dir DIR]  [--pair VS FS]...  [INPUTS...]
  gpushaderc.py builtin -o GpuBuiltinShaders.inc  shaders/builtin.json
  gpushaderc.py rewrite VS FS          (書き換え結果を表示するだけ。デバッグ用)

  機種専用形式 (SDL_GPU の PRIVATE) のパックを作るとき (pack / builtin 共通):
    --target private --hlsl-compile-vs "CMD" --hlsl-compile-fs "CMD"
  SPIR-V を spirv-cross で HLSL (SM5.0) にし、CMD でバイナリにする。CMD の {in} が
  HLSL ファイル、{out} が出力ファイルに置き換わる (パスは / 区切りで渡すこと)。
  機種のコンパイラとその引数は機種側のビルドが与える (このツールは持たない)。
  builtin で --symbol NAME を付けると、ソースを含まずパックだけを NAME の配列で出す。

  D3D12 (SDL_GPU の DXIL) のパックを作るとき (pack / builtin 共通):
    --target dxil
  SPIR-V を spirv-cross で HLSL (SM6.0) にし、dxc で DXIL にする。リソースの set は
  SDL_GPU の D3D12 の register space (頂点 t/s=space0, b=space1 / フラグメント
  t/s=space2, b=space3) にそのまま対応する。dxc は VULKAN_SDK の Bin か PATH から探す。
  --dxc PATH で使う dxc を指定できる (機種向けの dxc を使うと事前コンパイル済みのコードが
  入り、実行時の再コンパイルが要らなくなる。Xbox なら GXDK の bin/<機種>/dxc.exe)。
  D3D12 には点の大きさが無いので、gl_PointSize を書く頂点シェーダーは点を四角形に
  広げる形にする (角の属性 _kz_corner と画面サイズ _kz_viewport を足す。エンジンが
  点のリストを四角形のインスタンス描画にする)。gl_PointCoord もそこから求める。

  --pair で頂点・フラグメントのファイルを組で渡すか、INPUTS に次を渡す:
    - 実行時に書き出された収集ディレクトリ (*.vs と同名の *.fs の組)
    - JSON (builtin.json と同じ形式: {"programs":[{"name":..,"vs":..,"fs":..}]})

キー: UTF-8 の (vs + "\\0" + fs) に FNV-1a 64bit。テキストは BOM を除き CRLF を LF に
そろえてからハッシュする (実行時の GpuShaderProgram と同じ規則)。
"""

import argparse
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

PACK_MAGIC = b"KZGS"
PACK_VERSION = 2
# パックに入れるシェーダーの形式 (SDL_GPU の SDL_GPU_SHADERFORMAT_SPIRV / _PRIVATE に対応)
FORMAT_SPIRV = 1
FORMAT_PRIVATE = 2
FORMAT_DXIL = 3

# SDL_GPU の SPIR-V リソースセット (SDL_gpu.h の SDL_CreateGPUShader の説明)
SET_VS_SAMPLERS, SET_VS_UNIFORMS = 0, 1
SET_FS_SAMPLERS, SET_FS_UNIFORMS = 2, 3

SAMPLER_TYPES = {"sampler2D", "samplerCube", "sampler2DArray", "sampler3D"}
PRECISIONS = {"lowp", "mediump", "highp"}
INTERP = {"flat", "smooth", "centroid", "noperspective", "invariant"}
BUILTIN_RENAMES = {
    "texture2D": "texture",
    "texture2DLod": "textureLod",
    "texture2DProj": "textureProj",
    "texture2DProjLod": "textureProjLod",
    "textureCube": "texture",
    "textureCubeLod": "textureLod",
    "texture2DLodEXT": "textureLod",
    "textureCubeLodEXT": "textureLod",
    "gl_FragColor": "_kz_FragColor",
    "gl_FragData": "_kz_FragData",
}


class ShaderError(Exception):
    pass


# --------------------------------------------------------------------------
# 正規化とハッシュ (C++ 側 GpuShaderProgram と同じ規則)
# --------------------------------------------------------------------------
def normalize_text(s):
    if s.startswith("﻿"):
        s = s[1:]
    return s.replace("\r\n", "\n")


def program_key(vs, fs):
    data = (normalize_text(vs) + "\0" + normalize_text(fs)).encode("utf-8")
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# --------------------------------------------------------------------------
# トークナイザ
# --------------------------------------------------------------------------
_TOKEN_RE = re.compile(
    r"(?P<ws>\s+)"
    r"|(?P<lc>//[^\n]*)"
    r"|(?P<bc>/\*.*?\*/)"
    r"|(?P<pp>#(?:[^\n\\]|\\.)*)"
    r"|(?P<id>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<num>\d+\.?\d*(?:[eE][+-]?\d+)?[fFuU]?|\.\d+(?:[eE][+-]?\d+)?[fF]?)"
    r"|(?P<p>.)",
    re.S,
)


def tokenize(src):
    toks = []
    pos = 0
    # 行頭以外の '#' も pp として拾うが GLSL では行頭のみ現れる前提
    for m in _TOKEN_RE.finditer(src):
        kind = m.lastgroup
        toks.append((kind, m.group()))
        pos = m.end()
    return toks


def significant(toks):
    return [t for t in toks if t[0] not in ("ws", "lc", "bc")]


# --------------------------------------------------------------------------
# トップレベル宣言の解析
# --------------------------------------------------------------------------
class Decl:
    """storage 修飾付きのトップレベル宣言 1 文 (複数の名前を含みうる)"""

    def __init__(self, storage, quals, type_, names, layout):
        self.storage = storage        # attribute / varying / uniform / in / out
        self.quals = quals            # precision / interp 修飾 (文字列リスト)
        self.type = type_
        self.names = names            # [(name, array_suffix or "")]
        self.layout = layout          # 元の layout(...) の中身 (無視するが保持)


def parse_decl(stoks):
    """significant トークン列 (末尾 ';' を除く) を Decl にする。対象外なら None。"""
    i = 0
    layout = None
    if i < len(stoks) and stoks[i][1] == "layout":
        # layout ( ... )
        depth = 0
        j = i + 1
        while j < len(stoks):
            if stoks[j][1] == "(":
                depth += 1
            elif stoks[j][1] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        layout = " ".join(t[1] for t in stoks[i + 2:j])
        i = j + 1
    quals = []
    storage = None
    while i < len(stoks) and stoks[i][0] == "id" and (
        stoks[i][1] in PRECISIONS or stoks[i][1] in INTERP
        or stoks[i][1] in ("attribute", "varying", "uniform", "in", "out", "const")
    ):
        w = stoks[i][1]
        if w in ("attribute", "varying", "uniform", "in", "out"):
            storage = w
        elif w == "const":
            return None
        else:
            quals.append(w)
        i += 1
    if storage is None:
        return None
    if i >= len(stoks) or stoks[i][0] != "id":
        raise ShaderError("宣言の型が読めない: " + " ".join(t[1] for t in stoks))
    if stoks[i][1] == "struct" or (i + 1 < len(stoks) and stoks[i + 1][1] == "{"):
        raise ShaderError("struct / interface block の %s は未対応" % storage)
    # 型の前後に precision が来ることもある
    type_ = stoks[i][1]
    i += 1
    while i < len(stoks) and stoks[i][1] in PRECISIONS:
        quals.append(stoks[i][1])
        i += 1
    names = []
    while i < len(stoks):
        if stoks[i][0] != "id":
            raise ShaderError("宣言の名前が読めない: " + " ".join(t[1] for t in stoks))
        name = stoks[i][1]
        i += 1
        arr = ""
        if i < len(stoks) and stoks[i][1] == "[":
            j = i
            while j < len(stoks) and stoks[j][1] != "]":
                j += 1
            arr = "".join(t[1] for t in stoks[i:j + 1])
            i = j + 1
        if i < len(stoks) and stoks[i][1] == "=":
            raise ShaderError("初期値付きの %s は未対応: %s" % (storage, name))
        names.append((name, arr))
        if i < len(stoks) and stoks[i][1] == ",":
            i += 1
            continue
        break
    if i != len(stoks):
        raise ShaderError("宣言の末尾が読めない: " + " ".join(t[1] for t in stoks))
    return Decl(storage, quals, type_, names, layout)


def split_top_level(toks):
    """トップレベルの文に分ける。戻り値は ("decl", tokens) / ("raw", tokens) の列。

    'decl' はブレース深さ 0 で ';' で終わる文 (トークンは ';' を含む)。
    関数定義などその他は 'raw' のまま。
    """
    out = []
    cur = []
    depth = 0
    paren = 0
    for t in toks:
        cur.append(t)
        if t[0] == "pp" and depth == 0 and paren == 0 and len(significant(cur)) == 1:
            out.append(("raw", cur))
            cur = []
            continue
        v = t[1]
        if t[0] != "p":
            continue
        if v == "(":
            paren += 1
        elif v == ")":
            paren -= 1
        elif v == "{":
            depth += 1
        elif v == "}":
            depth -= 1
            if depth == 0 and paren == 0:
                out.append(("raw", cur))
                cur = []
        elif v == ";" and depth == 0 and paren == 0:
            out.append(("decl", cur))
            cur = []
    if cur:
        out.append(("raw", cur))
    return out


# --------------------------------------------------------------------------
# 書き換え本体
# --------------------------------------------------------------------------
class StageInfo:
    def __init__(self, stage):
        self.stage = stage            # "vert" / "frag"
        self.inputs = []              # [(quals, type, name, arr)]
        self.outputs = []
        self.uniforms = []            # [(quals, type, name, arr)]
        self.samplers = []            # [(type, name, arr)]
        self.body = []                # 書き換え後の文字列片 (宣言以外)
        self.uses_fragdata = False
        self.uses_fragcolor = False
        self.has_es3_output = False


def analyze(src, stage):
    info = StageInfo(stage)
    toks = tokenize(normalize_text(src))
    for kind, stmt in split_top_level(toks):
        sig = significant(stmt)
        if kind == "decl" and sig:
            if sig[0][1] == "precision":
                info.body.append(_emit(stmt))
                continue
            d = parse_decl(sig[:-1])
            if d is not None:
                _classify(info, d)
                continue
        if len(sig) == 1 and sig[0][0] == "pp":
            line = sig[0][1].strip()
            if re.match(r"#\s*(version|extension)\b", line):
                continue
            info.body.append(stmt[0][1] if stmt[0][0] == "ws" else "")
            info.body.append(line + "\n")
            continue
        info.body.append(_emit(stmt, info))
    return info


def _classify(info, d):
    for name, arr in d.names:
        if d.storage == "uniform":
            if d.type in SAMPLER_TYPES:
                info.samplers.append((d.type, name, arr))
            else:
                info.uniforms.append((d.quals, d.type, name, arr))
        elif d.storage == "attribute":
            if info.stage != "vert":
                raise ShaderError("フラグメントシェーダーに attribute がある")
            info.inputs.append((d.quals, d.type, name, arr))
        elif d.storage == "varying":
            if info.stage == "vert":
                info.outputs.append((d.quals, d.type, name, arr))
            else:
                info.inputs.append((d.quals, d.type, name, arr))
        elif d.storage == "in":
            info.inputs.append((d.quals, d.type, name, arr))
        elif d.storage == "out":
            if info.stage == "frag":
                info.has_es3_output = True
            info.outputs.append((d.quals, d.type, name, arr))


def _emit(stmt, info=None):
    parts = []
    for kind, v in stmt:
        if kind == "id" and v in BUILTIN_RENAMES:
            if info is not None:
                if v == "gl_FragColor":
                    info.uses_fragcolor = True
                elif v == "gl_FragData":
                    info.uses_fragdata = True
            parts.append(BUILTIN_RENAMES[v])
        elif kind == "pp":
            parts.append(v + "\n")
        else:
            parts.append(v)
    return "".join(parts)


def _decl_str(quals, type_, name, arr):
    q = " ".join(x for x in quals if x not in PRECISIONS or True)
    return ("%s %s %s%s" % (q, type_, name, arr)).strip()


def rewrite_pair(vs_src, fs_src, point_sprite=False):
    """(vs, fs) を GLSL 450 に書き換える。戻り値 (vs450, fs450, varying_locations)

    point_sprite: 点の大きさが無い環境 (D3D12) 向けに、gl_PointSize を書く頂点
    シェーダーを「点を四角形に広げる」形にする (_point_sprite を参照)。
    """
    vs = analyze(vs_src, "vert")
    fs = analyze(fs_src, "frag")

    # varying の location を頂点側の宣言順で決め、フラグメント側は名前で合わせる
    vloc = {}
    for q, t, n, a in vs.outputs:
        vloc[n] = len(vloc)
    nxt = len(vloc)
    for q, t, n, a in fs.inputs:
        if n not in vloc:
            vloc[n] = nxt
            nxt += 1

    sprite = point_sprite and _writes_point_size(vs)
    if sprite:
        # 画面サイズは頂点の uniform ブロックに足す (エンジンはブロックを 1 つだけ送る)
        vs.uniforms.append(([], "vec2", "_kz_viewport", ""))
    vs450, fs450 = _build(vs, vloc), _build(fs, vloc)
    if sprite:
        vs450, fs450 = _point_sprite(vs450, fs450, len(vs.inputs), nxt)
    return vs450, fs450, vloc


def _writes_point_size(info):
    return any("gl_PointSize" in part for part in info.body)


def _point_sprite(vs450, fs450, corner_loc, pc_loc):
    """点を四角形に広げる形に書き換える。

    エンジン (GpuCanvas::DrawMesh) は 4 頂点の三角形ストリップを点の数だけインスタンス
    描画し、角 (-1..1) を _kz_corner、描画先の大きさを _kz_viewport に入れる。元の main
    で求めた位置を、角の方向へ gl_PointSize の半分 (ピクセル) ずつずらす。gl_PointCoord は
    角から求めて _kz_pc で渡す (GL と同じく左上が 0,0)。
    """
    if not re.search(r"\bvoid\s+main\s*\(", vs450):
        raise ShaderError("点を四角形にできない (頂点シェーダーの main が見つからない)")
    head, rest = vs450.split("\n", 1)
    vs450 = (head + "\n"
             + "layout(location=%d) in vec2 _kz_corner;\n" % corner_loc
             + "layout(location=%d) out vec2 _kz_pc;\n" % pc_loc
             + re.sub(r"\bvoid\s+main\s*\(", "void _kz_main(", rest, count=1)
             + "\nvoid main() {\n"
             + "  _kz_main();\n"
             + "  gl_Position.xy += _kz_corner * gl_PointSize / _kz_viewport * gl_Position.w;\n"
             + "  _kz_pc = vec2(_kz_corner.x, -_kz_corner.y) * 0.5 + 0.5;\n"
             + "}\n")
    if "gl_PointCoord" in fs450:
        head, rest = fs450.split("\n", 1)
        fs450 = (head + "\nlayout(location=%d) in vec2 _kz_pc;\n" % pc_loc
                 + rest.replace("gl_PointCoord", "_kz_pc"))
    return vs450, fs450


def _build(info, vloc):
    out = ["#version 450\n"]
    if info.stage == "vert":
        for i, (q, t, n, a) in enumerate(info.inputs):
            out.append("layout(location=%d) in %s;\n" % (i, _decl_str(q, t, n, a)))
        for q, t, n, a in info.outputs:
            out.append("layout(location=%d) out %s;\n" % (vloc[n], _decl_str(q, t, n, a)))
        sset, uset = SET_VS_SAMPLERS, SET_VS_UNIFORMS
        block = "_KZ_VSU"
    else:
        for q, t, n, a in info.inputs:
            out.append("layout(location=%d) in %s;\n" % (vloc[n], _decl_str(q, t, n, a)))
        if info.has_es3_output:
            for i, (q, t, n, a) in enumerate(info.outputs):
                out.append("layout(location=%d) out %s;\n" % (i, _decl_str(q, t, n, a)))
        if info.uses_fragcolor:
            out.append("layout(location=0) out vec4 _kz_FragColor;\n")
        if info.uses_fragdata:
            out.append("layout(location=0) out vec4 _kz_FragData[1];\n")
        sset, uset = SET_FS_SAMPLERS, SET_FS_UNIFORMS
        block = "_KZ_FSU"
    for i, (t, n, a) in enumerate(info.samplers):
        out.append("layout(set=%d, binding=%d) uniform %s %s%s;\n" % (sset, i, t, n, a))
    if info.uniforms:
        out.append("layout(set=%d, binding=0, std140) uniform %s {\n" % (uset, block))
        for q, t, n, a in info.uniforms:
            out.append("  %s;\n" % _decl_str(q, t, n, a))
        out.append("};\n")
    out.extend(info.body)
    return "".join(out)


# --------------------------------------------------------------------------
# glslang / spirv-cross
# --------------------------------------------------------------------------
def find_tool(name):
    cands = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        cands.append(os.path.join(sdk, "Bin", name))
        cands.append(os.path.join(sdk, "bin", name))
    for c in cands:
        for ext in ("", ".exe"):
            if os.path.isfile(c + ext):
                return c + ext
    p = shutil.which(name)
    if p:
        return p
    raise ShaderError("%s が見つからない (VULKAN_SDK か PATH を設定すること)" % name)


def compile_spirv(src, stage, workdir, label):
    glslang = find_tool("glslangValidator")
    path = os.path.join(workdir, "%s.%s" % (label, stage))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)
    spv = path + ".spv"
    r = subprocess.run(
        [glslang, "-V", "--target-env", "vulkan1.0", "-S", stage, "-o", spv, path],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    if r.returncode != 0:
        raise ShaderError("glslang でエラー (%s.%s):\n%s\n--- 書き換え後のソース ---\n%s"
                          % (label, stage, (r.stdout + r.stderr).strip(), _numbered(src)))
    with open(spv, "rb") as f:
        return f.read(), spv


def reflect(spv_path):
    cross = find_tool("spirv-cross")
    r = subprocess.run([cross, spv_path, "--reflect"], capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise ShaderError("spirv-cross --reflect でエラー: " + r.stderr)
    return json.loads(r.stdout)


def _numbered(src):
    return "\n".join("%4d: %s" % (i + 1, l) for i, l in enumerate(src.split("\n")))


# --------------------------------------------------------------------------
# リフレクション → テキスト
#   B <v|f> <block_size>
#   U <v|f> <name> <offset> <type> <array_count> <array_stride> <matrix_stride>
#   S <v|f> <name> <binding> <type>
#   A <name> <location> <type>
# --------------------------------------------------------------------------
def reflection_text(vs_ref, fs_ref):
    lines = []
    for tag, ref in (("v", vs_ref), ("f", fs_ref)):
        types = ref.get("types", {})
        for ubo in ref.get("ubos", []):
            lines.append("B %s %d" % (tag, ubo["block_size"]))
            t = types.get(ubo["type"], {})
            for m in t.get("members", []):
                arr = m.get("array", [])
                if len(arr) > 1:
                    raise ShaderError("多次元配列の uniform は未対応: " + m["name"])
                lines.append("U %s %s %d %s %d %d %d" % (
                    tag, m["name"], m["offset"], m["type"],
                    arr[0] if arr else 0, m.get("array_stride", 0), m.get("matrix_stride", 0)))
        for tex in ref.get("textures", []):
            lines.append("S %s %s %d %s" % (tag, tex["name"], tex["binding"], tex["type"]))
    for inp in vs_ref.get("inputs", []):
        lines.append("A %s %d %s" % (inp["name"], inp["location"], inp["type"]))
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# プログラム 1 組の変換
# --------------------------------------------------------------------------
class Compiled:
    def __init__(self, key, vs_spv, fs_spv, refl, vs450, fs450):
        self.key, self.vs_spv, self.fs_spv, self.refl = key, vs_spv, fs_spv, refl
        self.vs450, self.fs450 = vs450, fs450
        # パックに入れるコード (target が private なら機種のバイナリ)
        self.vs_code, self.fs_code = vs_spv, fs_spv


class Target:
    """出力形式。private なら HLSL を経由して機種のコンパイラにかける"""

    def __init__(self, kind="spirv", vs_cmd=None, fs_cmd=None, dxc=None):
        self.kind = kind
        self.vs_cmd, self.fs_cmd = vs_cmd, fs_cmd
        self.dxc = dxc
        if dxc and kind != "dxil":
            raise ShaderError("--dxc は --target dxil のときだけ使える")
        if kind == "private" and not (vs_cmd and fs_cmd):
            raise ShaderError("--target private には --hlsl-compile-vs と --hlsl-compile-fs が要る")

    @property
    def format(self):
        return {"private": FORMAT_PRIVATE, "dxil": FORMAT_DXIL}.get(self.kind, FORMAT_SPIRV)


def to_hlsl(spv_path, out_path, shader_model="50"):
    # SDL_shadercross と同じオプション (entry は main)。
    #   SM5.0: PSSL 互換モード (機種専用形式)。register space を出さない
    #   SM6.0: DXIL。set を register space として出す (SDL_GPU の D3D12 の規約)
    cross = find_tool("spirv-cross")
    # --hlsl-enable-compat: gl_PointSize / gl_PointCoord を許す (HLSL には無いので
    # 書き込みは捨てられ、gl_PointCoord は定数になる)
    args = [cross, spv_path, "--hlsl", "--shader-model", shader_model,
            "--hlsl-nonwritable-uav-texture-as-srv",
            "--hlsl-flatten-matrix-vertex-input-semantics",
            "--hlsl-enable-compat"]
    r = subprocess.run(args + ["--output", out_path],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise ShaderError("spirv-cross --hlsl でエラー: " + (r.stdout + r.stderr))
    if shader_model == "50":
        _export_point_size(out_path)


def _export_point_size(hlsl_path):
    # HLSL には点の大きさの出力が無いので、spirv-cross は gl_PointSize を捨てる
    # (static 変数になるだけ)。書いているシェーダーでは PSIZE で出力に足す
    # (機種側のヘッダーが機種のセマンティクスへ読み替える。PS5.hlsl は S_POINT_SIZE)
    with open(hlsl_path, encoding="utf-8") as f:
        src = f.read()
    if "static float gl_PointSize;" not in src or "gl_PointSize = " not in src:
        return
    out_struct = "struct SPIRV_Cross_Output\n{\n"
    ret = "    return stage_output;\n"
    if src.count(out_struct) != 1 or src.count(ret) != 1:
        raise ShaderError("gl_PointSize を出力に足せない (spirv-cross の出力形式が想定外)")
    src = src.replace(out_struct, out_struct + "    float gl_PointSize : PSIZE;\n")
    src = src.replace(ret, "    stage_output.gl_PointSize = gl_PointSize;\n" + ret)
    with open(hlsl_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)


def _point_coord_semantic(hlsl_path):
    with open(hlsl_path, encoding="utf-8") as f:
        src = f.read()
    old = "_kz_PointCoord : TEXCOORD%d;" % _POINT_COORD_LOCATION
    if src.count(old) != 1:
        raise ShaderError("gl_PointCoord を POINTCOORD にできない (spirv-cross の出力形式が想定外)")
    src = src.replace(old, "_kz_PointCoord : POINTCOORD;")
    with open(hlsl_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)


def _slash(path):
    return path.replace(os.sep, "/")


def compile_private(spv_path, cmd_template, point_coord=False):
    hlsl = spv_path[:-4] + ".hlsl"
    out = spv_path[:-4] + ".private"
    to_hlsl(spv_path, hlsl)
    if point_coord:
        _point_coord_semantic(hlsl)
    args = [a.replace("{in}", _slash(hlsl)).replace("{out}", _slash(out))
            for a in shlex.split(cmd_template)]
    r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.exists(out):
        with open(hlsl, encoding="utf-8") as f:
            src = f.read()
        raise ShaderError("機種のシェーダーコンパイラでエラー:\n%s\n--- HLSL ---\n%s"
                          % ((r.stdout + r.stderr).strip(), _numbered(src)))
    with open(out, "rb") as f:
        return f.read()


def root_signature(vs_ref, fs_ref):
    """SDL_GPU の D3D12 がパイプラインごとに作るルートシグネチャと同じものを文字列で返す
    (SDL_gpu_d3d12.c の D3D12_INTERNAL_CreateGraphicsRootSignature と同じ順・同じ範囲)。

    機種向けの dxc で事前コンパイルするにはシェーダーにルートシグネチャが要る。
    数はエンジンが SDL_CreateGPUShader に渡す値と揃える (サンプラーは binding の最大 + 1、
    uniform ブロックは有れば 1)。"""
    def samplers(ref):
        return max((t["binding"] + 1 for t in ref.get("textures", [])), default=0)

    parts = []
    vs_n, fs_n = samplers(vs_ref), samplers(fs_ref)
    if vs_n:
        parts.append("DescriptorTable(Sampler(s0, space=0, numDescriptors=%d), visibility=SHADER_VISIBILITY_VERTEX)" % vs_n)
        parts.append("DescriptorTable(SRV(t0, space=0, numDescriptors=%d), visibility=SHADER_VISIBILITY_VERTEX)" % vs_n)
    if vs_ref.get("ubos"):
        parts.append("CBV(b0, space=1, visibility=SHADER_VISIBILITY_VERTEX)")
    if fs_n:
        parts.append("DescriptorTable(Sampler(s0, space=2, numDescriptors=%d), visibility=SHADER_VISIBILITY_PIXEL)" % fs_n)
        parts.append("DescriptorTable(SRV(t0, space=2, numDescriptors=%d), visibility=SHADER_VISIBILITY_PIXEL)" % fs_n)
    if fs_ref.get("ubos"):
        parts.append("CBV(b0, space=3, visibility=SHADER_VISIBILITY_PIXEL)")
    return ", ".join(["RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)"] + parts)


def compile_dxil(spv_path, stage, dxc=None, rootsig=None):
    hlsl = spv_path[:-4] + ".dxil.hlsl"
    out = spv_path[:-4] + ".dxil"
    to_hlsl(spv_path, hlsl, shader_model="60")
    if rootsig:
        with open(hlsl, encoding="utf-8") as f:
            src = f.read()
        with open(hlsl, "w", encoding="utf-8", newline="\n") as f:
            f.write('#define KZ_RS "%s"\n' % rootsig + src)
    if dxc:
        if not os.path.isfile(dxc):
            raise ShaderError("dxc が見つからない: " + dxc)
    else:
        dxc = find_tool("dxc")
    profile = "vs_6_0" if stage == "vert" else "ps_6_0"
    args = [dxc, "-nologo", "-T", profile, "-E", "main", "-Fo", out]
    if rootsig:
        args += ["-rootsig-define", "KZ_RS"]
    r = subprocess.run(args + [hlsl],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0 or not os.path.exists(out):
        with open(hlsl, encoding="utf-8") as f:
            src = f.read()
        raise ShaderError("dxc でエラー:\n%s\n--- HLSL ---\n%s"
                          % ((r.stdout + r.stderr).strip(), _numbered(src)))
    with open(out, "rb") as f:
        return f.read()


def compile_program(vs_src, fs_src, label, workdir, target):
    key = program_key(vs_src, fs_src)
    vs450, fs450, _ = rewrite_pair(vs_src, fs_src, point_sprite=(target.kind == "dxil"))
    vs_spv, vs_path = compile_spirv(vs450, "vert", workdir, label)
    fs_spv, fs_path = compile_spirv(fs450, "frag", workdir, label)
    vs_ref, fs_ref = reflect(vs_path), reflect(fs_path)
    refl = reflection_text(vs_ref, fs_ref)
    c = Compiled(key, vs_spv, fs_spv, refl, vs450, fs450)
    if target.kind == "private":
        c.vs_code = compile_private(vs_path, target.vs_cmd)
        if "gl_PointCoord" in fs450:
            # HLSL には点の中の座標が無く、spirv-cross は gl_PointCoord を定数
            # (0.5, 0.5) にしてしまう。入力変数に置き換えた版を作り、HLSL で
            # POINTCOORD にする (機種側のヘッダーが読み替える。PS5.hlsl は S_POINT_COORD)
            _, pc_path = compile_spirv(_point_coord_as_input(fs450), "frag", workdir, label + "_pc")
            c.fs_code = compile_private(pc_path, target.fs_cmd, point_coord=True)
        else:
            c.fs_code = compile_private(fs_path, target.fs_cmd)
    elif target.kind == "dxil":
        rs = root_signature(vs_ref, fs_ref)
        c.vs_code = compile_dxil(vs_path, "vert", target.dxc, rs)
        c.fs_code = compile_dxil(fs_path, "frag", target.dxc, rs)
    return c


_POINT_COORD_LOCATION = 31   # ユーザーの varying と重ならない場所


def _point_coord_as_input(fs450):
    head, rest = fs450.split("\n", 1)
    return (head + "\nlayout(location = %d) in vec2 _kz_PointCoord;\n" % _POINT_COORD_LOCATION
            + rest.replace("gl_PointCoord", "_kz_PointCoord"))


def write_pack(path, programs, fmt):
    seen = {}
    for p in programs:
        seen[p.key] = p  # 同じキーは 1 つに
    with open(path, "wb") as f:
        f.write(PACK_MAGIC)
        f.write(struct.pack("<III", PACK_VERSION, fmt, len(seen)))
        for key in sorted(seen):
            p = seen[key]
            refl = p.refl.encode("utf-8")
            f.write(struct.pack("<Q", key))
            for blob in (p.vs_code, p.fs_code, refl):
                f.write(struct.pack("<I", len(blob)))
                f.write(blob)


def write_spv_dir(d, programs):
    os.makedirs(d, exist_ok=True)
    for p in programs:
        with open(os.path.join(d, "%016x.vert.spv" % p.key), "wb") as f:
            f.write(p.vs_spv)
        with open(os.path.join(d, "%016x.frag.spv" % p.key), "wb") as f:
            f.write(p.fs_spv)


# --------------------------------------------------------------------------
# 入力の収集
# --------------------------------------------------------------------------
def read_text(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read()


def collect_pairs(args, explicit_pairs=()):
    pairs = []  # (label, vs_src, fs_src)
    for vs, fs in explicit_pairs:
        pairs.append((os.path.splitext(os.path.basename(vs))[0], read_text(vs), read_text(fs)))
    for a in args:
        if a.lower().endswith(".json"):
            base = os.path.dirname(os.path.abspath(a))
            with open(a, "r", encoding="utf-8") as f:
                j = json.load(f)
            for prog in j["programs"]:
                pairs.append((prog["name"], read_text(os.path.join(base, prog["vs"])),
                              read_text(os.path.join(base, prog["fs"]))))
        elif os.path.isdir(a):
            for fn in sorted(os.listdir(a)):
                if fn.endswith(".vs"):
                    fsn = fn[:-3] + ".fs"
                    if os.path.exists(os.path.join(a, fsn)):
                        pairs.append((fn[:-3], read_text(os.path.join(a, fn)),
                                      read_text(os.path.join(a, fsn))))
        else:
            raise ShaderError("入力が読めない: " + a)
    return pairs


def compile_all(pairs, target):
    out = []
    with tempfile.TemporaryDirectory(prefix="gpushaderc_") as work:
        for i, (label, vs, fs) in enumerate(pairs):
            safe = re.sub(r"[^A-Za-z0-9_]", "_", label) or "p%d" % i
            out.append(compile_program(vs, fs, "%03d_%s" % (i, safe), work, target))
    return out


# --------------------------------------------------------------------------
# 組み込みシェーダー (C++ へ埋め込む .inc を生成)
# --------------------------------------------------------------------------
def c_string(s):
    s = normalize_text(s)
    lines = s.split("\n")
    out = []
    for i, l in enumerate(lines):
        if i == len(lines) - 1 and l == "":
            break
        esc = l.replace("\\", "\\\\").replace('"', '\\"')
        out.append('\t"%s\\n"' % esc)
    return "\n".join(out) if out else '\t""'


def write_builtin_inc(path, json_path, compiled, pairs, fmt, symbol=None):
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp_path = tmp.name
    try:
        write_pack(tmp_path, compiled, fmt)
        with open(tmp_path, "rb") as f:
            pack = f.read()
    finally:
        os.unlink(tmp_path)
    lines = [
        "// このファイルは gpushaderc.py builtin が %s から生成する。手で編集しないこと。" % os.path.basename(json_path),
        "#pragma once",
        "",
    ]
    name = symbol or "TVPGpuBuiltinShaderPack"
    if not symbol:
        for (label, vs, fs) in pairs:
            ident = re.sub(r"[^A-Za-z0-9_]", "_", label)
            lines.append("static const char TVPGpuBuiltinVS_%s[] =\n%s;" % (ident, c_string(vs)))
            lines.append("static const char TVPGpuBuiltinFS_%s[] =\n%s;" % (ident, c_string(fs)))
            lines.append("")
    lines.append("static const unsigned char %s[] = {" % name)
    for i in range(0, len(pack), 16):
        lines.append("\t" + ",".join("0x%02x" % b for b in pack[i:i + 16]) + ",")
    lines.append("};")
    lines.append("static const unsigned int %sSize = %d;" % (name, len(pack)))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# --------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--spv-dir")
    p.add_argument("--pair", nargs=2, action="append", default=[], metavar=("VS", "FS"))
    p.add_argument("inputs", nargs="*")
    b = sub.add_parser("builtin")
    b.add_argument("-o", "--output", required=True)
    b.add_argument("--spv-dir")
    b.add_argument("--symbol")
    b.add_argument("json")
    for x in (p, b):
        x.add_argument("--target", choices=("spirv", "private", "dxil"), default="spirv")
        x.add_argument("--hlsl-compile-vs")
        x.add_argument("--hlsl-compile-fs")
        x.add_argument("--dxc")
    r = sub.add_parser("rewrite")
    r.add_argument("vs")
    r.add_argument("fs")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "rewrite":
            vs450, fs450, _ = rewrite_pair(read_text(a.vs), read_text(a.fs))
            print("// ---- vert ----\n" + vs450 + "\n// ---- frag ----\n" + fs450)
            return 0
        target = Target(a.target, a.hlsl_compile_vs, a.hlsl_compile_fs, a.dxc)
        if a.cmd == "pack":
            pairs = collect_pairs(a.inputs, a.pair)
            if not pairs:
                raise ShaderError("入力がない")
            compiled = compile_all(pairs, target)
            write_pack(a.output, compiled, target.format)
        else:
            pairs = collect_pairs([a.json])
            compiled = compile_all(pairs, target)
            write_builtin_inc(a.output, a.json, compiled, pairs, target.format, a.symbol)
        if a.spv_dir:
            write_spv_dir(a.spv_dir, compiled)
        for (label, _, _), c in zip(pairs, compiled):
            print("%016x  %s" % (c.key, label))
        return 0
    except ShaderError as e:
        print("gpushaderc: " + str(e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
