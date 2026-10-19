#include "PenOverrides.h"
#include "Json.h"
#include "Store.h"

namespace pb {

// ------------------------------------------------------------
// 匹配
// ------------------------------------------------------------
bool PenOverride::Matches(DWORD devVid, DWORD devPid, USHORT devUsage,
                          const std::string& devPathLower) const {
    // 所有「已指定」的条件都必须满足（AND 关系）。
    // 未指定的条件视为通配 —— 但至少得指定一个，否则一条规则会命中所有设备
    // （见 ParseOverrides 的校验）。
    if (hasVid   && vid   != devVid)  return false;
    if (hasPid   && pid   != devPid)  return false;
    if (hasUsage && usage != devUsage) return false;
    if (!pathContains.empty() &&
        devPathLower.find(pathContains) == std::string::npos) return false;
    return true;
}

// ------------------------------------------------------------
// 解析辅助
// ------------------------------------------------------------
// vid/pid 既接受数字（1386）也接受十六进制字符串（"056A"）——
// 设备管理器与诊断日志都用十六进制显示，直接抄过来最不容易错。
static bool ReadHexOrNum(const JVal& j, const char* key, bool& present, DWORD& out) {
    const JVal* v = j.find(key);
    if (!v) { present = false; return true; }
    present = true;
    if (v->t == JVal::Num) { out = (DWORD)v->asI64(); return true; }
    if (v->t == JVal::Str) {
        const std::string& s = v->s;
        if (s.empty()) return false;
        const char* p = s.c_str();
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        char* end = nullptr;
        unsigned long val = strtoul(p, &end, 16);
        if (end == p || val > 0xFFFF) return false;   // 空串或超范围
        out = (DWORD)val;
        return true;
    }
    return false;   // 类型不对
}

static bool ReadLong(const JVal& j, const char* key, bool& present, LONG& out) {
    const JVal* v = j.find(key);
    if (!v) { present = false; return true; }
    if (v->t != JVal::Num) { present = false; return false; }
    present = true;
    out = (LONG)v->asI64();
    return true;
}

static bool ReadUShort(const JVal& j, const char* key, bool& present, USHORT& out) {
    const JVal* v = j.find(key);
    if (!v) { present = false; return true; }
    if (v->t == JVal::Num) {
        int64_t n = v->asI64();
        if (n < 0 || n > 0xFFFF) { present = false; return false; }
        present = true; out = (USHORT)n; return true;
    }
    if (v->t == JVal::Str) {          // usage 允许写 "0x01" 或 "0D:01" 的尾段
        std::string s = ToLowerA(v->s);
        size_t colon = s.rfind(':');
        if (colon != std::string::npos) s = s.substr(colon + 1);
        const char* p = s.c_str();
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        char* end = nullptr;
        unsigned long val = strtoul(p, &end, 16);
        if (end == p || val > 0xFFFF) { present = false; return false; }
        present = true; out = (USHORT)val; return true;
    }
    present = false;
    return false;
}

bool ParseOverrides(const std::string& jsonText,
                    std::vector<PenOverride>& out,
                    std::vector<std::string>& errors) {
    out.clear();
    errors.clear();

    // 空文本 / 空文件视为「没有覆写」，不算错误
    if (jsonText.empty()) return true;

    JVal root; std::string jerr;
    if (!JParse(jsonText, root, jerr)) {
        errors.push_back("JSON 解析失败: " + jerr);
        return false;
    }

    const JVal* arr = root.arr("overrides");
    if (!arr) {
        // 顶层没有 overrides 数组：可能用户把规则直接写在了顶层，给出提示
        if (root.t == JVal::Obj && !root.o.empty()) {
            errors.push_back("缺少 \"overrides\" 数组（顶层第一个键是 \"" +
                             root.o[0].first + "\"）");
        }
        return true;
    }

    for (size_t i = 0; i < arr->a.size(); ++i) {
        const JVal& j = arr->a[i];
        char idx[32];
        snprintf(idx, sizeof(idx), "#%zu", i + 1);

        if (j.t != JVal::Obj) {
            errors.push_back(std::string(idx) + " 不是对象，已跳过");
            continue;
        }

        PenOverride o;
        bool bad = false;

        // ---- 匹配键 ----
        if (!ReadHexOrNum(j, "vid", o.hasVid, o.vid)) {
            errors.push_back(std::string(idx) + " vid 非法（需 0..65535 的数字或十六进制字符串），已跳过");
            bad = true;
        }
        if (!ReadHexOrNum(j, "pid", o.hasPid, o.pid)) {
            errors.push_back(std::string(idx) + " pid 非法（需 0..65535 的数字或十六进制字符串），已跳过");
            bad = true;
        }
        if (!ReadUShort(j, "usage", o.hasUsage, o.usage)) {
            errors.push_back(std::string(idx) + " usage 非法，已跳过");
            bad = true;
        }
        if (const JVal* v = j.find("pathContains")) {
            if (v->t != JVal::Str) {
                errors.push_back(std::string(idx) + " pathContains 需为字符串，已跳过");
                bad = true;
            } else {
                o.pathContains = ToLowerA(v->s);
            }
        }

        // 至少要有一个匹配条件。否则这条规则会命中所有设备 ——
        // 那是配置事故（比如把全局量程改坏），必须拒绝。
        if (!bad && !o.hasVid && !o.hasPid && !o.hasUsage && o.pathContains.empty()) {
            errors.push_back(std::string(idx) +
                             " 缺少匹配条件（至少需 vid / pid / usage / pathContains 之一），"
                             "否则会命中所有设备，已跳过");
            bad = true;
        }

        // ---- 覆写字段 ----
        bool hasLXMin = false, hasLXMax = false, hasLYMin = false, hasLYMax = false;
        LONG lxMin = 0, lxMax = 0, lyMin = 0, lyMax = 0;
        if (!ReadLong(j, "logicalXMin", hasLXMin, lxMin)) {
            errors.push_back(std::string(idx) + " logicalXMin 需为数字，已跳过"); bad = true;
        }
        if (!ReadLong(j, "logicalXMax", hasLXMax, lxMax)) {
            errors.push_back(std::string(idx) + " logicalXMax 需为数字，已跳过"); bad = true;
        }
        if (!ReadLong(j, "logicalYMin", hasLYMin, lyMin)) {
            errors.push_back(std::string(idx) + " logicalYMin 需为数字，已跳过"); bad = true;
        }
        if (!ReadLong(j, "logicalYMax", hasLYMax, lyMax)) {
            errors.push_back(std::string(idx) + " logicalYMax 需为数字，已跳过"); bad = true;
        }
        // 四个值必须同时给全，否则量程不完整、无法建立映射
        if (!bad) {
            bool none = !hasLXMin && !hasLXMax && !hasLYMin && !hasLYMax;
            bool all  =  hasLXMin &&  hasLXMax &&  hasLYMin &&  hasLYMax;
            if (!none && !all) {
                errors.push_back(std::string(idx) +
                                 " 逻辑量程四个值必须同时给全或全部省略，已跳过");
                bad = true;
            } else if (all) {
                // 关键校验：非法量程会让映射系数变成 0 或负数、甚至除零，
                // 把设备彻底搞坏。宁可拒绝这条覆写也要保住设备可用。
                if (lxMax <= lxMin || lyMax <= lyMin) {
                    char b[160];
                    snprintf(b, sizeof(b),
                             "logicalX/YMax 必须大于对应 Min（收到 X[%ld..%ld] Y[%ld..%ld]），已跳过",
                             (long)lxMin, (long)lxMax, (long)lyMin, (long)lyMax);
                    errors.push_back(std::string(idx) + " " + b);
                    bad = true;
                } else {
                    o.hasLogical = true;
                    o.lxMin = lxMin; o.lxMax = lxMax; o.lyMin = lyMin; o.lyMax = lyMax;
                }
            }
        }

        if (!ReadLong(j, "pressureMax", o.hasPressureMax, o.pressureMax)) {
            errors.push_back(std::string(idx) + " pressureMax 需为数字，已跳过"); bad = true;
        } else if (o.hasPressureMax && o.pressureMax <= 0) {
            errors.push_back(std::string(idx) + " pressureMax 必须 > 0（否则归一会除零），已跳过");
            o.hasPressureMax = false; bad = true;
        }

        if (!ReadUShort(j, "penLink", o.hasPenLink, o.penLink)) {
            errors.push_back(std::string(idx) + " penLink 需为 0..65535 的数字，已跳过"); bad = true;
        }

        if (const JVal* v = j.find("deviceType")) {
            if (v->t == JVal::Str) {
                std::string s = ToLowerA(v->s);
                if (s == "external" || s == "tablet" || s == "digitizer") o.deviceType = PEN_EXTERNAL;
                else if (s == "integrated" || s == "display" || s == "screen") o.deviceType = PEN_INTEGRATED;
                else if (s == "touch" || s == "touchscreen") o.deviceType = PEN_TOUCH_SCREEN;
                else {
                    errors.push_back(std::string(idx) +
                        " deviceType 未知（可用 external / integrated / touch），已跳过");
                    bad = true;
                }
                if (!bad) o.hasDeviceType = true;
            } else if (v->t == JVal::Num) {
                int t = v->asI32();
                if (t == PEN_EXTERNAL || t == PEN_INTEGRATED || t == PEN_TOUCH_SCREEN) {
                    o.deviceType = (uint8_t)t; o.hasDeviceType = true;
                } else {
                    errors.push_back(std::string(idx) + " deviceType 需为 1/2/4，已跳过");
                    bad = true;
                }
            } else {
                errors.push_back(std::string(idx) + " deviceType 需为字符串或数字，已跳过");
                bad = true;
            }
        }

        if (const JVal* v = j.find("displayName")) {
            if (v->t != JVal::Str || v->s.empty()) {
                errors.push_back(std::string(idx) + " displayName 需为非空字符串，已跳过");
                bad = true;
            } else {
                o.hasDisplayName = true;
                o.displayName = v->s;   // 保留原始大小写与反斜杠
            }
        }
        {
            const JVal* v = j.find("displayIndex");
            if (v) {
                if (v->t != JVal::Num || v->asI32() < 0) {
                    errors.push_back(std::string(idx) + " displayIndex 需为非负整数，已跳过");
                    bad = true;
                } else {
                    o.hasDisplayIndex = true;
                    o.displayIndex = v->asI32();
                }
            }
        }

        if (bad) continue;             // 该条被拒绝，继续解析其余规则
        out.push_back(o);
    }
    return true;
}

std::vector<PenOverride> LoadOverrides(std::vector<std::string>& errors) {
    std::vector<PenOverride> out;
    errors.clear();

    std::string path = DataDir();
    if (path.empty()) return out;      // 测试环境无数据目录：视为没有覆写
    path += "\\pen_overrides.json";

    std::string text;
    if (!ReadWholeFile(path, text)) return out;   // 文件不存在是正常情况

    std::vector<PenOverride> parsed;
    if (!ParseOverrides(text, parsed, errors)) {
        LogF("[pen] override 文件解析失败 %s", path.c_str());
        return out;
    }
    for (const auto& e : errors)
        LogF("[pen] override WARN: %s（该条已忽略，设备仍按自动路径工作）", e.c_str());
    if (!parsed.empty())
        LogF("[pen] override 已加载 %zu 条规则 <- %s", parsed.size(), path.c_str());
    out.swap(parsed);
    return out;
}

// ------------------------------------------------------------
// 显示器解析
// ------------------------------------------------------------
struct MonEnumCtx {
    std::vector<std::string> names;
    std::vector<RECT>        rects;
};

static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    MonEnumCtx* ctx = (MonEnumCtx*)lp;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        ctx->names.push_back(W2U(mi.szDevice));
        ctx->rects.push_back(mi.rcMonitor);
    }
    return TRUE;
}

static void EnumMonitors(MonEnumCtx& ctx) {
    ctx.names.clear(); ctx.rects.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&ctx);
}

std::vector<std::string> EnumDisplayNames() {
    MonEnumCtx ctx;
    EnumMonitors(ctx);
    return ctx.names;
}

bool ResolveDisplayRect(const PenOverride& o, RECT& outRect) {
    MonEnumCtx ctx;
    EnumMonitors(ctx);
    if (ctx.names.empty()) return false;

    // 名称优先：稳定、可读，且日志里会把可用名称列给用户。
    // 比较时忽略大小写与开头的 "\\\\.\\"（用户常写成 DISPLAY1）。
    if (o.hasDisplayName) {
        std::string want = ToLowerA(o.displayName);
        // 去掉反斜杠与点前缀，只留设备名主体
        std::string wantKey;
        for (char c : want) if (c != '\\' && c != '.' && c != '?' && c != '/') wantKey += c;
        for (size_t i = 0; i < ctx.names.size(); ++i) {
            std::string key;
            for (char c : ToLowerA(ctx.names[i]))
                if (c != '\\' && c != '.' && c != '?' && c != '/') key += c;
            if (key == wantKey) { outRect = ctx.rects[i]; return true; }
        }
        LogF("[pen] override WARN: displayName=\"%s\" 未找到匹配的显示器", o.displayName.c_str());
        // 名称没命中就不要再退回索引：用户既然写了名称，说明有明确意图，
        // 静默改用索引会映射到意料之外的屏幕，宁可失败让他去改配置。
        return false;
    }

    if (o.hasDisplayIndex) {
        if (o.displayIndex < 0 || (size_t)o.displayIndex >= ctx.rects.size()) {
            LogF("[pen] override WARN: displayIndex=%d 越界（共 %zu 个显示器）",
                 o.displayIndex, ctx.rects.size());
            return false;
        }
        outRect = ctx.rects[(size_t)o.displayIndex];
        return true;
    }
    return false;
}

} // namespace pb
