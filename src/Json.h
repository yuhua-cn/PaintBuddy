#pragma once
// ============================================================
//  极简 JSON：写入器（流式拼接）+ 解析器（DOM）
//  仅覆盖本项目所需子集，无第三方依赖
// ============================================================
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace pb {

struct JVal {
    enum T { Nul, Boo, Num, Str, Arr, Obj } t = Nul;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* find(const char* key) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // 按键取值（找不到则返回默认值）
    double      num(const char* key, double def = 0) const { const JVal* v = find(key); return v ? v->asNum(def) : def; }
    int64_t     i64(const char* key, int64_t def = 0) const { const JVal* v = find(key); return v ? v->asI64(def) : def; }
    int         i32(const char* key, int def = 0) const { const JVal* v = find(key); return v ? v->asI32(def) : def; }
    std::string str(const char* key, const char* def = "") const { const JVal* v = find(key); return v ? v->asStr(def) : std::string(def); }
    bool        boo(const char* key, bool def = false) const { const JVal* v = find(key); return v ? v->asBool(def) : def; }
    const JVal* arr(const char* key) const { const JVal* v = find(key); return (v && v->t == Arr) ? v : nullptr; }
    const JVal* obj(const char* key) const { const JVal* v = find(key); return (v && v->t == Obj) ? v : nullptr; }

    // 取自身值。刻意用 as* 命名而非重载：
    // 两组重载都可零参调用，MSVC 会判定为二义性（C2668）
    double      asNum(double def = 0) const { return (t == Num) ? n : def; }
    int64_t     asI64(int64_t def = 0) const { return (t == Num) ? (int64_t)n : def; }
    int         asI32(int def = 0) const { return (t == Num) ? (int)n : def; }
    std::string asStr(const char* def = "") const { return (t == Str) ? s : std::string(def); }
    bool        asBool(bool def = false) const { return (t == Boo) ? b : def; }
};

// 解析。成功返回 true。深度上限防止恶意/损坏数据爆栈。
bool JParse(const char* text, size_t len, JVal& out, std::string& err);
inline bool JParse(const std::string& text, JVal& out, std::string& err) {
    return JParse(text.data(), text.size(), out, err);
}

// 流式写入器
class JW {
public:
    JW& ob();                       // { 开始对象
    JW& oe();                       // } 结束对象
    JW& ab();                       // [ 开始数组
    JW& ae();                       // ] 结束数组
    JW& k(const char* key);         // "key":
    // 注意：Win64 上 int32_t 就是 int，不能同时声明两个重载
    JW& v(int64_t x);
    JW& v(int x);
    JW& v(uint32_t x);
    JW& v(double x);
    JW& v(bool x);
    JW& v(const std::string& x);
    JW& v(const char* x);
    JW& raw(const std::string& preEscaped); // 已转义内容直接写入

    JW& kv(const char* key, int64_t x) { return k(key).v(x); }
    JW& kv(const char* key, int x) { return k(key).v(x); }
    JW& kv(const char* key, double x) { return k(key).v(x); }
    JW& kv(const char* key, bool x) { return k(key).v(x); }
    JW& kv(const char* key, const std::string& x) { return k(key).v(x); }
    JW& kv(const char* key, const char* x) { return k(key).v(x); }

    std::string take() { std::string r; r.swap(buf); depth.clear(); return r; }
    const std::string& str() const { return buf; }
    void clear() { buf.clear(); depth.clear(); }

private:
    struct Frame { bool isArr; bool first; };
    std::vector<Frame> depth;
    std::string buf;
    void sep();
    void push(bool isArr) { depth.push_back({isArr, true}); }
    void pop() { if (!depth.empty()) depth.pop_back(); }
    static void escTo(std::string& out, const std::string& s);
};

} // namespace pb
