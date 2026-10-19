// ============================================================
//  数位笔通道诊断：一次性探测笔数据到底走哪条通道
//
//  背景：数位板启用 Windows Ink（Wacom 默认开）后，笔输入被系统
//  接管，可能不再通过普通 Raw Input(HID digitizer) 投递，而是走
//  WM_POINTER；同时笔会生成「注入的鼠标事件」。本工具同时监听：
//    A) Raw Input —— usage page 0x0D（digitizer）报文
//    B) WM_POINTER —— 指针/笔事件（含压力、类型）
//    C) 鼠标 WM_INPUT / 注入鼠标事件特征
//  运行后用笔在屏幕上画几笔，报告会打印每条通道各收到多少，
//  从而确定主程序该用哪条通道采集笔。
// ============================================================
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")

static const wchar_t* const CLS = L"PenDiagWnd";
static HWND g_hwnd = nullptr;

// 诊断报告双输出：控制台 + UTF-8 文件（供用户回传）。
// 为什么不能只靠 shell 重定向：中文 Windows 控制台是 GBK，`>` 重定向出来的
// 文件中文会乱码，用户回传过来我这边也读不对。这里显式以 UTF-8 写文件。
static FILE* g_report = nullptr;
#define REPORT(...) do { \
    printf(__VA_ARGS__); \
    if (g_report) { fprintf(g_report, __VA_ARGS__); } \
} while (0)

// 统计
static uint64_t g_riTotal = 0;       // 所有 WM_INPUT
static uint64_t g_riHid = 0;         // dwType==HID
static uint64_t g_riDigitizer = 0;   // usage page 0x0D
static uint64_t g_riMouse = 0;       // dwType==MOUSE
// 按 HID Usage 细分（判断数据到底来自哪个集合）
static uint64_t g_usage01 = 0;       // Finger / 本机 Wacom 的 0D:01 集合
static uint64_t g_usage02 = 0;       // Pen
static uint64_t g_usage04 = 0;       // Touch Screen
static uint64_t g_usage0C = 0;       // Multi-point digitizer
static uint64_t g_usageOther = 0;
static std::map<HANDLE, int> g_riDev;         // 每个设备报文数
static std::map<HANDLE, std::string> g_riDevName;

// —— 每设备明细（供诊断导出：采样率、usage、原始报文样本）——
struct DevDetail {
    std::string name;
    USHORT usagePage = 0, usage = 0;
    DWORD  vid = 0, pid = 0;
    int    reports = 0;
    DWORD  firstTick = 0, lastTick = 0;   // 首末报文时刻，用于算有效采样率
    std::vector<std::string> samples;    // 前若干条原始报文 hex
};
static std::map<HANDLE, DevDetail> g_devDetail;
static const size_t kMaxSamplesPerDev = 5;   // 每设备最多保留几条报文样本

// 采样率偏低是「设备停在低速模式」的典型症状（部分国产板未装官方驱动时）
static void NoteReport(HANDLE h, const BYTE* data, ULONG len) {
    DevDetail& d = g_devDetail[h];
    DWORD now = GetTickCount();
    if (d.firstTick == 0) d.firstTick = now;
    d.lastTick = now;
    ++d.reports;
    if (d.samples.size() < kMaxSamplesPerDev && len > 0) {
        std::string hex;
        char b[8];
        ULONG n = len > 24 ? 24 : len;
        for (ULONG i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02X ", data[i]); hex += b; }
        if (len > n) hex += "...";
        d.samples.push_back(hex);
    }
}

static uint64_t g_ptrTotal = 0;      // 所有 WM_POINTER*
static uint64_t g_ptrPen = 0;        // PT_PEN
static uint64_t g_ptrTouch = 0;      // PT_TOUCH
static uint64_t g_ptrMouse = 0;      // PT_MOUSE
static uint64_t g_ptrPressureNonZero = 0;
static uint32_t g_ptrMaxPressure = 0;

static uint64_t g_injectMouse = 0;   // 带 FROMTOUCH 签名的注入鼠标事件
static uint64_t g_plainMouse = 0;

// WM_POINTER 相关 API（运行时动态加载，避免链接依赖）
typedef BOOL (WINAPI *GetPointerInfoT)(UINT32, POINTER_INFO*);
typedef BOOL (WINAPI *GetPointerPenInfoT)(UINT32, POINTER_PEN_INFO*);
static GetPointerInfoT    pGetPointerInfo = nullptr;
static GetPointerPenInfoT pGetPointerPenInfo = nullptr;

static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string DevName(HANDLE h) {
    wchar_t buf[512] = {0};
    UINT cb = sizeof(buf);
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, buf, &cb) != (UINT)-1)
        return WideToUtf8(buf);
    return "(unknown)";
}

static std::string DevTypeStr(HANDLE h) {
    RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
    UINT sz = sizeof(info);
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) return "?";
    char b[64];
    if (info.dwType == RIM_TYPEHID)
        snprintf(b, sizeof(b), "HID usage=%02X:%02X VID=%04lX PID=%04lX",
                 info.hid.usUsagePage, info.hid.usUsage, info.hid.dwVendorId, info.hid.dwProductId);
    else if (info.dwType == RIM_TYPEMOUSE) snprintf(b, sizeof(b), "MOUSE");
    else if (info.dwType == RIM_TYPEKEYBOARD) snprintf(b, sizeof(b), "KEYBOARD");
    else snprintf(b, sizeof(b), "type=%lu", (unsigned long)info.dwType);
    return b;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INPUT: {
        ++g_riTotal;
        UINT size = 0;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
            return DefWindowProcW(h, m, w, l);
        std::vector<BYTE> buf(size);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size)
            return DefWindowProcW(h, m, w, l);
        RAWINPUT* ri = (RAWINPUT*)buf.data();
        if (ri->header.dwType == RIM_TYPEHID) {
            ++g_riHid;
            g_riDev[ri->header.hDevice]++;
            if (g_riDevName.find(ri->header.hDevice) == g_riDevName.end())
                g_riDevName[ri->header.hDevice] = DevName(ri->header.hDevice);
            // 判断是否 digitizer：查该设备的 usage page，并按 usage 分类统计
            RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
            UINT sz = sizeof(info);
            if (GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1) {
                if (info.hid.usUsagePage == 0x0D) {
                    ++g_riDigitizer;
                    switch (info.hid.usUsage) {
                    case 0x01: ++g_usage01; break;
                    case 0x02: ++g_usage02; break;
                    case 0x04: ++g_usage04; break;
                    case 0x0C: ++g_usage0C; break;
                    default:   ++g_usageOther; break;
                    }
                    // 记录明细：usage / VID / PID / 报文样本 / 时间跨度
                    DevDetail& dd = g_devDetail[ri->header.hDevice];
                    dd.name = g_riDevName[ri->header.hDevice];
                    dd.usagePage = info.hid.usUsagePage;
                    dd.usage     = info.hid.usUsage;
                    dd.vid       = info.hid.dwVendorId;
                    dd.pid       = info.hid.dwProductId;
                    if (ri->data.hid.dwCount > 0 && ri->data.hid.dwSizeHid > 0)
                        NoteReport(ri->header.hDevice, ri->data.hid.bRawData,
                                   ri->data.hid.dwSizeHid);
                }
            }
        } else if (ri->header.dwType == RIM_TYPEMOUSE) {
            ++g_riMouse;
        }
        return DefWindowProcW(h, m, w, l);
    }
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
    case WM_POINTERENTER: case WM_POINTERLEAVE: case WM_POINTERCAPTURECHANGED: {
        ++g_ptrTotal;
        if (pGetPointerInfo) {
            POINTER_INFO pi{};
            if (pGetPointerInfo(GET_POINTERID_WPARAM(w), &pi)) {
                switch (pi.pointerType) {
                case PT_PEN: {
                    ++g_ptrPen;
                    if (pGetPointerPenInfo) {
                        POINTER_PEN_INFO ppi{};
                        if (pGetPointerPenInfo(GET_POINTERID_WPARAM(w), &ppi)) {
                            if (ppi.pressure > 0) ++g_ptrPressureNonZero;
                            if (ppi.pressure > g_ptrMaxPressure) g_ptrMaxPressure = ppi.pressure;
                        }
                    }
                    break;
                }
                case PT_TOUCH:      ++g_ptrTouch; break;
                case PT_MOUSE:      ++g_ptrMouse; break;
                default: break;
                }
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    case WM_MOUSEMOVE: {
        LRESULT extra = GetMessageExtraInfo();
        // Windows Ink / 触摸注入鼠标事件的签名
        if ((((DWORD)extra) & 0xFFFFFF00) == (0xFF515700 & 0xFFFFFF00)) ++g_injectMouse;
        else ++g_plainMouse;
        return DefWindowProcW(h, m, w, l);
    }
    default: return DefWindowProcW(h, m, w, l);
    }
}

int main(int argc, char** argv) {
    int seconds = 15;
    if (argc > 1) seconds = atoi(argv[1]);
    if (seconds < 3) seconds = 3;
    if (seconds > 120) seconds = 120;

    // 第 2 个参数 =pageonly 时用 RIDEV_PAGEONLY 注册整个 0x0D 页，
    // 用于验证「放开注册」在真机是否被接受、以及会涌入哪些杂设备报文。
    // 缺省仍用旧的显式 4-usage 清单，便于两种模式对比。
    bool pageOnly = false;
    if (argc > 2 && _stricmp(argv[2], "pageonly") == 0) pageOnly = true;

    // 诊断报告输出文件：与 exe 同目录，UTF-8，供用户回传。
    //
    // 刻意用 "wb" 而不是 L"w, ccs=UTF-8"：后者会把流变成宽字符流，
    // fprintf 传入的 UTF-8 窄字节会按本地代码页（中文 Windows 是 GBK）
    // 二次转换，中文全变乱码 —— 正好毁掉回传文件的用途。
    // 二进制模式下 UTF-8 字节原样落盘，再手写一个 BOM 让记事本能正确识别。
    //
    // 打开失败不致命（还能看控制台输出），只影响导出。
    {
        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
        std::wstring outPath = dir + L"pen_diag_report.txt";
        errno_t e = _wfopen_s(&g_report, outPath.c_str(), L"wb");
        if (e != 0) g_report = nullptr;
        if (g_report) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, sizeof(bom), g_report);
        }
        printf("诊断报告将同时写入：%s%s\n\n",
               WideToUtf8(outPath.c_str()).c_str(),
               g_report ? "" : "（打开失败，仅控制台输出）");
    }
    if (g_report) {
        REPORT("==== 绘画伴侣 数位笔诊断报告 ====\n");
        char timeBuf[64];
        time_t t = time(nullptr);
        struct tm lt; localtime_s(&lt, &t);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);
        REPORT("生成时间: %s   采集时长: %d 秒   注册模式: %s\n\n",
               timeBuf, seconds, pageOnly ? "PAGEONLY(0x0D 全部 TLC)" : "显式 usage 清单");
    }

    pGetPointerInfo    = (GetPointerInfoT)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetPointerInfo");
    pGetPointerPenInfo = (GetPointerPenInfoT)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetPointerPenInfo");

    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = CLS;
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed %lu\n", GetLastError()); return 1; }

    // 用一个可见的小窗口，确保能收到 WM_POINTER（指针输入需要真实窗口）
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, CLS, L"数位笔诊断 - 请在此窗口用笔画几笔",
                             WS_OVERLAPPEDWINDOW, 100, 100, 520, 360,
                             nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) { printf("CreateWindow failed %lu\n", GetLastError()); return 1; }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // 让窗口接收 WM_POINTER（把指针输入路由到本窗口）
    typedef BOOL (WINAPI *RegisterPointerInputTargetT)(HWND, UINT32);
    // 不强制独占，只启用指针消息：EnableMouseInPointer 可选
    // 关键：调用 RegisterPointerInputTarget 让本窗口收到 pen 的 WM_POINTER
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    // POINTER_INPUT_TYPE 全部 = 0xFFFFFFFF 需 PT_POINTER；这里启用 pen+touch+mouse
    // RegisterPointerInputTarget 会独占，改用默认路由：窗口本就能收到 WM_POINTER（若系统投递）

    // 注册 Raw Input。两种模式：
    //   pageOnly  → RIDEV_PAGEONLY 注册整个 0x0D 页（usUsage 必须为 0），
    //               与主程序默认一致，用于验证系统是否接受 + 观测杂设备流量
    //   默认      → 显式 01/02/04/0C 四个 usage（旧行为，便于对比）
    // 两种模式都额外注册鼠标，用于对比注入鼠标通道。
    RAWINPUTDEVICE rid[8];
    int n = 0;
    if (pageOnly) {
        rid[n].usUsagePage = 0x0D; rid[n].usUsage = 0;
        rid[n].dwFlags = RIDEV_PAGEONLY | RIDEV_INPUTSINK; rid[n].hwndTarget = g_hwnd; ++n;
        printf("[mode] RIDEV_PAGEONLY (0x0D 全部 TLC)\n");
    } else {
        const int digUsages[4] = { 0x01, 0x02, 0x04, 0x0C };
        for (int i = 0; i < 4; ++i) {
            rid[n].usUsagePage = 0x0D; rid[n].usUsage = (USHORT)digUsages[i];
            rid[n].dwFlags = RIDEV_INPUTSINK; rid[n].hwndTarget = g_hwnd; ++n;
        }
        printf("[mode] 显式 usage 清单 (01/02/04/0C)\n");
    }
    rid[n].usUsagePage = 0x01; rid[n].usUsage = 0x02; rid[n].dwFlags = RIDEV_INPUTSINK; rid[n].hwndTarget = g_hwnd; ++n; // Mouse
    if (!RegisterRawInputDevices(rid, n, sizeof(RAWINPUTDEVICE))) {
        printf("RegisterRawInputDevices failed %lu\n", GetLastError());
    } else {
        printf("RegisterRawInputDevices OK (n=%d)\n", n);
    }

    // 枚举现有 digitizer 设备
    REPORT("==== 已连接的数位设备（Raw Input 枚举）====\n");
    UINT nd = 0;
    GetRawInputDeviceList(nullptr, &nd, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(nd);
    if (nd) GetRawInputDeviceList(list.data(), &nd, sizeof(RAWINPUTDEVICELIST));
    int penLike = 0;
    for (UINT i = 0; i < nd; ++i) {
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if (info.dwType != RIM_TYPEHID) continue;
        if (info.hid.usUsagePage != 0x0D) continue;
        ++penLike;
        REPORT("  [%u] usage=%02X:%02X VID=%04lX PID=%04lX  %s\n", i,
               info.hid.usUsagePage, info.hid.usUsage,
               info.hid.dwVendorId, info.hid.dwProductId,
               DevName(list[i].hDevice).c_str());
    }
    if (penLike == 0) REPORT("  （未发现任何 usage page 0x0D 数位设备！）\n");

    printf("\n==== 请用数位笔在窗口内画几笔（%d 秒）====\n", seconds);
    printf("（也可用鼠标点画，用于对比）\n\n");

    // 消息循环
    DWORD end = GetTickCount() + (DWORD)seconds * 1000;
    int lastReport = 0;
    MSG msg;
    while ((int)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int elapsed = seconds - (int)((end - GetTickCount()) / 1000);
        if (elapsed != lastReport && elapsed % 3 == 0) {
            lastReport = elapsed;
            printf("  [%2ds] RI:hid=%llu dig=%llu | POINTER:pen=%llu touch=%llu | injectMouse=%llu\n",
                   elapsed,
                   (unsigned long long)g_riHid, (unsigned long long)g_riDigitizer,
                   (unsigned long long)g_ptrPen, (unsigned long long)g_ptrTouch,
                   (unsigned long long)g_injectMouse);
        }
        Sleep(10);
    }
done:

    REPORT("\n==================== 诊断报告 ====================\n");
    REPORT("A) Raw Input 通道:\n");
    REPORT("   WM_INPUT 总数        : %llu\n", (unsigned long long)g_riTotal);
    REPORT("   其中 HID             : %llu\n", (unsigned long long)g_riHid);
    REPORT("   其中 digitizer(0x0D) : %llu   <<< 数位笔原始报文\n", (unsigned long long)g_riDigitizer);
    REPORT("   按 Usage 细分（关键：数据来自哪个集合）:\n");
    REPORT("       usage 0x01 (Finger/本机Wacom集合) : %llu\n", (unsigned long long)g_usage01);
    REPORT("       usage 0x02 (Pen)                  : %llu\n", (unsigned long long)g_usage02);
    REPORT("       usage 0x04 (Touch Screen)         : %llu\n", (unsigned long long)g_usage04);
    REPORT("       usage 0x0C (Multi-point)          : %llu\n", (unsigned long long)g_usage0C);
    if (g_usageOther) REPORT("       其他 usage                        : %llu\n", (unsigned long long)g_usageOther);
    REPORT("   其中 mouse           : %llu\n", (unsigned long long)g_riMouse);

    // —— 每设备明细：报文数 / 有效采样率 / 原始报文样本 ——
    //
    // 有效采样率是关键指标：部分国产数位板在未装官方驱动时会停在低速模式
    // （出厂固件默认低轮询 + 低坐标分辨率，需收到特定 Feature/Output 报文
    // 才切到全速）。症状是轨迹稀疏、速度曲线失真，而用户完全不知道原因。
    // 笔的正常轮询率通常在 133~266 Hz；低于 80 Hz 就值得怀疑。
    REPORT("   各设备明细:\n");
    for (const auto& kv : g_devDetail) {
        const DevDetail& d = kv.second;
        REPORT("   ---- VID=%04lX PID=%04lX usage=%02X:%02X ----\n",
               d.vid, d.pid, d.usagePage, d.usage);
        REPORT("     路径   : %s\n", d.name.c_str());
        REPORT("     报文数 : %d\n", d.reports);
        if (d.reports > 1 && d.lastTick > d.firstTick) {
            double secs = (double)(d.lastTick - d.firstTick) / 1000.0;
            double rate = (double)(d.reports - 1) / secs;
            REPORT("     有效采样率: %.1f Hz（跨度 %.2f 秒）%s\n",
                   rate, secs,
                   rate < 80.0 ? "   <<< 偏低！设备可能停在低速模式（考虑安装官方驱动）" : "");
        } else {
            REPORT("     有效采样率: 无法计算（报文数不足或时间跨度为 0）\n");
        }
        for (size_t i = 0; i < d.samples.size(); ++i)
            REPORT("     报文[%zu] : %s\n", i, d.samples[i].c_str());
    }
    if (g_devDetail.empty()) REPORT("     （无 digitizer 设备产生报文）\n");

    REPORT("\nB) WM_POINTER 通道:\n");
    REPORT("   WM_POINTER 总数      : %llu\n", (unsigned long long)g_ptrTotal);
    REPORT("   其中 PEN             : %llu   <<< 数位笔（Windows Ink 路径）\n", (unsigned long long)g_ptrPen);
    REPORT("   其中 TOUCH           : %llu\n", (unsigned long long)g_ptrTouch);
    REPORT("   其中 MOUSE           : %llu\n", (unsigned long long)g_ptrMouse);
    REPORT("   笔压力>0 的采样      : %llu  (最大压力值 %u)\n",
           (unsigned long long)g_ptrPressureNonZero, g_ptrMaxPressure);

    REPORT("\nC) 鼠标消息通道:\n");
    REPORT("   注入鼠标(FROMTOUCH)  : %llu   <<< 笔经 Windows Ink 变成的伪鼠标\n", (unsigned long long)g_injectMouse);
    REPORT("   普通鼠标             : %llu\n", (unsigned long long)g_plainMouse);

    REPORT("\n==================== 结论 ====================\n");
    if (g_riDigitizer > 0) {
        REPORT("★ 数位笔走 Raw Input(HID digitizer) —— 主程序当前方案正确，\n");
        REPORT("  若仍无数据，问题在解析或坐标映射，看主程序 pb.log 的 parse 记录。\n");
        // 数据落在哪个 usage 决定了主程序能否收到（PAGEONLY 注册后都能收到）
        if (g_usage01 == 0 && g_usage02 == 0 && g_usage04 == 0 && g_usage0C == 0 && g_usageOther > 0) {
            REPORT("  !! 数据落在非常规 usage 上（非 01/02/04/0C）——\n");
            REPORT("     主程序若用显式 usage 清单会完全收不到；必须用 RIDEV_PAGEONLY。\n");
        }
    } else if (g_ptrPen > 0) {
        REPORT("★ 数位笔走 WM_POINTER（Windows Ink 路径），Raw Input 收不到！\n");
        REPORT("  主程序需改用 WM_POINTER + GetPointerPenInfo 采集（含压力 %u）。\n", g_ptrMaxPressure);
    } else if (g_injectMouse > 0 && g_riDigitizer == 0 && g_ptrPen == 0) {
        REPORT("★ 只见到注入鼠标事件，未见 digitizer 也未见 POINTER-pen。\n");
        REPORT("  笔数据被系统转成了伪鼠标；主程序的注入鼠标回退路径会接管。\n");
    } else {
        REPORT("？ 三条通道都没收到笔数据。请确认：\n");
        REPORT("  - 诊断期间确实用笔在窗口内画了\n");
        REPORT("  - 数位板驱动正常（在其他软件里能用笔）\n");
        REPORT("  - 若窗口不是前台，POINTER 可能不投递；请把诊断窗口置于前台再画\n");
    }
    REPORT("================================================\n");
    REPORT("\n回传方式：把 pen_diag_report.txt 与 %%LOCALAPPDATA%%\\PaintingBuddy\\pb.log\n");
    REPORT("          一并发给开发者即可定位兼容性问题。\n");
    if (g_report) { fclose(g_report); g_report = nullptr; }

    RegisterRawInputDevices(nullptr, 0, sizeof(RAWINPUTDEVICE));
    DestroyWindow(g_hwnd);
    return 0;
}
