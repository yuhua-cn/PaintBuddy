// ============================================================
//  绘画伴侣 —— 程序入口
//
//  职责：单实例保护、DPI 感知、目录与配置初始化、
//        启动无感采集与录屏、运行界面、退出时安全收尾。
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Config.h"
#include "Capture.h"
#include "Recorder.h"
#include "MainWindow.h"
#ifdef PB_BETA
#include "Video.h"        // RecyclePath：测试发布版首次运行清理（可恢复）
#include "BetaVersion.h"  // 由 build_beta.bat 生成，定义 PB_BETA_VERSION
#endif

#include <shellapi.h>
#include <shlobj.h>      // SHGetFolderPathW：仅测试发布版的首次运行清理用到
#include <cstdio>

using namespace pb;

// ------------------------------------------------------------
//  DPI 感知：优先 Per-Monitor V2，逐级回退
// ------------------------------------------------------------
static void EnableDpiAwareness() {
    // Win10 1703+
    typedef BOOL (WINAPI *SetCtxFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetCtxFn fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) {
            // -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (fn((DPI_AWARENESS_CONTEXT)(INT_PTR)-4)) return;
            if (fn((DPI_AWARENESS_CONTEXT)(INT_PTR)-3)) return;   // PER_MONITOR_AWARE
        }
    }
    SetProcessDPIAware();
}

// ------------------------------------------------------------
//  「当前活动状态」的组装已移入 Recorder::MakeActivityInfo()，
//  这样界面里切换录屏开关后重启录屏时能用同一份实现，不必重复。
// ------------------------------------------------------------

#ifdef PB_BETA
// ------------------------------------------------------------
//  测试发布版专用：按构建版本的首次运行清理
//
//  只有 build_beta.bat（定义 PB_BETA）才会把这段编译进去。
//  build.bat 出的正式版二进制里根本不存在它，不可能误删用户数据。
//
//  背景：测试发布版与正式版**共用**同一数据目录
//  %LOCALAPPDATA%\PaintingBuddy。为了让测试者拿到干净环境
//  （不带上一个包残留的日志、设置、事件流与录屏缓存），
//  某个包在本机**第一次**运行时把整个目录移入回收站。
//
//  「第一次」以构建版本号为判据，而不是「这台机器装没装过测试版」：
//    标记文件 %LOCALAPPDATA%\PaintingBuddyBeta.version 里存着
//    上一次完成清理的版本号。与当前 PB_BETA_VERSION 相同 → 同一个包
//    的后续启动，保留数据；不同或文件不存在 → 新包首次运行，清理。
//    于是每出一个新包，测试者首启都能拿到干净环境，而反复重启
//    同一个包不会把自己的测试数据删掉。
//
//  三个刻意的设计决定：
//  1) 标记文件放在数据目录**之外**。若放进目录里，目录被删标记也没了，
//     会退化成每次启动都删。测试者想强制重新清理，删掉它即可。
//  2) 走回收站（RecyclePath）而不是永久删除。测试者本机若装着
//     正式版、目录里是真实绘画记录，误删后仍可从回收站恢复。
//  3) 必须在 EnsureDirs() **之前**执行：EnsureDirs 会重建目录，
//     而 LogF 在 g_dataDir 为空时直接 return（Store.cpp:143），
//     放在之后既删不掉新写的日志、也没法记录清理动作。
//     故清理结果先存进 g_betaNote，等 LogF 可用后再补记。
// ------------------------------------------------------------
static std::string g_betaNote;

// 读标记文件里的版本号（纯 ASCII，几十字节足够）；读不到返回空串。
static std::string BetaReadVersion(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    char buf[64] = {0};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = '\0';
    std::string s(buf);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

static bool BetaWriteVersion(const std::wstring& path, const std::string& ver) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    WriteFile(h, ver.data(), (DWORD)ver.size(), &wrote, nullptr);
    CloseHandle(h);
    return wrote == (DWORD)ver.size();
}

static void BetaFirstRunCleanup() {
    const std::string ver = PB_BETA_VERSION;

    wchar_t appdata[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata))) {
        g_betaNote = "[beta] 取不到 LOCALAPPDATA，跳过首次运行清理";
        return;
    }
    const std::wstring base = std::wstring(appdata) + L"\\PaintingBuddy";
    const std::wstring mark = std::wstring(appdata) + L"\\PaintingBuddyBeta.version";

    const std::string last = BetaReadVersion(mark);
    if (!last.empty() && last == ver) {
        g_betaNote = "[beta] 版本 " + ver + " 已在本机运行过，保留既有数据";
        return;
    }

    // 走到这里 = 本包首次运行：last 为空（从没跑过）或与新包版本不同（升级/换包）
    if (GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string err;
        g_betaNote = RecyclePath(base, err)
            ? "[beta] 版本 " + ver + " 首次运行：旧数据目录已移入回收站"
            : "[beta] 版本 " + ver + " 首次运行：清理旧数据目录失败 —— " + err;
        if (!last.empty()) g_betaNote += "（上一版本 " + last + "）";
    } else {
        g_betaNote = "[beta] 版本 " + ver + " 首次运行：无旧数据目录需要清理";
    }

    // 无论清理成功与否都写标记。目录被占用等偶发失败不该让每次启动都重试
    // 删除；需要重来时由测试者删掉标记文件。
    if (!BetaWriteVersion(mark, ver))
        g_betaNote += "；写入版本标记失败，下次启动会重新清理";
}
#endif

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    // --tray：开机自启时由注册表 Run 值带上，表示静默进托盘、不弹窗口。
    // 只认这一个参数，其余一律忽略，避免与将来可能的参数冲突。
    bool trayMode = false;
    if (lpCmdLine && wcsstr(lpCmdLine, L"--tray")) trayMode = true;

    // —— 单实例：避免两个实例同时装低级钩子造成重复记录 ——
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"PaintingBuddy_SingleInstance_Mutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND exist = FindWindowW(L"PaintingBuddyMainWnd", nullptr);
        if (exist) {
            // 与 ShowMainWindow(true) 对齐：窗口可能处于「隐藏」态（最小化即隐藏 /
            // 开机静默启动），此时 SW_RESTORE 单独不足以让它出现，必须先 SW_SHOW。
            ShowWindow(exist, SW_SHOW);
            ShowWindow(exist, SW_RESTORE);
            SetForegroundWindow(exist);
        } else {
            MessageBoxW(nullptr, L"绘画伴侣已在运行。", L"绘画伴侣", MB_OK | MB_ICONINFORMATION);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    EnableDpiAwareness();

#ifdef PB_BETA
    // 必须早于 EnsureDirs：它会重建数据目录，之后就删不掉上一版残留了。
    // 此处 LogF 还不可用（g_dataDir 为空会直接 return），故结果先暂存。
    BetaFirstRunCleanup();
#endif

    // —— 基础设施 ——
    Clock::Init();
    EnsureDirs();
    CfgReload();
    LogF("===== 绘画伴侣启动 =====");
#ifdef PB_BETA
    LogF("[beta] 测试发布版，构建版本 %s", PB_BETA_VERSION);
    LogF("%s", g_betaNote.c_str());
#endif
    LogF("[init] 数据目录 %s", DataDir().c_str());

    // —— 开机自动启动 ——
    // 自愈式同步：按配置对齐 HKCU Run 值。放在 EnsureDirs + CfgReload 之后
    // （需要 LogF 可用、需要配置已加载）。exe 换位置后从新位置启动一次即自动纠正。
    AutoStartSync(Cfg().autoStart);
    LogF("[init] 开机自启 配置=%d 注册表=%d 启动方式=%s",
         (int)Cfg().autoStart, (int)AutoStartEnabled(), trayMode ? "静默托盘" : "常规窗口");

    // —— 无感采集 ——
    bool capOk = false;
    if (Cfg().enabled) {
        capOk = Capture::I().Start();
        LogF("[init] 采集启动 %s", capOk ? "成功" : "失败");
    } else {
        LogF("[init] 采集被配置关闭");
    }

    // —— 录屏 ——
    // 总是启动录屏线程（只要系统支持），是否真正录制由 Cfg().recordVideo 控制：
    // 录屏线程每轮轮询都会读该配置，关着就不开会话。这样界面里的录屏开关
    // 只需改配置即可即时生效，不必再去启停线程（启停还要处理采集依赖与时序）。
    bool recOk = false;
    if (Recorder::I().IsSupported()) {
        recOk = Recorder::I().Start(&Recorder::MakeActivityInfo);
        LogF("[init] 录屏线程启动 %s（recordVideo=%d）",
             recOk ? "成功" : "失败", (int)Cfg().recordVideo);
    } else {
        LogF("[init] 系统不支持屏幕捕获，录屏未启用");
    }

    // —— 界面 ——
    int rc = App::I().Run(hInst, nCmdShow, trayMode);

    // —— 收尾：先停录屏（会 Finalize 当前片段），再停采集（会 Flush 缓冲）——
    LogF("[exit] 正在安全收尾…");
    if (recOk) Recorder::I().Stop();
    if (capOk) Capture::I().Stop();
    Capture::I().FlushNow();
    Events().Close();
    LogF("===== 绘画伴侣退出 =====");

    if (mutex) CloseHandle(mutex);
    return rc;
}
