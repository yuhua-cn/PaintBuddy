#include "Json.h"
#include <cmath>

namespace pb {

// ------------------------------------------------------------
// 写入器
// ------------------------------------------------------------
void JW::sep() {
    if (depth.empty()) return;
    Frame& f = depth.back();
    if (f.first) f.first = false;
    else buf += ',';
}

JW& JW::ob() { sep(); buf += '{'; push(false); return *this; }
JW& JW::oe() { buf += '}'; pop(); return *this; }
JW& JW::ab() { sep(); buf += '['; push(true);  return *this; }
JW& JW::ae() { buf += ']'; pop(); return *this; }

JW& JW::k(const char* key) {
    sep();
    escTo(buf, key ? key : "");
    buf += ':';
    // 让紧随其后的 v() 不再补逗号
    if (!depth.empty()) depth.back().first = true;
    return *this;
}

JW& JW::v(int64_t x)  { sep(); buf += std::to_string(x); return *this; }
JW& JW::v(int x)      { sep(); buf += std::to_string(x); return *this; }
JW& JW::v(uint32_t x) { sep(); buf += std::to_string(x); return *this; }
JW& JW::v(bool x)     { sep(); buf += (x ? "true" : "false"); return *this; }
JW& JW::v(const char* x) { sep(); escTo(buf, x ? x : ""); return *this; }
JW& JW::v(const std::string& x) { sep(); escTo(buf, x); return *this; }
JW& JW::raw(const std::string& s) { sep(); buf += s; return *this; }

JW& JW::v(double x) {
    sep();
    if (!std::isfinite(x)) { buf += "null"; return *this; }
    char b[48];
    // %.9g 足以无损往返 float/double 的常用精度，同时避免冗长
    snprintf(b, sizeof(b), "%.9g", x);
    buf += b;
    return *this;
}

void JW::escTo(std::string& out, const std::string& s) {
    out += '"';
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;   // UTF-8 原样透传
            }
        }
    }
    out += '"';
}

// ------------------------------------------------------------
// 解析器
// ------------------------------------------------------------
namespace {

struct Parser {
    const char* s;
    size_t i, n;
    int depth = 0;
    std::string err;
    static const int MAX_DEPTH = 128;

    void ws() {
        while (i < n) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }
    bool fail(const char* m) { err = m; err += " @"; err += std::to_string(i); return false; }

    bool hex4(unsigned& out) {
        if (i + 4 > n) return fail("bad \\u");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return fail("bad hex");
        }
        return true;
    }
    static void appendUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& out) {
        if (i >= n || s[i] != '"') return fail("expect string");
        ++i;
        out.clear();
        while (i < n) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= n) return fail("bad escape");
            char e = s[i++];
            switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                // 处理代理对
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (i + 1 < n && s[i] == '\\' && s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        unsigned lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            i = save;
                        }
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape char");
            }
        }
        return fail("unterminated string");
    }
    bool num(double& out) {
        size_t st = i;
        if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
        bool any = false;
        while (i < n && isdigit((unsigned char)s[i])) { ++i; any = true; }
        if (i < n && s[i] == '.') {
            ++i;
            while (i < n && isdigit((unsigned char)s[i])) { ++i; any = true; }
        }
        if (any && i < n && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
            while (i < n && isdigit((unsigned char)s[i])) ++i;
        }
        if (!any) return fail("bad number");
        std::string tok(s + st, i - st);
        out = strtod(tok.c_str(), nullptr);
        return true;
    }
    bool val(JVal& v) {
        ws();
        if (i >= n) return fail("eof");
        if (++depth > MAX_DEPTH) { --depth; return fail("too deep"); }
        bool ok = valInner(v);
        --depth;
        return ok;
    }
    bool valInner(JVal& v) {
        char c = s[i];
        if (c == '{') {
            ++i; v.t = JVal::Obj;
            ws();
            if (i < n && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= n || s[i] != ':') return fail("expect :");
                ++i;
                JVal child;
                if (!val(child)) return false;
                v.o.emplace_back(std::move(key), std::move(child));
                ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == '}') { ++i; return true; }
                return fail("expect , or }");
            }
        }
        if (c == '[') {
            ++i; v.t = JVal::Arr;
            ws();
            if (i < n && s[i] == ']') { ++i; return true; }
            for (;;) {
                JVal child;
                if (!val(child)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == ']') { ++i; return true; }
                return fail("expect , or ]");
            }
        }
        if (c == '"') { v.t = JVal::Str; return str(v.s); }
        if (c == 't') { if (n - i >= 4 && !memcmp(s + i, "true", 4)) { i += 4; v.t = JVal::Boo; v.b = true; return true; } return fail("bad literal"); }
        if (c == 'f') { if (n - i >= 5 && !memcmp(s + i, "false", 5)) { i += 5; v.t = JVal::Boo; v.b = false; return true; } return fail("bad literal"); }
        if (c == 'n') { if (n - i >= 4 && !memcmp(s + i, "null", 4)) { i += 4; v.t = JVal::Nul; return true; } return fail("bad literal"); }
        v.t = JVal::Num;
        return num(v.n);
    }
};

} // namespace

bool JParse(const char* text, size_t len, JVal& out, std::string& err) {
    // 跳过 UTF-8 BOM（EF BB BF）。
    //
    // 为什么必须处理：覆写文件 pen_overrides.json 与配置 config.json 都可能被
    // 用户用记事本编辑后保存，而 Windows 记事本与 PowerShell 的
    // `Set-Content -Encoding UTF8` 都会在开头写入 BOM。解析器遇到 BOM 会在
    // 读到 '{' 之前就失败——本机实机验证正是这样抓到的（离线测试直接传
    // 字符串字面量、无 BOM，因此覆盖不到这条路径）。
    //
    // 对合法 JSON 绝对安全：合法 JSON 的首字符必然是 '{'、'['、'"'、数字或
    // true/false/null 之一，不可能以 EF BB BF 开头，故跳过 BOM 不会误伤任何
    // 有效输入。
    if (len >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text += 3;
        len  -= 3;
    }
    Parser p{text, 0, len, 0, std::string()};
    out = JVal();
    if (!p.val(out)) { err = p.err; return false; }
    p.ws();
    if (p.i != p.n) { err = "trailing garbage @" + std::to_string(p.i); return false; }
    err.clear();
    return true;
}

} // namespace pb
