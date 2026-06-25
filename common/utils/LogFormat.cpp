//---------------------------------------------------------------------------
// tvpfmt::vformat 実装
//
// ログ用途に限定した最小の {} 書式整形器。fmtlib / <format> への依存を
// 完全に断つための自前実装 (LogIntf.h のコメント参照)。
//
// LogCore の有効/無効に関わらず必要なため、別ファイルに分離して常にリンク。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "LogIntf.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// spec = 波括弧の中身 (':' は既に剥がしてある)
// buf には %…形式の printf フォーマット文字列を組み立てる
static void format_one(std::string& out, const tvpfmt::detail::FormatArg& a,
                       const char *spec_begin, const char *spec_end)
{
    using K = tvpfmt::detail::FormatArg;

    bool zero = false;
    int  width = 0;
    char type  = 0;

    const char *p = spec_begin;
    if (p < spec_end && *p == '0') { zero = true; ++p; }
    while (p < spec_end && *p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); ++p; }
    if (p < spec_end) type = *p++;
    // 残りは無視 (精度/アライン等は非対応)

    auto build_int_fmt = [&](char conv) {
        // 例: "%08llx", "%lld", "%5llu"
        std::string f = "%";
        if (zero)      f += '0';
        if (width > 0) { char wb[16]; std::snprintf(wb, sizeof(wb), "%d", width); f += wb; }
        f += "ll";
        f += conv;
        return f;
    };

    char buf[128];

    switch (a.kind) {
    case K::K_S64: {
        char conv = 'd';
        bool is_unsigned = false;
        if      (type == 'x') { conv = 'x'; is_unsigned = true; }
        else if (type == 'X') { conv = 'X'; is_unsigned = true; }
        else if (type == 'o') { conv = 'o'; is_unsigned = true; }
        else if (type == 'u') { conv = 'u'; is_unsigned = true; }
        // type == 0, 'd', 'i' → 10 進符号付き
        std::string f = build_int_fmt(conv);
        if (is_unsigned)
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<unsigned long long>(a.i64));
        else
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<long long>(a.i64));
        out.append(buf);
        break;
    }
    case K::K_U64:
    case K::K_BOOL: {
        char conv = 'u';
        if      (type == 'x') conv = 'x';
        else if (type == 'X') conv = 'X';
        else if (type == 'o') conv = 'o';
        else if (type == 'd' || type == 'i') conv = 'd';
        std::string f = build_int_fmt(conv);
        if (conv == 'd')
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<long long>(a.u64));
        else
            std::snprintf(buf, sizeof(buf), f.c_str(), static_cast<unsigned long long>(a.u64));
        out.append(buf);
        break;
    }
    case K::K_DBL: {
        // {}, {:f}, {:g}, {:e}, {:.Nf} など最低限。精度指定は上記の簡易 parser では
        // 拾えていないので、ここでは常に '%g' 既定 ('f'/'e'/'g'/'G' が明示されたら従う)。
        char conv = 'g';
        if (type == 'f' || type == 'F' || type == 'e' || type == 'E' || type == 'g' || type == 'G')
            conv = type;
        char f[16];
        if (width > 0)
            std::snprintf(f, sizeof(f), "%%%s%d%c", zero ? "0" : "", width, conv);
        else
            std::snprintf(f, sizeof(f), "%%%c", conv);
        std::snprintf(buf, sizeof(buf), f, a.dbl);
        out.append(buf);
        break;
    }
    case K::K_PTR:
        std::snprintf(buf, sizeof(buf), "%p", a.ptr);
        out.append(buf);
        break;
    case K::K_CSTR:
        out.append(a.cstr ? a.cstr : "(null)");
        break;
    case K::K_STR:
        out.append(a.str);
        break;
    case K::K_NONE:
        break;
    }
}

} // namespace

namespace tvpfmt {

std::string vformat(const char *fmt_, const format_args& fa)
{
    std::string out;
    if (!fmt_) return out;

    size_t idx = 0;
    const char *p = fmt_;
    while (*p) {
        char c = *p;
        if (c == '{') {
            if (p[1] == '{') { out.push_back('{'); p += 2; continue; }
            const char *end = std::strchr(p, '}');
            if (!end) throw format_error("tvpfmt: unterminated '{' in format string");
            const char *spec_begin = p + 1;
            if (spec_begin < end && *spec_begin == ':') ++spec_begin;
            // 位置指定 ({0}, {1}) は未対応 — 常に順次割り当て
            if (idx >= fa.args.size())
                throw format_error("tvpfmt: argument index out of range");
            format_one(out, fa.args[idx++], spec_begin, end);
            p = end + 1;
        } else if (c == '}') {
            if (p[1] == '}') { out.push_back('}'); p += 2; continue; }
            throw format_error("tvpfmt: unmatched '}' in format string");
        } else {
            out.push_back(c);
            ++p;
        }
    }
    return out;
}

} // namespace tvpfmt
