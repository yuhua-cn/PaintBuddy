// ============================================================
//  Win32 API 垫片的 macOS 实现
//
//  对应 mac/compat/windows.h 中声明的全部函数。
//  这些函数只被「原样复用」的 Windows 端头文件（Common.h 的 inline
//  函数、Store.h 的类型）间接调用；Windows 端的 .cpp 实现在 macOS
//  构建中一律不参与编译，由 mac/platform/*.mm 重写替代。
//
//  ⚠️ 两处必须与 Windows 端语义严格一致，否则会静默产生错误数据：
//    1. W2U / U2W 的编码转换 —— macOS 的 wchar_t 是 32 位 UTF-32，
//       不是 Windows 的 16 位 UTF-16。所有涉及 std::wstring 的路径、
//       文件名、窗口标题都经过这里。转换错了不会崩溃，只会得到乱码
//       文件名或读不到历史数据。
//    2. GetSystemTimePreciseAsFileTime 的纪元基准 —— Common.h::Clock
//       假定 FILETIME 是「100ns 单位、起点 1601-01-01」，并减去
//       116444736000000000 换算成 Unix epoch 微秒。基准错了，
//       全部时间戳与按天切分的日志文件名都会错，且与 Windows 端
//       录的历史数据对不上。
// ============================================================

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include <mach/mach_time.h>
#include <libproc.h>
#include <time.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include "windows.h"
// 统一句柄类型（Process / File / Find）。CloseHandle 要靠它转调
// FileShim.mm 的 ReleaseHandle，避免两处各写一遍释放逻辑而漏掉某种句柄。
#include "shims/ShimHandle.h"

namespace {

// ------------------------------------------------------------
//  mach_absolute_time 的时基
//
//  Apple Silicon 上 mach_absolute_time 的 tick 不是纳秒（实测 24MHz，
//  即 numer=125 / denom=3），必须换算，否则 Clock::NowUs 的时间戳会
//  整体偏慢约 41.7 倍 —— 而且不会报错，只是数据全错。
// ------------------------------------------------------------
struct Timebase {
    uint64_t ticksPerSec = 1000000000ULL;
    Timebase() {
        mach_timebase_info_data_t tb{};
        if (mach_timebase_info(&tb) == KERN_SUCCESS && tb.numer > 0 && tb.denom > 0) {
            // nanoseconds = ticks * numer / denom
            // → ticks per second = 1e9 * denom / numer
            ticksPerSec = (uint64_t)((1000000000.0 * (double)tb.denom) / (double)tb.numer);
            if (ticksPerSec == 0) ticksPerSec = 1000000000ULL;
        }
    }
};
const Timebase& Tb() {
    static Timebase tb;
    return tb;
}

// FILETIME 纪元差：1601-01-01 → 1970-01-01，单位 100ns。
// 与 Common.h:127 的常量保持同一个值（那里是硬编码的字面量）。
constexpr uint64_t kFileTimeEpochBias = 116444736000000000ULL;

// ------------------------------------------------------------
//  UTF-32 ↔ UTF-8
//
//  手写而非用 CFString：一是避免在纯 C++ 编译单元里引入 CoreFoundation
//  依赖，二是 CFString 的 UTF32 变体（LE/BE）需要额外判断字节序，
//  而手写实现的语义完全可控、便于单元测试。
//
//  无效码点（代理区 D800-DFFF、超过 10FFFF）统一替换为 U+FFFD，
//  与 Windows 端 WideCharToMultiByte 传 lpDefaultChar 时的行为接近，
//  且保证输出永远是合法 UTF-8 —— 否则写进 jsonl 会让 Windows 端
//  的 JSON 解析器读到非法序列。
// ------------------------------------------------------------
uint32_t SanitizeCp(uint32_t cp) {
    if (cp >= 0x110000u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0xFFFDu;
    return cp;
}

size_t Utf8Len(uint32_t cp) {
    cp = SanitizeCp(cp);
    if (cp < 0x80u)     return 1;
    if (cp < 0x800u)    return 2;
    if (cp < 0x10000u)  return 3;
    return 4;
}

void Utf8Write(uint32_t cp, char* out) {
    cp = SanitizeCp(cp);
    if (cp < 0x80u) {
        out[0] = (char)cp;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
    }
}

// 从 UTF-8 解码一个码点，推进 pos。非法序列返回 U+FFFD 并前进 1 字节，
// 保证循环一定终止（不会因坏数据死循环）。
uint32_t Utf8Decode(const char* s, size_t len, size_t& pos) {
    if (pos >= len) return 0xFFFDu;
    uint8_t b0 = (uint8_t)s[pos];
    size_t need;
    uint32_t cp;
    if (b0 < 0x80u)        { ++pos; return b0; }
    else if ((b0 & 0xE0u) == 0xC0u) { need = 1; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0u) == 0xE0u) { need = 2; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8u) == 0xF0u) { need = 3; cp = b0 & 0x07u; }
    else { ++pos; return 0xFFFDu; }

    // 续字节不足：不能读越界。注意 need 是「除首字节外还需要的字节数」，
    // 故条件是 pos + need >= len（pos 本身已确认 < len）。
    if (pos + need >= len) { ++pos; return 0xFFFDu; }
    for (size_t k = 1; k <= need; ++k) {
        uint8_t bk = (uint8_t)s[pos + k];
        if ((bk & 0xC0u) != 0x80u) { ++pos; return 0xFFFDu; }
        cp = (cp << 6) | (uint32_t)(bk & 0x3Fu);
    }
    pos += need + 1;

    // 拒绝过长编码（overlong）：同样是非法 UTF-8
    static const uint32_t kMin[4] = { 0u, 0x80u, 0x800u, 0x10000u };
    if (cp < kMin[need]) return 0xFFFDu;
    return SanitizeCp(cp);
}

// ------------------------------------------------------------
//  OpenProcess 的句柄载体
//
//  macOS 没有进程句柄，但 Common.h::GetProcessExeName 的用法是
//  「OpenProcess → QueryFullProcessImageNameW → CloseHandle」，
//  三者成对出现。这里复用 FileShim.mm 定义的统一 ShimHandle 类型
//  （kind=Process，存 pid）。
//
//  ⚠️ 不要在本文件另定义一套句柄结构与 CloseHandle：那样 File 与
//     Find 句柄的释放就只有 FileShim.mm 知道，两处各写一遍必然漏掉
//     某种类型 —— 漏掉就是 fd 泄漏或 closedir 崩溃。
//     CloseHandle 统一转调 pbshim::ReleaseHandle。
// ------------------------------------------------------------

int ScreenDpi96() {
    @autoreleasepool {
        NSScreen* s = [NSScreen mainScreen];
        CGFloat f = s ? s.backingScaleFactor : 1.0;
        if (!(f > 0.0)) f = 1.0;
        // 以 96 为「1 倍」基准，与 Painter::DpiScale() = dpi/96 对齐。
        // 不能用 macOS 的 72pt/inch，否则 Retina 屏上界面整体缩小到 0.75 倍。
        return (int)(96.0 * (double)f + 0.5);
    }
}

} // anonymous namespace

// ------------------------------------------------------------
//  高精度时钟
// ------------------------------------------------------------
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency) {
    if (!lpFrequency) return FALSE;
    lpFrequency->QuadPart = (int64_t)Tb().ticksPerSec;
    return TRUE;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    if (!lpPerformanceCount) return FALSE;
    lpPerformanceCount->QuadPart = (int64_t)mach_absolute_time();
    return TRUE;
}

void GetSystemTimePreciseAsFileTime(FILETIME* lpSystemTimeAsFileTime) {
    if (!lpSystemTimeAsFileTime) return;
    struct timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        // 退化：用 time() 拿秒级精度，至少保证纪元基准正确
        time_t t = time(nullptr);
        ts.tv_sec = t;
        ts.tv_nsec = 0;
    }
    // 100ns 计数 = (epoch 秒 * 1e7) + (纳秒 / 100) + 纪元偏置
    uint64_t hundreds = (uint64_t)ts.tv_sec * 10000000ULL
                      + (uint64_t)ts.tv_nsec / 100ULL
                      + kFileTimeEpochBias;
    lpSystemTimeAsFileTime->dwLowDateTime  = (uint32_t)(hundreds & 0xFFFFFFFFULL);
    lpSystemTimeAsFileTime->dwHighDateTime = (uint32_t)(hundreds >> 32);
}

// ------------------------------------------------------------
//  编码转换
//
//  严格复刻 Win32 语义（Common.h::W2U/U2W 依赖这三条）：
//    · 源长度 < 0 表示以 null 结尾，且返回长度**含**终止 null；
//      源长度 >= 0 表示精确长度，返回值不含 null。
//    · 目标缓冲为空或容量为 0 时进入探测模式，只返回所需长度。
//    · 容量不足时返回 0 并置 errno（W2U/U2W 据此判定失败返回空串）。
// ------------------------------------------------------------
int WideCharToMultiByte(UINT /*CodePage*/, DWORD /*dwFlags*/,
                        const wchar_t* lpWideCharStr, int cchWideChar,
                        char* lpMultiByteStr, int cbMultiByte,
                        const char* /*lpDefaultChar*/, BOOL* lpUsedDefaultChar) {
    if (lpUsedDefaultChar) *lpUsedDefaultChar = FALSE;
    if (!lpWideCharStr) { errno = EINVAL; return 0; }

    size_t srcLen;
    bool includeNull;
    if (cchWideChar < 0) { srcLen = wcslen(lpWideCharStr); includeNull = true; }
    else                 { srcLen = (size_t)cchWideChar;    includeNull = false; }

    size_t need = 0;
    for (size_t i = 0; i < srcLen; ++i)
        need += Utf8Len((uint32_t)lpWideCharStr[i]);
    if (includeNull) need += 1;

    // 探测模式
    if (!lpMultiByteStr || cbMultiByte == 0) return (int)need;
    if (cbMultiByte < 0 || (size_t)cbMultiByte < need) { errno = ENOBUFS; return 0; }

    size_t off = 0;
    for (size_t i = 0; i < srcLen; ++i) {
        uint32_t cp = (uint32_t)lpWideCharStr[i];
        Utf8Write(cp, lpMultiByteStr + off);
        off += Utf8Len(cp);
    }
    if (includeNull) lpMultiByteStr[off++] = '\0';
    return (int)need;
}

int MultiByteToWideChar(UINT /*CodePage*/, DWORD /*dwFlags*/,
                        const char* lpMultiByteStr, int cbMultiByte,
                        wchar_t* lpWideCharStr, int cchWideChar) {
    if (!lpMultiByteStr) { errno = EINVAL; return 0; }

    size_t srcLen;
    bool includeNull;
    if (cbMultiByte < 0) { srcLen = strlen(lpMultiByteStr); includeNull = true; }
    else                 { srcLen = (size_t)cbMultiByte;     includeNull = false; }

    // 第一遍：数出需要多少 wchar_t
    size_t need = 0;
    size_t pos = 0;
    while (pos < srcLen) { Utf8Decode(lpMultiByteStr, srcLen, pos); ++need; }
    if (includeNull) need += 1;

    // 探测模式
    if (!lpWideCharStr || cchWideChar == 0) return (int)need;
    if (cchWideChar < 0 || (size_t)cchWideChar < need) { errno = ENOBUFS; return 0; }

    // 第二遍：写入
    size_t off = 0;
    pos = 0;
    while (pos < srcLen) {
        uint32_t cp = Utf8Decode(lpMultiByteStr, srcLen, pos);
        lpWideCharStr[off++] = (wchar_t)cp;
    }
    if (includeNull) lpWideCharStr[off++] = L'\0';
    return (int)need;
}

// ------------------------------------------------------------
//  进程映像名
// ------------------------------------------------------------
HANDLE OpenProcess(DWORD /*dwDesiredAccess*/, BOOL /*bInheritHandle*/, DWORD dwProcessId) {
    if (dwProcessId == 0) return nullptr;
    // 先探一下进程是否存在：proc_pidpath 对不存在的 pid 返回 0
    char probe[PROC_PIDPATHINFO_MAXSIZE] = {0};
    int n = proc_pidpath((int)dwProcessId, probe, sizeof(probe));
    if (n <= 0) return nullptr;

    pbshim::ShimHandle* h = new (std::nothrow) pbshim::ShimHandle();
    if (!h) return nullptr;
    h->kind = pbshim::HandleKind::Process;
    h->pid  = (pid_t)dwProcessId;
    return (HANDLE)h;
}

BOOL QueryFullProcessImageNameW(HANDLE hProcess, DWORD /*dwFlags*/,
                                LPWSTR lpExeName, DWORD* lpdwSize) {
    if (!hProcess || !lpExeName || !lpdwSize) return FALSE;
    pbshim::ShimHandle* h = (pbshim::ShimHandle*)hProcess;
    if (h->kind != pbshim::HandleKind::Process) return FALSE;

    DWORD cap = *lpdwSize;
    if (cap == 0) return FALSE;

    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    int n = proc_pidpath((int)h->pid, path, sizeof(path));
    if (n <= 0) return FALSE;

    // UTF-8 → UTF-32，受 cap 限制（cap 是 wchar_t 个数，含终止 null）
    size_t pos = 0, written = 0;
    size_t len = (size_t)n;
    while (pos < len && written + 1 < (size_t)cap) {
        uint32_t cp = Utf8Decode(path, len, pos);
        lpExeName[written++] = (wchar_t)cp;
    }
    lpExeName[written] = L'\0';
    *lpdwSize = (DWORD)written;   // Win32 语义：输出长度不含终止 null
    return TRUE;
}

BOOL CloseHandle(HANDLE hObject) {
    if (!hObject) return FALSE;

    // GetDC 返回的哨兵不是堆对象：调用方（Common.h::WindowDpi）用的是
    // ReleaseDC 而非 CloseHandle，但这里仍做一道保护，避免误关哨兵。
    if (hObject == (HANDLE)&pbshim::g_dcSentinelHandle) return TRUE;

    // 统一转调 FileShim.mm 的 ReleaseHandle —— 它同时认识
    // Process / File / Find 三种句柄。见该文件顶部的说明。
    return pbshim::ReleaseHandle((pbshim::ShimHandle*)hObject) ? TRUE : FALSE;
}

// ------------------------------------------------------------
//  DPI 探测路径
//
//  GetModuleHandleW 返回 nullptr（macOS 无 user32.dll），
//  GetProcAddress 随之返回 nullptr，于是 Common.h::WindowDpi 的
//  `if (fn)` 判定为假，自然落到 GetDC + GetDeviceCaps(LOGPIXELSX)
//  分支 —— 无需修改 Common.h 一行代码即可拿到正确的 Retina 缩放值。
// ------------------------------------------------------------
HMODULE GetModuleHandleW(LPCWSTR /*lpModuleName*/) {
    return nullptr;
}

void* GetProcAddress(HMODULE /*hModule*/, LPCSTR /*lpProcName*/) {
    return nullptr;
}

HDC GetDC(HWND /*hwnd*/) {
    // 非空哨兵，使 Common.h::WindowDpi 的 `if (dc)` 判定成立，
    // 从而走到 GetDeviceCaps(LOGPIXELSX) 分支拿到 Retina 缩放值。
    // 它是个静态对象的地址、不是堆句柄，ReleaseDC 是 no-op，
    // 且 CloseHandle 里有等值保护（不会误 delete 它）。
    return (HDC)&pbshim::g_dcSentinelHandle;
}

int ReleaseDC(HWND /*hwnd*/, HDC /*hdc*/) {
    return 1;
}

int GetDeviceCaps(HDC hdc, int nIndex) {
    if (!hdc) return 0;
    if (nIndex == LOGPIXELSX || nIndex == LOGPIXELSY) return ScreenDpi96();
    return 0;
}
