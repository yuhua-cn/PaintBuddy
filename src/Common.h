#pragma once
// ============================================================
//  绘画伴侣 (Painting Buddy) — 公共定义 / 时间基准 / 核心数据结构
//  纯 Win32 + Direct2D 实现，无第三方依赖
// ============================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace pb {

// ------------------------------------------------------------
// 操作分类：用户定义的「四种操作」
//   Normal —— 普通操作（键盘按键、鼠标）
//   Undo   —— 特殊操作（撤回 Ctrl+Z）
//   Redo   —— 特殊操作（重做 Ctrl+Y / Ctrl+Shift+Z）
//   Brush  —— 画笔操作（数位笔）
// ------------------------------------------------------------
enum class OpKind : uint8_t {
    Normal = 0,
    Undo   = 1,
    Redo   = 2,
    Brush  = 3,
};

inline const char* OpKeyName(OpKind k) {
    switch (k) {
    case OpKind::Undo:  return "undo";
    case OpKind::Redo:  return "redo";
    case OpKind::Brush: return "brush";
    default:            return "normal";
    }
}
inline OpKind OpKindFromName(const std::string& s) {
    if (s == "undo")  return OpKind::Undo;
    if (s == "redo")  return OpKind::Redo;
    if (s == "brush") return OpKind::Brush;
    return OpKind::Normal;
}

// 修饰键位掩码。
// 注意：不能沿用 MOD_SHIFT / MOD_ALT / MOD_WIN 这类裸名字 ——
// WinUser.h 中 RegisterHotKey 已把它们定义为宏，会导致本枚举被宏
// 展开而语法崩坏（整份头文件连锁报错）。统一加 PB_ 前缀规避。
enum PbMod : uint8_t {
    PB_MOD_NONE  = 0,
    PB_MOD_CTRL  = 1 << 0,
    PB_MOD_SHIFT = 1 << 1,
    PB_MOD_ALT   = 1 << 2,
    PB_MOD_WIN   = 1 << 3,
};

// 数位笔设备类型（HID Digitizer Usage Page 0x0D）
enum PenDevice : uint8_t {
    PEN_UNKNOWN        = 0,
    PEN_EXTERNAL       = 0x01,  // External Pen / 数位板
    PEN_INTEGRATED     = 0x02,  // Integrated Pen / 数位屏
    PEN_TOUCH_SCREEN   = 0x04,  // Touch Screen
    PEN_TOUCH_PAD      = 0x08,  // Touch Pad / 笔记本触控板（非绘图设备，不参与笔映射）
};

// ------------------------------------------------------------
// 高精度时钟：QPC 计数 + 启动时用 GetSystemTimePreciseAsFileTime 校准到
// Unix epoch 微秒。既保证单调高精度，又保证时间戳可读、可跨天分析。
// ------------------------------------------------------------
class Clock {
public:
    static void Init() {
        LARGE_INTEGER f, q;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q);
        s_freq = f.QuadPart ? f.QuadPart : 1;
        s_baseQpc = q.QuadPart;
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        s_baseEpochUs = FileTimeToUs(ft);
    }
    // 当前 Unix epoch 微秒
    static int64_t NowUs() {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        return s_baseEpochUs + (q.QuadPart - s_baseQpc) * 1000000LL / s_freq;
    }
    static int64_t NowMs() { return NowUs() / 1000; }
    // 重新校准（系统时间被用户改动 / 睡眠唤醒后）
    static void Resync() {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        int64_t nowEp = FileTimeToUs(ft);
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        s_baseQpc = q.QuadPart;
        s_baseEpochUs = nowEp;
    }
    static int64_t FileTimeToUs(const FILETIME& ft) {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        // FILETIME 单位 100ns，起点 1601-01-01；转为 1970 epoch 微秒
        return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10ULL);
    }

private:
    static inline int64_t s_freq = 1;
    static inline int64_t s_baseQpc = 0;
    static inline int64_t s_baseEpochUs = 0;
};

// ------------------------------------------------------------
// 时间格式化辅助
// ------------------------------------------------------------
inline std::string UsToLocalStr(int64_t us, const char* fmt = "%Y-%m-%d %H:%M:%S") {
    time_t sec = (time_t)(us / 1000000LL);
    struct tm lt;
    localtime_s(&lt, &sec);
    char buf[64];
    strftime(buf, sizeof(buf), fmt, &lt);
    return buf;
}
inline std::string UsToCompact(int64_t us) {
    return UsToLocalStr(us, "%Y%m%d_%H%M%S");
}
// 当天 00:00:00 的 epoch 微秒
inline int64_t DayStartUs(int64_t us) {
    time_t sec = (time_t)(us / 1000000LL);
    struct tm lt;
    localtime_s(&lt, &sec);
    lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
    time_t t0 = mktime(&lt);
    return (int64_t)t0 * 1000000LL;
}

// ------------------------------------------------------------
// 字符串 / 编码辅助
// ------------------------------------------------------------
inline std::string W2U(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
inline std::string W2U(const std::wstring& w) { return W2U(w.c_str()); }
inline std::wstring U2W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string ToLowerA(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
inline bool EndsWithI(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    return _stricmp(s.c_str() + (s.size() - suf.size()), suf.c_str()) == 0;
}
// 去除 Windows 文件名非法字符，用于「文件名由前台窗口名称得出」
inline std::wstring SanitizeFileName(const std::wstring& name, size_t maxLen = 80) {
    static const wchar_t* bad = L"\\/:*?\"<>|\r\n\t";
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        if (wcschr(bad, c)) out += L'_';
        else if ((unsigned short)c < 0x20) out += L'_';
        else out += c;
    }
    // 去首尾空白与点
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    size_t start = 0;
    while (start < out.size() && out[start] == L' ') ++start;
    out = out.substr(start);
    if (out.size() > maxLen) out.resize(maxLen);
    if (out.empty()) out = L"未命名窗口";
    return out;
}

// 取进程完整映像名（仅文件名部分，UTF-8 小写）
inline std::string GetProcessExeName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::string();
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD len = (DWORD)(sizeof(buf) / sizeof(wchar_t));
    std::string result;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        std::wstring full(buf, len);
        size_t p = full.find_last_of(L"\\/");
        result = W2U(p == std::wstring::npos ? full : full.substr(p + 1));
    }
    CloseHandle(h);
    return result;
}

// ------------------------------------------------------------
// 核心数据结构：落盘前的聚合结果
// ------------------------------------------------------------

// 轨迹采样点（相对上一点的时间差，节省空间）
struct PathPoint {
    int32_t dtUs = 0;     // 距上一点的时间（微秒）
    int32_t x = 0, y = 0; // 屏幕绝对坐标
    float   speed = 0.f;  // 像素/秒
};

// 数位笔采样点
struct PenPoint {
    int32_t dtUs = 0;
    int32_t x = 0, y = 0;
    float   speed = 0.f;    // 像素/秒
    float   pressure = 0.f; // 归一化 0..1
    uint8_t flags = 0;      // bit0=tipDown bit1=barrel bit2=eraser bit3=inRange
};
enum PenPointFlag : uint8_t {
    PENF_TIP    = 1 << 0,
    PENF_BARREL = 1 << 1,
    PENF_ERASER = 1 << 2,
    PENF_INRANGE= 1 << 3,
};

// 一次键盘操作（0.8s 内连续相同键已合并）
struct KeyEvent {
    int64_t tUs = 0;      // 首次按下
    int64_t endUs = 0;    // 最后一次按下（合并区间结束）
    int     vk = 0;
    uint8_t mods = 0;
    int32_t count = 1;    // 合并计数
    OpKind  kind = OpKind::Normal;
    bool    penActive = false; // 触发时数位笔是否正在按笔
};

// 一次鼠标笔画（左键按下 → 抬起）或单次点击
struct MouseStroke {
    int64_t downUs = 0, upUs = 0;
    int32_t button = 0;   // 0=left 1=right 2=middle
    int32_t startX = 0, startY = 0, endX = 0, endY = 0;
    float   pathLen = 0.f;
    std::vector<PathPoint> pts;
    OpKind  kind = OpKind::Normal;
};

// 一次数位笔笔画（hover=true 表示悬空移动段，非落笔）
struct PenStrokeRec {
    int64_t downUs = 0, upUs = 0;
    uint8_t device = PEN_UNKNOWN;   // PenDevice
    float   maxPressure = 0.f, avgPressure = 0.f;
    float   maxSpeed = 0.f, avgSpeed = 0.f;
    float   pathLen = 0.f;
    bool    eraser = false;
    bool    hover = false;
    bool    keyboardAttributed = false; // 落笔瞬间有键按下 → 整次记为键盘操作
    OpKind  kind = OpKind::Brush;
    std::vector<PenPoint> pts;
};

// 前台应用变化
struct FocusEvent {
    int64_t tUs = 0;
    std::string exe;      // 进程名（小写）
    std::string title;    // 窗口标题（UTF-8）
    bool    isTarget = false; // 是否为受监控的绘画软件
};

// 统一的「操作」原子 —— 分析层只关心这个
// 所有类型都归一为 [tUs, endUs] 区间 + OpKind
struct OpAtom {
    int64_t tUs = 0, endUs = 0;
    OpKind  kind = OpKind::Normal;
    uint8_t src = 0;        // 0=键盘 1=鼠标 2=数位笔
    float   intensity = 0.f;// 笔画长度 / 按键次数等强度指标
    std::string note;       // 附加信息（键名、窗口名等）
};

// ------------------------------------------------------------
// 落盘的原始事件（JSONL 一行一个）
// ------------------------------------------------------------
struct RawRecord {
    int64_t tUs = 0;
    uint8_t type = 0;       // 'k' 'm' 'M' 'p' 'f' '#'
    OpKind  kind = OpKind::Normal;
    // 键盘
    int     vk = 0;
    uint8_t mods = 0;
    int32_t count = 1;
    // 鼠标 / 笔
    int64_t endUs = 0;
    int32_t button = 0;
    uint8_t device = 0;       // PenDevice 类型（0x01 外接板 / 0x02 数位屏 / 0x04 触摸屏）
    uint32_t deviceId = 0;    // 设备序号（进程内，见 Dev::devSeq）；0=未知/旧日志
    bool    eraser = false;
    bool    kbAttr = false;
    bool    hover = false;
    float   maxPressure = 0.f, avgPressure = 0.f;
    float   maxSpeed = 0.f, avgSpeed = 0.f, pathLen = 0.f;
    std::vector<PathPoint> mpts;
    std::vector<PenPoint>  ppts;
    // 前台
    std::string exe, title;
    bool    isTarget = false;
};

// ------------------------------------------------------------
// 目录 / 路径
// ------------------------------------------------------------
std::string DataDir();      // 数据根目录（UTF-8）
std::wstring DataDirW();
std::string LogDir();       // JSONL 日志目录
std::wstring LogDirW();
std::string VideoDir();     // 录屏输出目录
std::wstring VideoDirW();
std::string ConfigPath();   // 配置文件
void        EnsureDirs();

// 按用户配置覆盖录屏缓存目录（配置为空时保持 EnsureDirs 的默认值）。
// 必须在 EnsureDirs 之后、且每次配置变更后调用；目录不存在则创建，
// 创建失败时保留原值并写日志（避免把录屏写到无效路径上）。
void        ApplyVideoDirOverride(const std::string& dirUtf8);

// 取用户配置的导出目录（为空时返回空串，调用方回退到系统「视频」库）
std::wstring ConfiguredExportDir();

// 仅供开发/测试工具使用：把数据目录整体重定向到隔离位置。
// 合成数据生成器与离屏预览工具必须调用本函数，绝不能写用户真实数据目录
// （%LOCALAPPDATA%\PaintingBuddy）—— 否则合成记录会与真实绘画记录混在
// 同一个 .jsonl 里，只能靠时间戳逐条甄别才能清除。
// 生产程序（main.cpp）不调用本函数，仍走 EnsureDirs()。
void        OverrideDataDir(const std::string& baseUtf8);

// 开发工具用的合成数据隔离目录：读环境变量 PB_SAMPLE_DIR，
// 未设置时默认 build\sampledata（构建脚本从项目根运行）。
// 三个开发工具（gen_sample / render_preview / ui_preview）共用，
// 避免各自重复同一段逻辑、也避免 getenv 的弃用警告散落多处。
std::string SampleDataDir();

// ------------------------------------------------------------
// DPI 取值：全项目唯一入口
//
// 此前 App::Run / App::OnCreate / Painter::Create 各自写了一遍
// GetDeviceCaps(LOGPIXELSX)，三处口径可能不一致 —— 而布局与渲染必须
// 用同一个 DPI，否则界面会整体错位。统一收敛到这里。
//
// 为什么优先 GetDpiForWindow：进程是 Per-Monitor V2 感知，窗口可能落在
// 与主显示器缩放不同的屏上；GetDC(hwnd)+LOGPIXELSX 在部分配置下返回的是
// 系统（主显示器）DPI，而非窗口所在显示器的 DPI，正是「换台机器就错位」
// 的典型来源。GetDpiForWindow 直接给出该窗口的真实 DPI。
// 用 GetProcAddress 动态取，避免依赖特定 NTDDI_VERSION 的头文件声明，
// 与 main.cpp 里 SetProcessDpiAwarenessContext 的做法一致。
// ------------------------------------------------------------
inline int WindowDpi(HWND hwnd) {
    if (hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn fn = (GetDpiForWindowFn)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (fn) {
            UINT d = fn(hwnd);
            if (d > 0) return (int)d;
        }
        HDC dc = GetDC(hwnd);
        if (dc) {
            int lx = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(hwnd, dc);
            if (lx > 0) return lx;
        }
    }
    // 无窗口（启动阶段）或上面都失败：退回屏幕 DPI
    HDC dc = GetDC(nullptr);
    if (dc) {
        int lx = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (lx > 0) return lx;
    }
    return 96;
}

// 日志（诊断用，写入 DataDir/pb.log）
void LogF(const char* fmt, ...);

} // namespace pb
