// ============================================================
//  窗口可见性对 Raw Input 投递的影响 —— 隔离实验
//
//  背景：主程序的采集窗口是 WS_POPUP 且「从未 ShowWindow」，日志显示
//  数位笔 WM_INPUT 一次都没到达。本实验用 SendInput 注入鼠标移动，
//  对比三种窗口状态能否收到 WM_INPUT：
//    1) message-only 窗口（HWND_MESSAGE）
//    2) WS_POPUP 且 ShowWindow(SW_SHOW) 显示过
//    3) WS_POPUP 且「从未 ShowWindow」（= 主程序当前配置）
//  用鼠标是因为可用 SendInput 确定性注入；结论对「窗口能否收到
//  WM_INPUT」这条管道成立，数位笔走同一条管道。
// ============================================================
#include <windows.h>
#include <cstdio>

static HWND g_msgOnly = nullptr;
static HWND g_popupShown = nullptr;
static HWND g_popupHidden = nullptr;
static int g_cMsg = 0, g_cShown = 0, g_cHidden = 0;

static LRESULT CALLBACK P(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        if (h == g_msgOnly) ++g_cMsg;
        else if (h == g_popupShown) ++g_cShown;
        else if (h == g_popupHidden) ++g_cHidden;
        return DefWindowProcW(h, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

static void inject() {
    POINT p; GetCursorPos(&p);
    int vw = GetSystemMetrics(SM_CXSCREEN), vh = GetSystemMetrics(SM_CYSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (vw > 0) ? (LONG)((double)p.x / vw * 65535.0) : 0;
    in.mi.dy = (vh > 0) ? (LONG)((double)p.y / vh * 65535.0) : 0;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
}
static void pump(int ms) {
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG m;
    while ((int)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(5);
    }
}
static void regTo(HWND target) {
    RAWINPUTDEVICE rr{}; rr.usUsagePage = 0x01; rr.usUsage = 0x02; rr.dwFlags = RIDEV_REMOVE; rr.hwndTarget = nullptr;
    RegisterRawInputDevices(&rr, 1, sizeof(rr));
    RAWINPUTDEVICE r{}; r.usUsagePage = 0x01; r.usUsage = 0x02; r.dwFlags = RIDEV_INPUTSINK; r.hwndTarget = target;
    RegisterRawInputDevices(&r, 1, sizeof(r));
}

int main() {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = P; wc.hInstance = hi; wc.lpszClassName = L"VisProbe";
    RegisterClassW(&wc);

    g_msgOnly = CreateWindowExW(0, wc.lpszClassName, L"m", 0, 0,0,0,0, HWND_MESSAGE, nullptr, hi, nullptr);
    g_popupShown = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, wc.lpszClassName, L"s", WS_POPUP,
                                   -32000,-32000,1,1, nullptr, nullptr, hi, nullptr);
    ShowWindow(g_popupShown, SW_SHOW);   // 显示过
    g_popupHidden = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, wc.lpszClassName, L"h", WS_POPUP,
                                    0,0,0,0, nullptr, nullptr, hi, nullptr);
    // 刻意不 ShowWindow —— 复刻主程序当前配置

    printf("msgOnly=%p popupShown=%p popupHidden=%p\n", (void*)g_msgOnly,(void*)g_popupShown,(void*)g_popupHidden);

    regTo(g_msgOnly);     g_cMsg=g_cShown=g_cHidden=0; inject(); pump(350);
    printf("[1] message-only        -> WM_INPUT=%d\n", g_cMsg);

    regTo(g_popupShown);  g_cMsg=g_cShown=g_cHidden=0; inject(); pump(350);
    printf("[2] popup + SW_SHOW     -> WM_INPUT=%d\n", g_cShown);

    regTo(g_popupHidden); g_cMsg=g_cShown=g_cHidden=0; inject(); pump(350);
    printf("[3] popup 从未 Show     -> WM_INPUT=%d   <<< 主程序当前配置\n", g_cHidden);

    printf("\n==== 结论 ====\n");
    if (g_cHidden == 0 && g_cShown > 0)
        printf("★ 确认：从未 ShowWindow 的 popup 窗口收不到 WM_INPUT！\n  主程序需把采集窗口显示到屏幕外（SW_SHOWNOACTIVATE）。\n");
    else if (g_cHidden > 0)
        printf("隐藏 popup 也能收到 WM_INPUT —— 可见性不是主程序笔失效的原因，\n  需 pen_diag 进一步确认笔走哪条通道。\n");
    else
        printf("注入可能被环境拦截，三者都需看绝对值。\n");

    RAWINPUTDEVICE rr{}; rr.usUsagePage=0x01; rr.usUsage=0x02; rr.dwFlags=RIDEV_REMOVE; rr.hwndTarget=nullptr;
    RegisterRawInputDevices(&rr,1,sizeof(rr));
    DestroyWindow(g_msgOnly); DestroyWindow(g_popupShown); DestroyWindow(g_popupHidden);
    return 0;
}
