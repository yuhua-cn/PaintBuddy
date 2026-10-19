#pragma once
// ============================================================
//  Windows API 兼容垫片 —— macOS 端
//
//  为什么存在
//  ─────────
//  src/ 下的 Common.h / Ui.h / Charts.h / Store.h / Analyzer.h / Config.h
//  是 Windows 端源码，本移植方案要求「零修改复用」。其中 Common.h 直接
//  #include <windows.h>，Ui.h 直接 #include <d2d1.h>/<dwrite.h>/<wincodec.h>。
//  构建时把本目录（mac/compat）放在 include 搜索路径**最前面**，即可让这些
//  include 命中垫片而非真实 Windows SDK 头。Windows 端构建（build*.bat）
//  完全不涉及 mac/ 目录，互不干扰。
//
//  设计边界（重要）
//  ───────────────
//  · 只定义「原样编译的那几个文件」实际用到的符号，不做 Win32 仿真。
//    Windows 端专属的实现文件在 macOS 构建中**不参与编译**，由
//    mac/platform/*.mm 重写替代：
//        Store.cpp      → Store_mac.mm     （含注册表 → LaunchAgent）
//        Config.cpp     → Config_mac.mm    （targets 语义改 bundle id）
//        Ui.cpp         → Ui_mac.mm        （D2D → Core Graphics）
//        Capture.cpp    → Capture_mac.mm   （钩子 → CGEventTap）
//        PenInput.cpp   → PenInput_mac.mm  （HID 报文 → tablet 事件）
//        MainWindow.cpp → appkit/*.mm      （消息循环 → AppKit）
//        Recorder/Video → Recorder_mac.mm / Video_mac.mm
//        main.cpp       → appkit/main.mm
//    因此 CreateFileW / RegisterRawInputDevices / SetWindowsHookExW /
//    RegSetValueExW 等 API 一律**不提供**——提供了也无人调用，只会掩盖
//    「某个本该重写的文件被误编进来」这类错误。
//    不移植：PenInject.* / PenOverrides.* / PenInputDev.h / app.rc /
//            resource.h（Mac 端不注入事件、不解析 HID 报文、不用 rc 资源）
//
//  · 类型形状必须与 Windows 完全一致：Charts.cpp 会直接访问
//    D2D1_COLOR_F 的 .r/.g/.b/.a 与 D2D1_RECT_F 的 .left/.top/.right/.bottom，
//    字段名或顺序错一个就编译失败。
//
//  · ⚠️ wchar_t 宽度不同：Windows 是 16 位 UTF-16，macOS 是 32 位 UTF-32。
//    Common.h 的 SanitizeFileName / ExePathW / Painter::Text 全部以
//    std::wstring 与 wchar_t* 为参数。W2U / U2W 的实现必须按 UTF-32 处理
//    （见 shims/Win32Shim.mm）。这是本垫片最容易出错的地方，
//    任何涉及字符串的改动都要复查这一条。
// ============================================================

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <cwchar>       // ⚠️ 必需：Common.h:190 的 SanitizeFileName 用 wcschr，
                        // 但 Common.h 自身没 include <cwchar> —— Windows 上由
                        // SDK 头间接带入，本垫片必须显式提供，否则 wcschr 未声明。
#include <cctype>       // ⚠️ 必需：Common.h:181 的 ToLowerA 用 tolower，
                        // Common.h 同样没 include <cctype>，理由同上。
#include <string>
#include <strings.h>      // strcasecmp
#include <unistd.h>
#include <sys/types.h>

// ------------------------------------------------------------
//  基本标量类型
//
//  ⚠️ BOOL 必须复用 objc 的定义，不能自己 typedef。
//  macOS 的 <objc/objc.h> 已定义 BOOL（arm64 上是 bool，x86_64 上是
//  signed char），且没有 include guard 保护 —— 自己再 typedef int32_t
//  会直接编译报错「重复定义且类型不同」。
//
//  更隐蔽的坑：如果 BOOL 在 .cpp 里是 int32_t、在 .mm 里是 objc 的 bool，
//  那么 `WideCharToMultiByte(..., BOOL* lpUsedDefaultChar)` 在两种编译单元
//  里会生成**不同的 C++ mangled name**（参数类型参与 mangling），
//  实现方与调用方对不上，链接期报 undefined symbol。
//  无条件 include <objc/objc.h> 可让 BOOL 在所有编译单元里类型一致。
//  它是纯 C 头文件，在没有 ObjC 语法的 .cpp 里 include 是安全的。
// ------------------------------------------------------------
#include <objc/objc.h>   // BOOL（复用系统定义，见上方说明）

typedef uint8_t   BYTE;
typedef uint16_t  WORD;
typedef uint32_t  DWORD;
typedef int32_t   LONG;
typedef uint32_t  ULONG;
typedef int64_t   LONGLONG;
typedef uint64_t  ULONGLONG;
typedef unsigned int UINT;
typedef int            INT;
typedef float          FLOAT;

// HRESULT 必须是**有符号** 32 位：Store.cpp:75 用 SUCCEEDED(hr) 判定
// SHGetFolderPathW 的结果，而 SUCCEEDED 的语义是 hr >= 0（失败为负值）。
// 若 typedef 成 uint32_t，任何负的错误码都会变成巨大正数，
// SUCCEEDED 恒为真 —— 目录定位失败会被当成成功，数据写进错误路径。
typedef int32_t   HRESULT;

#define S_OK            ((HRESULT)0L)
#define S_FALSE         ((HRESULT)1L)
#define E_FAIL          ((HRESULT)0x80004005L)
#define E_INVALIDARG    ((HRESULT)0x80070057L)
#ifndef SUCCEEDED
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr)      (((HRESULT)(hr)) < 0)
#endif

// Win32 错误码。Store.cpp 用它们判断 CreateDirectoryW / RegOpenKeyExW 的结果。
// 这里给出与 Windows 相同的数值，便于日志里看到的错误码语义一致。
#define ERROR_SUCCESS            0L
#define ERROR_FILE_NOT_FOUND     2L
#define ERROR_ACCESS_DENIED      5L
#define ERROR_INVALID_PARAMETER  87L
#define ERROR_MORE_DATA          234L
#define ERROR_ALREADY_EXISTS     183L
#define ERROR_WRITE_FAULT        29L

typedef void*     HANDLE;
typedef void*     HWND;
typedef void*     HDC;
typedef void*     HMODULE;
typedef void*     HINSTANCE;
typedef void*     HBITMAP;
typedef void*     HICON;
typedef void*     HKEY;

typedef DWORD*        LPDWORD;
typedef wchar_t       WCHAR;
typedef const wchar_t* LPCWSTR;
typedef wchar_t*      LPWSTR;
typedef char*         LPSTR;
typedef const char*   LPCSTR;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

// ------------------------------------------------------------
//  虚拟键码（Virtual-Key Codes）
//
//  Store.cpp:449 的 VkNameShared 用这些常量把 vk 整数还原成可读键名
//  （"撤回 Ctrl+Z"、"F5"、"小键盘3" 等），采集层与分析层共用。
//
//  ⚠️ 这些值必须是 **Windows 官方的精确值**，不能随意填或映射到
//     macOS 的 keycode。原因：vk 字段会**原样写进 jsonl**（数据格式
//     的一部分），Mac 端读 Windows 录的历史数据时，靠这些值把 vk
//     还原成键名。若这里数值错了，Mac 上显示的键名会全错，且是
//     静默的（不崩溃、不报错）。
//     注：这只影响「读取 Windows 数据时的显示」。Mac 端自己采集时
//     要把 macOS keycode 映射回这些 Windows VK 值再落盘，才能保持
//     数据兼容 —— 那是 M3 采集层的职责，不在本垫片范围。
//
//  值取自 winuser.h，逐个核对过。
// ------------------------------------------------------------
#define VK_BACK           0x08
#define VK_TAB            0x09
#define VK_RETURN         0x0D
#define VK_SHIFT          0x10
#define VK_CONTROL        0x11
#define VK_MENU           0x12      // Alt
#define VK_CAPITAL        0x14      // Caps Lock
#define VK_ESCAPE         0x1B
#define VK_SPACE          0x20
#define VK_PRIOR          0x21      // Page Up
#define VK_NEXT           0x22      // Page Down
#define VK_END            0x23
#define VK_HOME           0x24
#define VK_LEFT           0x25
#define VK_UP             0x26
#define VK_RIGHT          0x27
#define VK_DOWN           0x28
#define VK_INSERT         0x2D
#define VK_DELETE         0x2E
#define VK_LWIN           0x5B
#define VK_RWIN           0x5C
#define VK_NUMPAD0        0x60
#define VK_NUMPAD9        0x69      // NUMPAD0..9 连续，Store.cpp 用范围比较
#define VK_F1             0x70
#define VK_F24            0x87      // F1..F24 连续，Store.cpp 用范围比较
#define VK_LSHIFT         0xA0
#define VK_RSHIFT         0xA1
#define VK_LCONTROL       0xA2
#define VK_RCONTROL       0xA3
#define VK_LMENU          0xA4
#define VK_RMENU          0xA5
#define VK_OEM_1          0xBA      // ;:
#define VK_OEM_PLUS       0xBB      // +
#define VK_OEM_COMMA      0xBC      // ,<
#define VK_OEM_MINUS      0xBD      // -_
#define VK_OEM_PERIOD     0xBE      // .>
#define VK_OEM_2          0xBF      // /?
#define VK_OEM_3          0xC0      // `~
#define VK_OEM_4          0xDB      // [{
#define VK_OEM_5          0xDC      // \|
#define VK_OEM_6          0xDD      // ]}
#define VK_OEM_7          0xDE      // '"

// ------------------------------------------------------------
//  64 位整数的联合体形态
//
//  Common.h::Clock 只用 LARGE_INTEGER::QuadPart；
//  Common.h::Clock::FileTimeToUs 用 ULARGE_INTEGER 的
//  LowPart / HighPart / QuadPart 三者，故匿名 struct 不可省。
//  匿名 struct in union 是 MSVC/GCC 扩展，CMakeLists 已加 -fms-extensions。
// ------------------------------------------------------------
typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t  HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        uint32_t LowPart;
        uint32_t HighPart;
    };
    uint64_t QuadPart;
} ULARGE_INTEGER;

// FILETIME：100ns 单位，起点 1601-01-01 UTC。
// Common.h::FileTimeToUs 会减去 116444736000000000 再除 10 得到 epoch 微秒，
// 故本垫片的 GetSystemTimePreciseAsFileTime 必须产出同一基准的计数。
typedef struct _FILETIME {
    uint32_t dwLowDateTime;
    uint32_t dwHighDateTime;
} FILETIME;

// ------------------------------------------------------------
//  MSVC 专有函数的等价物（纯头文件即可实现，无需平台代码）
// ------------------------------------------------------------

// ⚠️ 参数顺序陷阱：MSVC 的 localtime_s 是 (tm*, const time_t*)，
// 而 C11 标准的 localtime_s 是 (const time_t*, tm*) —— 两者相反。
// Common.h:142/154 的调用 `localtime_s(&lt, &sec)` 是 MSVC 顺序，
// 这里必须照 MSVC 的顺序实现，否则两个参数会被悄悄调换。
inline int localtime_s(struct tm* tmDest, const time_t* sourceTime) {
    if (!tmDest || !sourceTime) return EINVAL;
    return localtime_r(sourceTime, tmDest) ? 0 : errno;
}

inline int _stricmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    return strcasecmp(a, b);
}

inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        wchar_t ca = (*a >= L'A' && *a <= L'Z') ? (*a - L'A' + L'a') : *a;
        wchar_t cb = (*b >= L'A' && *b <= L'Z') ? (*b - L'A' + L'a') : *b;
        if (ca != cb) return (ca < cb) ? -1 : 1;
        ++a; ++b;
    }
    if (*a == *b) return 0;
    return (*a < *b) ? -1 : 1;
}

// Common.h 未用到，但 MSVC 安全函数族常被顺带引用，一并给出别名。
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// ------------------------------------------------------------
//  swprintf_s —— Charts.cpp 用到 13 处，是编译集中唯一缺失的符号
//
//  MSVC 提供两个重载：
//    1. int swprintf_s(wchar_t* buf, size_t sizeOfBuf, const wchar_t* fmt, ...)
//    2. template<size_t N>
//       int swprintf_s(wchar_t (&buf)[N], const wchar_t* fmt, ...)   // C++ 专用
//  第二个是模板，能自动推导数组长度，调用时**省略 size 参数**。
//  Charts.cpp 全部用的是这个形式（如 `swprintf_s(b, L"%.2f", v)`，
//  b 是 wchar_t 数组），故模板重载是必需的，只提供带 size 的那个会
//  匹配失败并报 "use of undeclared identifier"。
//
//  POSIX 的 vswprintf(buf, n, fmt, ap) 参数顺序与 MSVC 第一个重载一致，
//  故模板内部补上推导出的 N 即可。
//
//  ⚠️ 失败语义要对齐 MSVC：截断或编码错误时 vswprintf 返回负值，
//     MSVC 的 swprintf_s 此时会把 buf[0] 置为 L'\0'（保证字符串已终止）。
//     这里照做 —— 否则调用方拿到一个未终止的缓冲区，后续 wcslen 会越界读。
//     Charts.cpp 的 tooltip 拼接路径上有多处这种调用。
// ------------------------------------------------------------
#ifdef __cplusplus
#include <cstdarg>
#include <wchar.h>   // vswprintf 的全局命名空间版本。<cwchar> 只保证
                     // 把它放进 std::，显式引入 C 头可避免不同实现下的差异。

template <size_t N>
inline int swprintf_s(wchar_t (&buf)[N], const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(buf, N, fmt, ap);
    va_end(ap);
    if (r < 0) buf[0] = L'\0';
    return r;
}

// 带显式大小的重载（MSVC 形式 1）。当前编译集未用到，但与模板版并存可
// 让「先算好长度再格式化」的写法也能编译，且两者不会歧义
// （数组实参优先匹配模板，指针实参只能匹配这个）。
inline int swprintf_s(wchar_t* buf, size_t sizeOfBuf, const wchar_t* fmt, ...) {
    if (!buf || sizeOfBuf == 0) return -1;
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(buf, sizeOfBuf, fmt, ap);
    va_end(ap);
    if (r < 0) buf[0] = L'\0';
    return r;
}
#endif

// ------------------------------------------------------------
//  高精度时钟（实现见 shims/Win32Shim.mm，底层 mach_absolute_time）
//
//  Common.h::Clock 的口径是：NowUs = baseEpochUs + (qpc - baseQpc) * 1e6 / freq，
//  因此 freq 必须是「每秒计数数」，qpc 必须是同一时基的单调计数。
//  Apple Silicon 上 mach_absolute_time 的 tick 不等于纳秒，须经
//  mach_timebase_info 换算，实现里已处理。
// ------------------------------------------------------------
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);
BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
void GetSystemTimePreciseAsFileTime(FILETIME* lpSystemTimeAsFileTime);

// ------------------------------------------------------------
//  编码转换（实现见 shims/Win32Shim.mm）
//
//  ⚠️ macOS 的 wchar_t 是 UTF-32。实现按 UTF-32 ↔ UTF-8 转换，
//  并严格保留 Win32 的三条语义，因为 Common.h::W2U/U2W 依赖它们：
//    1. cchWideChar < 0 表示源以 null 结尾，且返回长度**含**终止 null；
//    2. 目标缓冲为 nullptr 或大小为 0 时只返回所需长度（探测模式）；
//    3. 缓冲不足时返回 0（W2U 据此判定失败并返回空串）。
// ------------------------------------------------------------
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags,
                        const wchar_t* lpWideCharStr, int cchWideChar,
                        char* lpMultiByteStr, int cbMultiByte,
                        const char* lpDefaultChar, BOOL* lpUsedDefaultChar);

int MultiByteToWideChar(UINT CodePage, DWORD dwFlags,
                        const char* lpMultiByteStr, int cbMultiByte,
                        wchar_t* lpWideCharStr, int cchWideChar);

// ------------------------------------------------------------
//  进程映像名（实现见 shims/Win32Shim.mm，底层 libproc）
//
//  Common.h::GetProcessExeName(DWORD) 是 inline 的，会被真实编译，
//  故这三个符号缺一不可。macOS 无进程句柄概念：OpenProcess 直接把 pid
//  当句柄返回，QueryFullProcessImageNameW 用 proc_pidpath 取路径，
//  CloseHandle 是 no-op。现有代码「取完名字立刻 CloseHandle」的用法
//  与此完全兼容。
// ------------------------------------------------------------
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000u
#define PROCESS_QUERY_INFORMATION         0x0400u

HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
BOOL   QueryFullProcessImageNameW(HANDLE hProcess, DWORD dwFlags,
                                  LPWSTR lpExeName, DWORD* lpdwSize);
BOOL   CloseHandle(HANDLE hObject);

// ------------------------------------------------------------
//  错误码
//  Store.h::DataDirHealth::lastError 是 DWORD，Mac 端填 errno。
//  这里让 GetLastError 直接返回 errno（诊断用途，不需要精确映射到
//  Win32 错误码空间——该字段只被打印，从不被比较）。
// ------------------------------------------------------------
inline DWORD GetLastError(void) { return (DWORD)errno; }
inline void  SetLastError(DWORD e) { errno = (int)e; }

// ------------------------------------------------------------
//  DPI 探测：让 Common.h::WindowDpi 走 fallback 分支即可拿到正确值
//
//  Common.h:378-402 的 WindowDpi(HWND) 是 inline 的，逻辑为：
//      1. GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")
//         → 若拿到函数指针则用它
//      2. 否则 GetDC(hwnd) + GetDeviceCaps(LOGPIXELSX)
//      3. 再否则 GetDC(nullptr) + GetDeviceCaps(LOGPIXELSX)
//      4. 全失败返回 96
//  macOS 上没有 GetDpiForWindow，于是让 GetProcAddress 返回 nullptr，
//  使其自然落到第 3 步；GetDeviceCaps 则返回 96 * backingScaleFactor。
//
//  为什么是 96 基准而不是 macOS 的 72pt/inch：Painter::DpiScale() = dpi/96
//  （Ui.h:173），整个布局体系以 96 为「1 倍」。若这里返回 72，
//  Retina 屏上会得到 0.75 倍缩放，界面整体缩小。
//  ⚠️ 这条口径与 Ui.h:174-186 记录的 Windows 端 DPI 缺陷直接相关，
//     改动前请先读那段注释。
// ------------------------------------------------------------
#define LOGPIXELSX 88
#define LOGPIXELSY 90

// GetDeviceCaps 的实现需要读 NSScreen.backingScaleFactor，故 Win32Shim 是
// Objective-C++（.mm）。这不会让 BOOL 类型漂移 —— windows.h 已无条件
// include <objc/objc.h>，所有编译单元看到的 BOOL 都是同一个定义。
HMODULE GetModuleHandleW(LPCWSTR lpModuleName);
void*   GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
HDC     GetDC(HWND hwnd);
int     ReleaseDC(HWND hwnd, HDC hdc);
int     GetDeviceCaps(HDC hdc, int nIndex);

// ------------------------------------------------------------
//  文件属性常量
//
//  Store.cpp:33/174 用 `attr & FILE_ATTRIBUTE_DIRECTORY` 判断是否目录，
//  用 `attr != INVALID_FILE_ATTRIBUTES` 判断路径是否存在。
//  数值与 Win32 一致，便于日志里的属性值可读。
// ------------------------------------------------------------
#define INVALID_FILE_ATTRIBUTES   ((DWORD)-1)
#define FILE_ATTRIBUTE_NORMAL     0x00000080u
#define FILE_ATTRIBUTE_DIRECTORY  0x00000010u
#define FILE_ATTRIBUTE_HIDDEN     0x00000002u
#define FILE_ATTRIBUTE_READONLY   0x00000001u

// CreateFileW 的参数常量
#define GENERIC_READ      0x80000000u
#define GENERIC_WRITE     0x40000000u
#define FILE_SHARE_READ   0x00000001u
#define FILE_SHARE_WRITE  0x00000002u
#define CREATE_NEW        1u
#define CREATE_ALWAYS     2u
#define OPEN_EXISTING     3u
#define OPEN_ALWAYS       4u
#define TRUNCATE_EXISTING 5u

// MoveFileExW 的标志。
// Store.cpp:323 用 MOVEFILE_WRITE_THROUGH 做原子写：先写 .tmp 再改名，
// 并要求改名操作本身落盘 —— 否则断电后可能只剩改名前的状态，
// config.json 变成半截 JSON，用户全部配置丢失。
#define MOVEFILE_REPLACE_EXISTING  0x1u
#define MOVEFILE_WRITE_THROUGH     0x8u

// GetFileAttributesExW 的信息级别
typedef enum GET_FILEEX_INFO_LEVELS {
    GetFileExInfoStandard = 0,
} GET_FILEEX_INFO_LEVELS;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;      // Store.cpp:248/353 用它拼 64 位文件大小
    DWORD    nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;

// FindFirstFileW 的结果结构。
// ⚠️ cFileName 必须是 wchar_t[MAX_PATH]：Store.cpp:338 用 W2U(fd.cFileName)
//    转成 UTF-8 文件名，宽度假定与 Windows 一致（32 位 UTF-32 由垫片处理）。
typedef struct _WIN32_FIND_DATAW {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    wchar_t  cFileName[MAX_PATH];
    wchar_t  cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

// ------------------------------------------------------------
//  ⚠️ 路径分隔符归一化（垫片的核心隐藏职责）
//
//  Store.cpp 用 L"\\" 硬编码分隔符拼接路径：
//    :76   base = appdata + L"\\PaintingBuddy"
//    :100  g_configPath = g_dataDir + "\\config.json"
//    :380  p = LogDir() + "\\" + day + ".jsonl"
//  在 macOS 上，反斜杠**不是**路径分隔符，而是合法的普通文件名字符。
//  若原样传给 POSIX API，`.../PaintingBuddy\logs` 会被当成一个
//  「名字里含反斜杠的单个文件」，于是目录永远建不出来、日志永远写不进，
//  而且**不会报错** —— 症状与 Windows 端那个 UTF-8 被按 GBK 解释的
//  历史故障一模一样：录屏正常、统计全空、磁盘上零证据。
//
//  因此下面每一个接受宽字符路径的 API，实现里都必须先把 '\' 归一化为 '/'。
//  新增任何文件 API 到垫片时，这一条不可遗漏。
// ------------------------------------------------------------

// 文件与目录（实现见 shims/FileShim.mm）
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   void* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL   ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead,
                DWORD* lpNumberOfBytesRead, void* lpOverlapped);
BOOL   WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
                 DWORD* lpNumberOfBytesWritten, void* lpOverlapped);
DWORD  GetFileAttributesW(LPCWSTR lpFileName);
BOOL   GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS level,
                            void* lpFileInformation);
// Store.cpp:288 用它取文件字节数（ReadWholeFile 先探大小再一次性读入）。
// lpFileSize 是 LARGE_INTEGER*，实现填 QuadPart。
BOOL   GetFileSizeEx(HANDLE hFile, LARGE_INTEGER* lpFileSize);
// Store.cpp:320 的 WriteWholeFileAtomic 用它，在 rename 覆盖前把数据落盘。
// ⚠️ 必须真的 fsync，不能空实现：它与 MOVEFILE_WRITE_THROUGH 一起构成
//    config.json 的原子写，防断电写坏。实现见 shims/FileShim.mm。
BOOL   FlushFileBuffers(HANDLE hFile);
BOOL   CreateDirectoryW(LPCWSTR lpPathName, void* lpSecurityAttributes);
BOOL   DeleteFileW(LPCWSTR lpFileName);
BOOL   MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);
HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
BOOL   FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
BOOL   FindClose(HANDLE hFindFile);
DWORD  GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);

// 安全函数族：Store.cpp:224 的 _dupenv_s（读 PB_SAMPLE_DIR）
int    _dupenv_s(char** pBuffer, size_t* pNumberOfElements, const char* varName);

// ------------------------------------------------------------
//  注册表 API → LaunchAgent
//
//  Store.cpp:704-757 用注册表实现开机自启（写 HKCU Run 键 + --tray）。
//  macOS 的对应机制是 ~/Library/LaunchAgents/<label>.plist。
//
//  垫片把注册表的「键/值」模型映射到「一个 plist 文件」：
//    RegOpenKeyExW(HKEY_CURRENT_USER, Run子键, ...) → 返回一个哨兵句柄
//      · 请求写权限(KEY_SET_VALUE)：总是成功（后续 Set/Delete 才真正落地）
//      · 只请求读权限(KEY_QUERY_VALUE)：plist 不存在时返回 ERROR_FILE_NOT_FOUND，
//        使 AutoStartEnabled 正确报告「未开启」
//    RegSetValueExW  → 生成 plist 并写入（含 --tray 参数）
//    RegQueryValueExW→ 读回 plist 里的程序路径，按 REG_SZ 宽字符返回
//    RegDeleteValueW → 删除 plist
//
//  ⚠️ Store.cpp:763-767 的注释指出「不能只比较值存不存在」：exe 挪过位置后
//     值依然存在但指向旧路径，那样开机会启动一个不存在的文件。
//     故 AutoStartSync 会比对内容是否为当前 exe。垫片的 Query 必须如实
//     返回 plist 里记录的路径，这个自愈逻辑才能生效。
// ------------------------------------------------------------
#define HKEY_CURRENT_USER  ((HKEY)(intptr_t)0x80000001)
#define KEY_QUERY_VALUE    0x0001u
#define KEY_SET_VALUE      0x0002u
#define KEY_ALL_ACCESS     0xF003Fu
#define REG_SZ             1u

LONG RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
                   DWORD samDesired, HKEY* phkResult);
LONG RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
                      LPDWORD lpType, BYTE* lpData, LPDWORD lpcbData);
LONG RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved,
                    DWORD dwType, const BYTE* lpData, DWORD cbData);
LONG RegDeleteValueW(HKEY hKey, LPCWSTR lpValueName);
LONG RegCloseKey(HKEY hKey);

// ------------------------------------------------------------
//  Store.h 需要的少量声明
// ------------------------------------------------------------
