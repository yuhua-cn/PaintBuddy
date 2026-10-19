// ============================================================
//  Raw Input 投递探针：实证「message-only 窗口收不到 WM_INPUT，
//  常规 popup 窗口能收到」这一根因。
//
//  方法：建两个窗口，都注册鼠标 Raw Input（RIDEV_INPUTSINK），
//  用 SendInput 注入一次绝对移动（不改变光标位置），跑消息循环，
//  统计各自收到的 WM_INPUT 数。
//  —— 与 usage page 无关，验证的是「窗口能否收到 WM_INPUT」这条管道，
//     数位笔走的是同一条管道，故结论对笔同样成立。
// ============================================================
#include <windows.h>
#include <hidsdi.h>
#include <cstdio>

static int g_msgOnlyCount = 0;
static int g_popupCount = 0;
static HWND g_msgOnly = nullptr;
static HWND g_popup = nullptr;

static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        if (h == g_msgOnly) ++g_msgOnlyCount;
        else if (h == g_popup) ++g_popupCount;
        // 必须调用 DefWindowProc 让系统清理 raw input 句柄
        return DefWindowProcW(h, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

int main() {
    HINSTANCE hi = GetModuleHandleW(nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"RawInputProbeWnd";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed %lu\n", GetLastError()); return 1; }

    // 窗口 A：message-only（旧代码的做法）
    g_msgOnly = CreateWindowExW(0, wc.lpszClassName, L"probe-msgonly", 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    // 窗口 B：常规 popup 隐藏窗口（修复后的做法）
    g_popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                              L"probe-popup", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, hi, nullptr);
    printf("msgOnly=%p popup=%p\n", (void*)g_msgOnly, (void*)g_popup);

    // 两个窗口都注册鼠标 Raw Input（INPUTSINK：非前台也收）
    RAWINPUTDEVICE rid[2];
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;   // 鼠标
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = g_msgOnly;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02;   // 鼠标
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = g_popup;
    // 注意：同一 usage 只能注册到一个目标；这里分两次注册，后者覆盖前者。
    // 为公平对比，改为分别独立注册：先只注册 msgOnly，注入并统计；再只注册 popup。
    RegisterRawInputDevices(nullptr, 0, sizeof(RAWINPUTDEVICE)); // 清空

    auto inject = []() {
        // 绝对移动到当前光标位置：不改变光标，但产生一次 raw input
        POINT p; GetCursorPos(&p);
        int vw = GetSystemMetrics(SM_CXSCREEN), vh = GetSystemMetrics(SM_CYSCREEN);
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = (vw > 0) ? (LONG)((double)p.x / vw * 65535.0) : 0;
        in.mi.dy = (vh > 0) ? (LONG)((double)p.y / vh * 65535.0) : 0;
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in.mi.time = 0;
        in.mi.dwExtraInfo = 0;
        UINT sent = SendInput(1, &in, sizeof(INPUT));
        return sent;
    };
    auto pump = [](int ms) {
        DWORD end = GetTickCount() + (DWORD)ms;
        MSG m;
        while ((int)(end - GetTickCount()) > 0) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            Sleep(5);
        }
    };

    // —— 第一轮：只注册到 message-only 窗口 ——
    RAWINPUTDEVICE r1{}; r1.usUsagePage = 0x01; r1.usUsage = 0x02;
    r1.dwFlags = RIDEV_INPUTSINK; r1.hwndTarget = g_msgOnly;
    BOOL ok1 = RegisterRawInputDevices(&r1, 1, sizeof(r1));
    printf("register->msgOnly ok=%d err=%lu\n", (int)ok1, GetLastError());
    g_msgOnlyCount = 0; g_popupCount = 0;
    UINT s1 = inject();
    pump(400);
    printf("[msgOnly-only]  SendInput=%u  msgOnly WM_INPUT=%d  popup WM_INPUT=%d\n",
           s1, g_msgOnlyCount, g_popupCount);
    int msgOnlyGot = g_msgOnlyCount;

    // 注销
    RAWINPUTDEVICE rr{}; rr.usUsagePage = 0x01; rr.usUsage = 0x02;
    rr.dwFlags = RIDEV_REMOVE; rr.hwndTarget = nullptr;
    RegisterRawInputDevices(&rr, 1, sizeof(rr));

    // —— 第二轮：只注册到 popup 窗口 ——
    RAWINPUTDEVICE r2{}; r2.usUsagePage = 0x01; r2.usUsage = 0x02;
    r2.dwFlags = RIDEV_INPUTSINK; r2.hwndTarget = g_popup;
    BOOL ok2 = RegisterRawInputDevices(&r2, 1, sizeof(r2));
    printf("register->popup   ok=%d err=%lu\n", (int)ok2, GetLastError());
    g_msgOnlyCount = 0; g_popupCount = 0;
    UINT s2 = inject();
    pump(400);
    printf("[popup-only]    SendInput=%u  msgOnly WM_INPUT=%d  popup WM_INPUT=%d\n",
           s2, g_msgOnlyCount, g_popupCount);
    int popupGot = g_popupCount;

    printf("\n==== 结论 ====\n");
    printf("message-only 窗口收到 WM_INPUT: %d\n", msgOnlyGot);
    printf("popup 窗口收到 WM_INPUT       : %d\n", popupGot);
    if (s1 == 0 && s2 == 0) {
        printf("SendInput 被环境拦截（沙箱/无交互桌面），无法在此实证；\n");
        printf("请在真实桌面运行本探针，或直接在主程序里用数位笔画一笔看 pb.log。\n");
    } else if (popupGot > 0 && msgOnlyGot == 0) {
        printf("已实证：message-only 收不到、popup 能收到 —— 与主程序修复一致。\n");
    } else if (popupGot > 0 && msgOnlyGot > 0) {
        printf("两者都收到：此环境 message-only 也能收（但主程序改 popup 更稳妥，无害）。\n");
    } else {
        printf("两者都没收到：可能注入未产生 raw input，请在真实桌面重试。\n");
    }

    RegisterRawInputDevices(&rr, 1, sizeof(rr));
    DestroyWindow(g_msgOnly);
    DestroyWindow(g_popup);
    return 0;
}
