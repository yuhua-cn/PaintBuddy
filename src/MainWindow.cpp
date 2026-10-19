#include "MainWindow.h"
#include "Capture.h"
#include "Recorder.h"
#include "Store.h"
#include "Video.h"
#include "Json.h"
#include "PenOverrides.h"   // EnumDisplayNames（诊断导出用）
#include "PenInput.h"       // ScanPenLog（诊断导出：区分会话口径）
#include "resource.h"       // IDI_APP：exe 内嵌图标资源

#include <shellapi.h>
#include <shlobj.h>      // SHGetKnownFolderIDList（导出默认目录用系统视频库）
#include <shobjidl.h>    // IFileOpenDialog（选择文件夹）
#include <windowsx.h>
#include <commdlg.h>
#include <winrt/base.h>  // com_ptr
#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")

namespace pb {

// ============================================================
//  常量
// ============================================================
static const wchar_t* const PB_WNDCLASS = L"PaintingBuddyMainWnd";
static const UINT TRAY_ID = 1001;

// 布局最小可用尺寸（DIP，客户区）。
// 布局是绝对坐标硬编码（侧栏 208 + 内容区，标题栏 56），低于此值内容会被
// PushClip 直接裁掉而不会自适应重排；侧栏底部「数据仅保存在本机 / 版本号」
// 两行也依赖 bottom-52 的定位，窗口过矮会与导航项重叠。
// 用于初始窗口尺寸下限与 WM_GETMINMAXINFO 的拖动下限，二者必须同一口径。
static const int MIN_LAYOUT_W = 900;
static const int MIN_LAYOUT_H = 600;

// 设置页文本输入槽位
enum InputSlot {
    IN_KEYMERGE = 0, IN_FPS, IN_BITRATE, IN_MINCLIP,
    IN_N1, IN_N2, IN_DEPTH, IN_ADDTARGET, IN_COUNT
};

// ============================================================
//  应用图标：优先取 exe 内嵌资源，失败才回退到程序化绘制
//
//  内嵌资源（res\app.ico 编进 PE）是主路径，它同时供 Explorer 解析
//  文件图标（桌面/开始菜单/固定任务栏）与运行时加载。MakeAppIcon
//  保留为兜底：资源缺失或加载失败时窗口与托盘仍有图标，不至于崩溃
//  或显示系统默认空白图标。
//
//  两个尺寸口径：
//  · LoadAppIcon(size)  —— 按精确尺寸取图标，走 LoadImageW + LR_DEFAULTCOLOR。
//    标题栏、任务栏运行中按钮、托盘都用它，避免拿大图标降采样导致发虚。
//    注意不能用 LoadIconW：那个 API 固定返回 32×32，取不到 16/20/24。
//  · SmallIconSizeForDpi(hwnd) / LargeIconSizeForDpi(hwnd)
//    —— 按窗口当前 DPI 换算尺寸，再把结果喂给 LoadAppIcon()。
// ============================================================
static HICON MakeAppIcon(int size) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!color || !mask) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    uint32_t* px = (uint32_t*)bits;
    float cx = size * 0.5f, cy = size * 0.5f;
    float rad = size * 0.44f;
    // iOS 蓝 → 紫 的简单双色
    uint8_t cr = 0x0A, cg = 0x84, cb = 0xFF;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            uint32_t v = 0;
            if (d <= rad) {
                float t = d / rad;
                uint8_t r = (uint8_t)(cr + (0x5E - cr) * t);
                uint8_t g = (uint8_t)(cg + (0x5C - cg) * t);
                uint8_t b = (uint8_t)(cb + (0xE6 - cb) * t);
                // 笔尖斜线（白色）
                float lineD = fabsf((dx + dy) * 0.7071f);
                bool onNib = (lineD < size * 0.055f) && (dx - dy) < size * 0.10f && (dx - dy) > -size * 0.42f;
                if (onNib) { r = 255; g = 255; b = 255; }
                uint8_t a = (d > rad - 1.f) ? (uint8_t)(255.f * std::max(0.f, rad - d)) : 255;
                v = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
            px[y * size + x] = v;
        }
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.xHotspot = 0; ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    ReleaseDC(nullptr, screen);
    return icon;
}

// 按精确尺寸加载内嵌图标；资源取不到时回退到程序化绘制。
// 返回的 HICON 由调用方负责 DestroyIcon（与 MakeAppIcon 口径一致）。
static HICON LoadAppIcon(int size) {
    if (size <= 0) size = GetSystemMetrics(SM_CXSMICON);
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HICON h = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                size, size, LR_DEFAULTCOLOR);
    if (h) return h;
    // 资源缺失（例如诊断构建未编译 app.rc）：退回程序化绘制，保证有图标
    return MakeAppIcon(size);
}

// 按窗口当前 DPI 换算「小图标」尺寸（标题栏 / 托盘）。
// 不能用 GetSystemMetrics(SM_CXSMICON)：在 Per-Monitor V2 进程里它返回
// 系统级值，不反映窗口实际所在显示器，跨屏拖动后图标会被拉伸。
// GetSystemMetricsForDpi 在 Win10 1607+ 才有，动态取址以兼容更早系统，
// 与本文件 AdjustWindowRectExForDpi（见 WM_GETMINMAXINFO）做法一致。
static int SmallIconSizeForDpi(HWND hwnd) {
    const int fallback = GetSystemMetrics(SM_CXSMICON);
    UINT dpi = 96;
    if (hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn fn = (GetDpiForWindowFn)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (fn) dpi = fn(hwnd);
    }
    typedef int (WINAPI *GetMetricsForDpiFn)(int, UINT);
    static GetMetricsForDpiFn fn = (GetMetricsForDpiFn)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (!fn) return fallback;
    int v = fn(SM_CXSMICON, dpi);
    return v > 0 ? v : fallback;
}

// 取「大图标」尺寸（任务栏运行中按钮、Alt+Tab）。同样按 DPI 换算。
static int LargeIconSizeForDpi(HWND hwnd) {
    UINT dpi = 96;
    if (hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        static GetDpiForWindowFn fn = (GetDpiForWindowFn)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (fn) dpi = fn(hwnd);
    }
    typedef int (WINAPI *GetMetricsForDpiFn)(int, UINT);
    static GetMetricsForDpiFn fn = (GetMetricsForDpiFn)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (!fn) return GetSystemMetrics(SM_CXICON);
    int v = fn(SM_CXICON, dpi);
    return v > 0 ? v : GetSystemMetrics(SM_CXICON);
}

// ============================================================
//  TextInput
// ============================================================
float TextInput::CaretX(Painter* p) {
    float x = 0;
    if (p && caret_ > 0 && caret_ <= (int)text_.size())
        x = p->MeasureW(text_.substr(0, (size_t)caret_), 14.f, TWeight::Regular);
    return x;
}

void TextInput::Draw(Painter& p, const D2D1_COLOR_F& fg, const D2D1_COLOR_F& bg,
                     const D2D1_COLOR_F& border, bool readOnly) {
    lastPainter_ = &p;
    const Theme& th = p.Th();
    D2D1_RECT_F r = rect_;
    if (RW(r) <= 0 || RH(r) <= 0) return;

    p.FillRound(r, 8.f, readOnly ? bg : (focus_ ? bg : bg));
    p.StrokeRound(r, 8.f, focus_ ? th.accent : border, focus_ ? 2.f : 1.f);

    D2D1_RECT_F inner = Inset(r, 10.f, 0.f);
    p.PushClip(inner);
    float cx = CaretX(&p);
    // 水平滚动，保证光标可见
    float visW = RW(inner);
    if (cx - (float)scroll_ > visW - 4.f) scroll_ = (int)(cx - visW + 8.f);
    if (cx - (float)scroll_ < 0) scroll_ = (int)std::max(0.f, cx - 4.f);

    p.Text(text_, RF(inner.left - (float)scroll_, inner.top, inner.right + 200.f, inner.bottom),
           14.f, TWeight::Regular, text_.empty() ? th.label3 : fg,
           TAlign::Left, TVAlign::Center, false);

    if (focus_ && !readOnly) {
        float bx = inner.left + cx - (float)scroll_;
        p.Line(bx, inner.top + 6.f, bx, inner.bottom - 6.f, th.accent, 1.5f);
    }
    p.PopClip();
}

bool TextInput::OnClick(float x, float y) {
    if (!PtIn(x, y, rect_)) { focus_ = false; return false; }
    focus_ = true;
    if (lastPainter_) {
        float rel = x - rect_.left - 10.f + (float)scroll_;
        // 线性扫描找最近插入点
        float best = 1e9f;
        caret_ = 0;
        for (int i = 0; i <= (int)text_.size(); ++i) {
            float w = lastPainter_->MeasureW(text_.substr(0, (size_t)i), 14.f, TWeight::Regular);
            float d = fabsf(w - rel);
            if (d < best) { best = d; caret_ = i; }
        }
    } else {
        caret_ = (int)text_.size();
    }
    return true;
}

bool TextInput::OnChar(WPARAM ch) {
    if (!focus_) return false;
    wchar_t c = (wchar_t)ch;
    if (c < 0x20) return false;
    if (c == 0x7F) return false;
    text_.insert(text_.begin() + caret_, c);
    ++caret_;
    return true;
}

bool TextInput::OnKey(WPARAM vk, LPARAM lp) {
    if (!focus_) return false;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    (void)lp;
    switch (vk) {
    case VK_BACK:
        if (caret_ > 0) { text_.erase(text_.begin() + caret_ - 1); --caret_; return true; }
        return false;
    case VK_DELETE:
        if (caret_ < (int)text_.size()) { text_.erase(text_.begin() + caret_); return true; }
        return false;
    case VK_LEFT:
        if (caret_ > 0) --caret_;
        return true;
    case VK_RIGHT:
        if (caret_ < (int)text_.size()) ++caret_;
        return true;
    case VK_HOME: caret_ = 0; return true;
    case VK_END:  caret_ = (int)text_.size(); return true;
    case 'A':
        if ((GetKeyState(VK_CONTROL) & 0x8000)) { caret_ = 0; text_.clear(); return true; }
        break;
    case 'C':
        if ((GetKeyState(VK_CONTROL) & 0x8000)) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                std::wstring s = text_;
                size_t bytes = (s.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h) {
                    void* p = GlobalLock(h);
                    if (p) { memcpy(p, s.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
                }
                CloseClipboard();
            }
            return true;
        }
        break;
    case 'V':
        if ((GetKeyState(VK_CONTROL) & 0x8000)) {
            if (OpenClipboard(nullptr)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* s = (const wchar_t*)GlobalLock(h);
                    if (s) { text_.insert((size_t)caret_, s); caret_ += (int)wcslen(s); }
                    GlobalUnlock(h);
                }
                CloseClipboard();
            }
            return true;
        }
        break;
    default: break;
    }
    (void)shift;
    return false;
}

// ============================================================
//  App
// ============================================================
App& App::I() {
    static App inst;
    return inst;
}

LRESULT CALLBACK App::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (App*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
        self->OnCreate(h);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (self) self->OnPaint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 全自绘，交给 D2D
    case WM_SIZE:
        // 「最小化即隐藏」兜底路径：Win+M / Win+D / 点任务栏按钮 不经
        // WM_SYSCOMMAND，直接送来 SIZE_MINIMIZED。这里改成隐藏并**不再往下调
        // OnSize** —— 顺带避免 OnSize(0,0)（最小化时 LOWORD/HIWORD(lp) 皆为 0，
        // 会把布局尺寸算成 0）。刻意不做成开关：与「无感记录」定位一致，
        // 也少一个状态要维护。
        if (self && wp == SIZE_MINIMIZED) { self->ShowMainWindow(false); return 0; }
        if (self) self->OnSize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DPICHANGED:
        if (self) {
            self->OnDpiChanged(LOWORD(wp));
            RECT* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        // 限制最小可拖动尺寸：布局为绝对坐标硬编码，窗口过小内容会被裁掉。
        // ptMinTrackSize 用窗口所在显示器的物理像素，故把 DIP 下限按当前
        // 窗口 DPI 放大，再换算成外框尺寸（含边框/标题栏）。跨屏拖动时
        // WM_DPICHANGED 会重新触发本消息，取值始终跟随当前显示器。
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        float sc = (float)WindowDpi(h) / 96.f;
        RECT rc{ 0, 0, (LONG)(MIN_LAYOUT_W * sc), (LONG)(MIN_LAYOUT_H * sc) };
        typedef BOOL (WINAPI *AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static AdjustForDpiFn adjFn = (AdjustForDpiFn)(void*)GetProcAddress(
            GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
        if (adjFn) adjFn(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)(96.f * sc));
        else       AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_TIMER:
        if (self) self->OnTimer(wp);
        return 0;
    case WM_MOUSEWHEEL:
        if (self) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt);
            // ScreenToClient 结果同为物理像素，与 OnMouse 一样需换算成 DIP，
            // 否则缩放 >100% 时滚轮会落在错误的区域（甘特区缩放失效/页面滚不动）。
            self->OnWheel(self->ToDipX(pt.x), self->ToDipY(pt.y), (float)GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
        if (self) return self->OnMouse(msg, wp, lp);
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_KEYUP:   case WM_SYSKEYUP:
        if (self) return self->OnKeyMsg(msg, wp, lp);
        break;
    case WM_CHAR:
        if (self) {
            if (self->focusInput_ >= 0 && self->focusInput_ < IN_COUNT) {
                if (self->inputs_[(size_t)self->focusInput_].OnChar(wp)) { self->Invalidate(); return 0; }
            }
            if (wp == 0x1B) { self->CommitInputs(); self->Invalidate(); return 0; }
        }
        break;
    case WM_SETFOCUS:
        if (self) self->Invalidate();
        break;
    case WM_KILLFOCUS:
        if (self) { self->CommitInputs(); self->Invalidate(); }
        break;
    case WM_ANALYSIS_DONE:
        if (self) { self->OnAnalysisDone(); self->Invalidate(); }
        return 0;
    case WM_JOB_DONE:
        // 录屏后台任务（缩略图 / 导出）完成：从完成队列排空全部结果
        if (self) { self->OnJobDone(); self->Invalidate(); }
        return 0;
    case WM_TRAY:
        if (self && LOWORD(lp) == WM_LBUTTONDBLCLK) { self->ShowMainWindow(true); return 0; }
        if (self && LOWORD(lp) == WM_RBUTTONUP) { self->ShowTrayMenu(); return 0; }
        return 0;
    case WM_SYSCOMMAND:
        // 「最小化即隐藏」主路径：系统标题栏的最小化按钮走这里。
        // 必须用 (wp & 0xFFF0) 掩码比较 —— SC_* 常量的低 4 位是系统保留给
        // 内部使用的，直接相等比较在某些情况下会漏判。
        // 直接隐藏且不让窗口真正进入最小化态，任务栏按钮不会闪一下再消失。
        if (self && (wp & 0xFFF0) == SC_MINIMIZE) { self->ShowMainWindow(false); return 0; }
        break;
    case WM_CLOSE:
        // 明确询问：退出程序，还是最小化到托盘继续无感记录
        if (self) {
            self->CommitInputs();
            int r = MessageBoxW(h,
                L"退出后记录与录屏将停止。\n\n「是」= 退出程序\n「否」= 最小化到托盘，继续记录",
                L"绘画伴侣", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (r == IDYES) {
                DestroyWindow(h);
            } else {
                self->ShowMainWindow(false);
            }
        }
        return 0;
    case WM_DESTROY:
        if (self) self->OnDestroy();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================================================
//  窗口生命周期
// ============================================================
void App::OnCreate(HWND hwnd) {
    // 真实 DPI：让 1px 发丝线与字号在高分屏上保持视觉一致。
    // 走 WindowDpi() 与 Painter::Create 取同一个值；Painter::Create 内部也会
    // 自行取一次，这里的 SetDpi 主要为覆盖 WM_DPICHANGED 之后的跨屏场景。
    float dpi = (float)WindowDpi(hwnd);

    dark_ = Cfg().darkMode;
    theme_ = dark_ ? Theme::Dark() : Theme::Light();
    painter_.SetTheme(theme_);
    if (!painter_.Create(hwnd)) {
        LogF("[ui] 渲染器初始化失败");
    }
    painter_.SetDpi(dpi);

    tab_ = Cfg().lastTab;
    if (tab_ < 0 || tab_ > 3) tab_ = 0;

    for (int i = 0; i < IN_COUNT; ++i) inputs_.push_back(TextInput());
    SyncInputsFromConfig();

    // 标签顺序必须与 HabitView::Panel 枚举序号一致（点击按索引直接转成 Panel）
    habitTabs_ = { L"习惯概览", L"一阶转移", L"长程关联", L"二阶条件" };

    // 窗口类注册时按系统 DPI（取不到窗口时按 96）给的图标，在本机缩放
    // 非 100% 时尺寸并不对（150% 下小图标该是 24px 而非 16px）。这里窗口
    // 已存在、能取到真实 DPI，先按它重取一次 per-window 图标。
    // 必须放在 TrayCreate 之前：本函数内的托盘分支以 trayActive_ 为条件，
    // 此时尚未建托盘，就不会重复重建一次托盘图标。
    RefreshIconsForDpi();

    TrayCreate();
    SetTimer(hwnd, TID_POLL, 500, nullptr);
    SetTimer(hwnd, TID_ANIM, 530, nullptr);

    // 首次分析：默认看「全部历史」，让习惯阈值有足够样本
    RefreshDayList();
    useFullHistory_ = true;
    RequestAnalysis(true);

    // 初始状态
    captureOn_ = Capture::I().IsRunning();
}

void App::OnDestroy() {
    if (hwnd_) { KillTimer(hwnd_, TID_POLL); KillTimer(hwnd_, TID_ANIM); KillTimer(hwnd_, TID_FADE); }
    fadeTimerOn_ = false;

    // 先停后台工作线程，再销毁绘图器：位图缓存随绘图器一起释放，
    // 若线程还在跑就可能触碰已释放的渲染目标资源。
    StopWorker();

    TrayDestroy();

    // 销毁本对象通过 WM_SETICON 挂上去的 per-window 图标。
    // 窗口类注册时那两个（wc.hIcon/wc.hIconSm）不在这里销毁：类图标随
    // 进程存续，且窗口类只注册一次；销毁它会让同类窗口拿到悬空句柄。
    if (iconLarge_) { DestroyIcon(iconLarge_); iconLarge_ = nullptr; }
    if (iconSmall_) { DestroyIcon(iconSmall_); iconSmall_ = nullptr; }

    CommitInputs();
    Cfg().lastTab = tab_;
    CfgSave();
    painter_.Destroy();
}

// 停止后台工作线程（幂等，可被 OnDestroy 与析构函数重复调用）。
// join() 没有超时，所以先置取消标志让 ConcatExport 在每帧的进度回调里
// 尽快返回，使等待真正有界；再唤醒可能在等待新任务的工作线程。
void App::StopWorker() {
    if (!workerStarted_.exchange(false)) return;
    exportCancel_.store(1);
    workerQuit_.store(true);
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobQueue_.clear();
        jobDone_.clear();
    }
}

// 兜底回收：离屏预览（InitForHeadlessPreview + RenderPagePng）从不建窗口，
// OnDestroy 不会执行；若工作线程仍 joinable，进程退出时 std::thread 析构会
// 调用 std::terminate（0xC0000409）。这里保证任何路径都能安全收尾。
App::~App() {
    StopWorker();
}

void App::OnSize(UINT w, UINT h) {
    painter_.Resize(w, h);
    Invalidate();
}

void App::OnDpiChanged(UINT dpi) {
    if (dpi > 0) painter_.SetDpi((float)dpi);
    RefreshIconsForDpi();
    Invalidate();
}

// 按窗口当前 DPI 重取三处图标：标题栏小图标、任务栏大图标、托盘图标。
//
// 所有权口径（易错，改动前先读）：
//   WM_SETICON 的返回值是「被替换下来的那个图标」。若窗口此前从未设过
//   per-window 图标，返回的是**窗口类**的图标句柄 —— 销毁它会让窗口类的
//   hIcon/hIconSm 字段悬空，后续新建同类窗口就拿到已释放的句柄。
//   因此这里一律不碰返回值，只销毁 iconLarge_/iconSmall_ 这两个由本对象
//   创建的句柄；二者为 nullptr 时说明当前用的还是类图标，跳过销毁。
void App::RefreshIconsForDpi() {
    if (!hwnd_) return;

    HICON big = LoadAppIcon(LargeIconSizeForDpi(hwnd_));
    // 变量名不能叫 small：windows.h 里有 #define small char（RPC 头文件遗留），
    // 写成 HICON small 会展开成 "HICON char" 直接编译失败（C2628/C2062）。
    HICON sml = LoadAppIcon(SmallIconSizeForDpi(hwnd_));
    if (!big && !sml) return;   // 两次都失败：保留现有图标，别把窗口弄成无图标

    if (big) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (iconLarge_) DestroyIcon(iconLarge_);
        iconLarge_ = big;
    }
    if (sml) {
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)sml);
        if (iconSmall_) DestroyIcon(iconSmall_);
        iconSmall_ = sml;
    }

    // 托盘图标同样按新 DPI 重建。NIM_MODIFY 而非删了重加：
    // 重加会让托盘图标闪一下并重置通知区域的排序位置。
    if (trayActive_) {
        HICON tray = LoadAppIcon(SmallIconSizeForDpi(hwnd_));
        if (tray) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = TRAY_ID;
            nid.uFlags = NIF_ICON;
            nid.hIcon = tray;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            if (trayIcon_) DestroyIcon(trayIcon_);
            trayIcon_ = tray;
        }
    }
}

void App::OnTimer(WPARAM id) {
    if (id == TID_POLL) {
        // 仅在概览/录屏页轮询实时状态，避免无谓刷新
        if (tab_ == 0 || tab_ == 2) {
            int64_t now = Clock::NowUs();
            if (now - lastStatusPollUs_ > 400000) {
                lastStatusPollUs_ = now;
                bool changed = false;
                bool running = Capture::I().IsRunning();
                if (running != captureOn_) { captureOn_ = running; changed = true; }
                if (!analysisBusy_.load() && !dataReady_) changed = true;
                if (changed) Invalidate();
                else Invalidate();   // 实时计数需要刷新
            }
        }

        // —— 自动刷新图表（总览 / 习惯两页共用同一份分析结果）——
        // 采集层不主动推送「新操作」，这里轮询「最近一次操作时刻」水位线：
        // 只要它推进（= 有新操作落盘），且距上次刷新超过节流间隔、后台不忙，
        // 就触发一次后台重算。这样连续作画时两个图表会持续跟着更新，
        // 而不是像此前那样纹丝不动。节流 2s，避免高频作画时反复重算。
        if (tab_ == 0 || tab_ == 1) {
            int64_t nowUs = Clock::NowUs();
            int64_t opUs = Capture::I().LastOpUs();
            if (opUs > lastRefreshOpUs_ &&
                !analysisBusy_.load() &&
                (nowUs - lastAutoRefreshUs_) >= AUTO_REFRESH_MIN_GAP_US) {
                lastAutoRefreshUs_ = nowUs;
                lastRefreshOpUs_ = opUs;
                RefreshDayList();            // 可能跨到新的一天
                RequestAnalysis(useFullHistory_);
            }
        }
    } else if (id == TID_ANIM) {
        if (focusInput_ >= 0) Invalidate();   // 光标闪烁
    } else if (id == TID_FADE) {
        // 推进总览图的概括↔明细淡入淡出。重绘后 OnPaint 会重新评估
        // Animating()，动画一结束就把本定时器关掉。
        if (gantt_.Animating()) Invalidate();
        else {
            if (hwnd_) KillTimer(hwnd_, TID_FADE);
            fadeTimerOn_ = false;
        }
    } else if (id == TID_TRAY_BUBBLE) {
        // 开机自启的一次性气泡：延迟到 explorer 就绪后再弹，弹完立刻关定时器。
        if (hwnd_) KillTimer(hwnd_, TID_TRAY_BUBBLE);
        TrayBalloon(L"绘画伴侣已在后台运行",
                    L"记录与录屏照常进行。点托盘图标可打开界面，右键可退出。");
    }
}

// ============================================================
//  布局
// ============================================================
D2D1_RECT_F App::ContentRect() const {
    float w = painter_.W(), h = painter_.H();
    return RF(sideW_, titleH_, w, h);
}
D2D1_RECT_F App::TitleRect() const {
    return RF(sideW_, 0.f, painter_.W(), titleH_);
}
D2D1_RECT_F App::SideRect() const {
    return RF(0.f, 0.f, sideW_, painter_.H());
}

// ============================================================
//  绘制
// ============================================================
void App::OnPaint() {
    if (!painter_.Valid()) return;
    painter_.SetTheme(theme_);
    painter_.Begin(&theme_.bg);

    DrawSidebar(SideRect());
    DrawTitleBar(TitleRect());

    D2D1_RECT_F content = ContentRect();
    painter_.PushClip(content);
    float used = 0.f;
    switch (tab_) {
    case 0: used = DrawOverview(content); break;
    case 1: used = DrawHabit(content);    break;
    case 2: used = DrawRecord(content);   break;
    case 3: used = DrawSettings(content); break;
    default: break;
    }
    contentH_ = used;
    painter_.PopClip();

    DrawScrollbar(content, contentH_);

    if (analysisBusy_.load()) {
        D2D1_RECT_F r = RF(content.right - 150.f, content.top + 10.f, content.right - 12.f, content.top + 36.f);
        painter_.FillRound(r, 13.f, theme_.accentSoft);
        painter_.Text(L"正在统计…", r, 12.f, TWeight::Medium, theme_.accent, TAlign::Center, TVAlign::Center);
    }

    painter_.End();

    // 淡入淡出动画驱动：仅在总览页且动画进行中才开启定时器，
    // 动画一结束立即关闭，避免常态空转耗电。
    bool needFade = (tab_ == 0) && gantt_.Animating();
    if (needFade && !fadeTimerOn_) {
        if (hwnd_) SetTimer(hwnd_, TID_FADE, FADE_FRAME_MS, nullptr);
        fadeTimerOn_ = true;
    } else if (!needFade && fadeTimerOn_) {
        if (hwnd_) KillTimer(hwnd_, TID_FADE);
        fadeTimerOn_ = false;
    }
}

void App::DrawSidebar(const D2D1_RECT_F& r) {
    // 侧边栏底色比内容区更深，形成 iOS 分栏层次
    painter_.FillRect(r, theme_.dark ? ColHex(0x1C1C1E) : ColHex(0xE9E9EE));

    // 顶部品牌区
    float top = r.top + 16.f;
    painter_.FillRound(RF(r.left + 16.f, top, r.left + 44.f, top + 28.f), 8.f, theme_.accent);
    painter_.Text(L"绘", RF(r.left + 16.f, top, r.left + 44.f, top + 28.f), 15.f,
                  TWeight::Semibold, ColHex(0xFFFFFF), TAlign::Center, TVAlign::Center);
    painter_.Text(L"绘画伴侣", RF(r.left + 52.f, top, r.right - 12.f, top + 28.f), 15.f,
                  TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);

    // 采集状态徽标
    float badgeY = top + 40.f;
    bool rec = Recorder::I().Snapshot().capturing;
    bool on = Capture::I().IsRunning();
    D2D1_COLOR_F dotCol = rec ? theme_.cUndo : (on ? ColHex(0x34C759) : theme_.label3);
    painter_.Circle(r.left + 22.f, badgeY + 7.f, 4.f, dotCol);
    painter_.Text(rec ? L"录制中" : (on ? L"记录中" : L"已停止"),
                  RF(r.left + 32.f, badgeY, r.right - 12.f, badgeY + 15.f), 12.f,
                  TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center);

    // 导航项
    navRects_.clear();
    struct NavItem { const wchar_t* name; const wchar_t* icon; };
    NavItem items[] = {
        { L"总览", L"◷" }, { L"习惯", L"◈" }, { L"录屏", L"◉" }, { L"设置", L"⚙" },
    };
    float ny = badgeY + 34.f;
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F ir = RF(r.left + 10.f, ny, r.right - 10.f, ny + 36.f);
        navRects_.push_back(ir);
        bool sel = (tab_ == i);
        if (sel) painter_.FillRound(ir, 8.f, theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xFFFFFF));
        painter_.Text(items[i].icon, RF(ir.left + 10.f, ir.top, ir.left + 30.f, ir.bottom), 14.f,
                      TWeight::Regular, sel ? theme_.accent : theme_.label2, TAlign::Left, TVAlign::Center);
        painter_.Text(items[i].name, RF(ir.left + 34.f, ir.top, ir.right - 10.f, ir.bottom), 14.f,
                      sel ? TWeight::Semibold : TWeight::Regular,
                      sel ? theme_.label : theme_.label2, TAlign::Left, TVAlign::Center);
        ny += 40.f;
    }

    // 底部：数据目录、更新预告、反馈群与版本。
    // 行高 15 DIP 共 4 行，注解区总高 78 DIP（分隔线上方留 22 DIP 间距）。
    // 侧栏宽 208 DIP，文本框宽 184 DIP。反馈群那行最长，按 11 DIP 字号
    // 估算约 180 DIP，余量极小——单行 Text 超出是按省略号截断而非换行，
    // 一旦截断群号就残缺。故该行单独用 10 DIP 字号（约 164 DIP）留足余量。
    float by = r.bottom - 78.f;
    painter_.Line(r.left + 12.f, by, r.right - 12.f, by, theme_.separator, 1.f);
    painter_.Text(L"数据仅保存在本机", RF(r.left + 12.f, by + 4.f, r.right - 12.f, by + 19.f), 11.f,
                  TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
    painter_.Text(L"后续支持联机桌宠", RF(r.left + 12.f, by + 19.f, r.right - 12.f, by + 34.f), 11.f,
                  TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
    painter_.Text(L"后续更新和反馈QQ群：1130498198", RF(r.left + 12.f, by + 34.f, r.right - 12.f, by + 49.f), 10.f,
                  TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
    // 版本行：左侧「版本 1.8 · Win64」常规色，右侧紧跟可点击的「@虞花」链接。
    // 链接用强调色并测实际宽度写入 authorLinkRect_，供悬停光标与点击命中。
    {
        D2D1_RECT_F vr = RF(r.left + 12.f, by + 49.f, r.right - 12.f, by + 64.f);
        const std::wstring ver = L"版本 1.8 · Win64 ";
        painter_.Text(ver, vr, 11.f, TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
        float vx = vr.left + painter_.MeasureW(ver, 11.f, TWeight::Regular);
        const std::wstring link = L"@虞花";
        float lw = painter_.MeasureW(link, 11.f, TWeight::Medium);
        authorLinkRect_ = RF(vx, vr.top, vx + lw, vr.bottom);
        painter_.Text(link, authorLinkRect_, 11.f, TWeight::Medium, theme_.accent,
                      TAlign::Left, TVAlign::Center);
    }
}

void App::DrawTitleBar(const D2D1_RECT_F& r) {
    painter_.FillRect(r, theme_.card);
    painter_.Line(r.left, r.bottom - 1.f, r.right, r.bottom - 1.f, theme_.separator, 1.f);

    static const wchar_t* titles[] = { L"总览图", L"习惯 · 行为序列关联",
                                       L"录屏 · 仅操作时段", L"设置" };
    static const wchar_t* subs[] = {
        L"深色为深度绘画，浅色为浅度绘画；打断超过 1 小时按 1 小时显示",
        L"普通操作 / 撤回 / 画笔 / 普通间隔 / 暂停绘画 的转移概率与长程关联",
        L"当且仅当你在目标软件中操作时录制，间隔与暂停不进入视频",
        L"目标软件、合并窗口、阈值与录屏参数",
    };
    painter_.Text(titles[tab_], RF(r.left + 20.f, r.top + 8.f, r.left + 520.f, r.top + 30.f),
                  17.f, TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);
    painter_.Text(subs[tab_], RF(r.left + 20.f, r.top + 30.f, r.right - 320.f, r.bottom - 4.f),
                  11.f, TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center, true);

    // 右侧操作按钮
    titleBtnRects_.clear();
    struct Btn { const wchar_t* t; int id; };
    std::vector<Btn> btns;
    if (tab_ == 0) {
        btns.push_back({ L"适应", 1 });
        btns.push_back({ L"放大", 2 });
        btns.push_back({ L"缩小", 3 });
        btns.push_back({ L"重新统计", 4 });
    } else if (tab_ == 1) {
        btns.push_back({ L"重新统计", 4 });
    } else if (tab_ == 3) {
        btns.push_back({ L"应用", 6 });
        btns.push_back({ L"恢复默认", 7 });
    }
    btns.push_back({ dark_ ? L"浅色" : L"深色", 8 });

    float bw = 0;
    for (const auto& b : btns) bw = std::max(bw, painter_.MeasureW(b.t, 12.f, TWeight::Medium) + 26.f);
    float x = r.right - 16.f;
    for (size_t i = btns.size(); i-- > 0; ) {
        float w = bw;
        x -= w;
        D2D1_RECT_F br = RF(x, r.top + (RH(r) - 28.f) * 0.5f, x + w, r.top + (RH(r) + 28.f) * 0.5f);
        bool primary = (btns[i].id == 4 || btns[i].id == 6);
        painter_.FillRound(br, 7.f, primary ? theme_.accent : (theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xF2F2F7)));
        painter_.Text(btns[i].t, br, 12.f, TWeight::Medium,
                      primary ? ColHex(0xFFFFFF) : theme_.label, TAlign::Center, TVAlign::Center);
        titleBtnRects_.push_back(br);
        titleBtnIds_.push_back(btns[i].id);
        x -= 8.f;
    }
    // 反转以恢复与 btns 相同顺序（上面是右→左绘制）
    std::reverse(titleBtnRects_.begin(), titleBtnRects_.end());
    std::reverse(titleBtnIds_.begin(), titleBtnIds_.end());
}

void App::DrawScrollbar(const D2D1_RECT_F& content, float contentH) {
    float viewH = RH(content);
    if (contentH <= viewH + 1.f) return;
    float trackTop = content.top + 4.f;
    float trackH = viewH - 8.f;
    float ratio = std::min(1.f, std::max(0.05f, viewH / contentH));
    float thumbH = trackH * ratio;
    float maxScroll = contentH - viewH;
    float pos = maxScroll > 0 ? (scrollY_ / maxScroll) * (trackH - thumbH) : 0.f;
    D2D1_RECT_F tr = RF(content.right - 10.f, trackTop, content.right - 3.f, trackTop + trackH);
    painter_.FillRound(tr, 3.5f, theme_.dark ? Col(255, 255, 255, 0.08f) : Col(0, 0, 0, 0.06f));
    D2D1_RECT_F th = RF(tr.left, trackTop + pos, tr.right, trackTop + pos + thumbH);
    painter_.FillRound(th, 3.5f, theme_.dark ? Col(255, 255, 255, 0.35f) : Col(0, 0, 0, 0.28f));
    scrollTrackRect_ = tr;
    scrollThumbRect_ = th;
    scrollMax_ = maxScroll;
    scrollTrackTop_ = trackTop;
    scrollTrackH_ = trackH;
    scrollThumbH_ = thumbH;
}

// ============================================================
//  页：总览
// ============================================================
float App::DrawOverview(const D2D1_RECT_F& r) {
    float x = r.left + 20.f;
    float y = r.top + 18.f - scrollY_;
    float w = RW(r) - 40.f - 10.f;

    CaptureCounters cc = Capture::I().Snapshot();
    RecorderStats rs = Recorder::I().Snapshot();

    // —— 实时状态卡片 ——
    {
        float tileW = (w - 36.f) / 4.f;
        auto tile = [&](int i, const std::wstring& t, const std::wstring& v,
                        const std::wstring& s, const D2D1_COLOR_F& ac) {
            D2D1_RECT_F tr = RF(x + (float)i * (tileW + 12.f), y, x + (float)i * (tileW + 12.f) + tileW, y + 88.f);
            painter_.StatTile(tr, t, v, s, ac);
        };
        // —— 磁贴 0「今日绘画」：今天的作画时长（时/分/秒固定格式）——
        // 口径 = 分析层作画块与今天 [0点, 现在] 的交集之和，与甘特图、
        // 汇总卡完全一致；跨天块会被裁到当天边界内。
        int64_t nowUs = Clock::NowUs();
        int64_t dayBegin = DayStartUs(nowUs);
        {
            int64_t todayPaintUs = 0;
            for (const auto& b : analysis_.blocks) {
                int64_t s = std::max<int64_t>(b.start, dayBegin);
                int64_t e = std::min<int64_t>(b.end, nowUs);
                if (e > s) todayPaintUs += (e - s);
            }
            tile(0, L"今日绘画", FormatDurationHM(todayPaintUs),
                 cc.targetActive ? L"正在记录" : L"不在目标软件，不记录",
                 cc.targetActive ? ColHex(0x34C759) : theme_.label3);
        }
        // —— 磁贴 1「今日摸鱼」：今天真实记录的「暂停绘画」次数与总时长 ——
        // 直接从已加载的分析结果里按今天的时间范围筛，因此与图表口径一致；
        // 数位设备的识别状态不再占总览磁贴（诊断信息在录屏页与 pb.log 里看）。
        {
            int todayPause = 0;
            int64_t totalPauseUs = 0;
            for (const auto& g : analysis_.gaps) {
                if (g.type != GapType::Pause) continue;
                if (g.start < dayBegin || g.start > nowUs) continue;
                ++todayPause;
                int64_t e = std::min<int64_t>(g.end, nowUs);   // 未结束的暂停裁到现在
                totalPauseUs += (e - g.start);
            }
            std::wstring sub = (todayPause > 0)
                ? (L"总时长 " + FormatDurationUs(totalPauseUs))
                : L"今天还没有暂停记录";
            tile(1, L"今日摸鱼", std::to_wstring(todayPause) + L" 次", sub,
                 todayPause > 0 ? theme_.cPause : theme_.label3);
        }

        // —— 今日操作：同样取自分析层（已合并），不再读采集层原始计数器 ——
        // 读原始计数器会绕过 MergeOps：一次滚轮甩动在系统层是几十条消息，
        // 于是磁贴显示几十次，而图表只显示一次 —— 两个数字自相矛盾。
        // 代价：数值跟随后台自动刷新（约 2 秒节流），不再逐帧实时跳动；
        // 换来的是与图表、习惯视图完全一致的口径。
        {
            int todayOps = 0, todayUndo = 0, todayBrush = 0, todayNormal = 0;
            for (const auto& o : analysis_.ops) {
                if (o.t < dayBegin || o.t > nowUs) continue;
                ++todayOps;
                if (o.kind == OpKind::Undo || o.kind == OpKind::Redo) ++todayUndo;
                else if (o.kind == OpKind::Brush)                     ++todayBrush;
                else                                                  ++todayNormal;
            }
            std::wstring sub = (todayOps > 0)
                ? (L"撤回/重做 " + std::to_wstring(todayUndo) +
                   L" · 画笔 " + std::to_wstring(todayBrush) +
                   L" · 普通 " + std::to_wstring(todayNormal))
                : L"今天还没有操作记录";
            tile(2, L"今日操作（已合并）", std::to_wstring(todayOps), sub,
                 todayOps > 0 ? theme_.accent : theme_.label3);
        }
        tile(3, L"录屏", rs.capturing ? L"录制中" : (rs.supported ? L"待机" : L"不支持"),
             rs.capturing ? L"已写入 " + FormatNumber((double)rs.framesWritten, 0) + L" 帧"
                          : L"跳过空闲帧 " + FormatNumber((double)rs.framesSkipped, 0),
             rs.capturing ? theme_.cUndo : theme_.label3);
        y += 100.f;
    }

    // —— 阈值说明 ——
    {
        // 正文改用 TextWrap（自动换行）：Text 是单行 + 省略号截断，
        // 而这段文案有 5 个分项，窄窗口下会被截掉尾部（含活动合并窗口）。
        // 高度 = 上下内边距 24 + 标题 20 + 两行正文 30 + 余量。
        float h = 82.f;
        D2D1_RECT_F cr = RF(x, y, x + w, y + h);
        painter_.Card(cr, 10.f);
        D2D1_RECT_F in = Inset(cr, 16.f, 12.f);
        painter_.Text(L"分类阈值（由你的操作习惯自动统计得出，可在设置页固定）",
                      RF(in.left, in.top, in.right, in.top + 18.f), 12.f, TWeight::Semibold,
                      theme_.label, TAlign::Left, TVAlign::Center);
        wchar_t b[512];
        // n1 已改为「暂停绘画」下限（几十秒量级），活动合并窗口是独立的 mergeSec。
        swprintf_s(b, L"正常作画 < %.0f 秒   ·   %.0f 秒 ≤ 暂停绘画 < %.0f 秒   ·   ≥ %.0f 秒 = 打断绘画   ·   活动合并窗口 %.2f 秒   ·   深度绘画：块内典型间隔 ≤ %.2f 秒",
                   analysis_.th.n1Sec, analysis_.th.n1Sec, analysis_.th.n2Sec,
                   analysis_.th.n2Sec, analysis_.th.mergeSec, analysis_.th.depthGapSec);
        painter_.TextWrap(std::wstring(b), RF(in.left, in.top + 20.f, in.right, in.bottom),
                          11.f, TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Top);
        y += h + 14.f;
    }

    // —— 图例 ——
    {
        D2D1_RECT_F lr = RF(x, y, x + w, y + 22.f);
        GanttView::DrawLegend(painter_, lr, theme_);
        y += 30.f;
    }

    // —— 甘特图 ——
    {
        D2D1_RECT_F gr = RF(x, y, x + w, y + 30.f);
        painter_.Text(L"作画时间线", gr, 15.f, TWeight::Semibold, theme_.label,
                      TAlign::Left, TVAlign::Center);
        if (!dataReady_ || gantt_.Empty()) {
            painter_.Text(L"滚轮以鼠标为中心缩放 · 拖拽平移 · 放大后显示逐笔操作", RF(x + 120.f, y, x + w, y + 30.f),
                          11.f, TWeight::Regular, theme_.label3, TAlign::Right, TVAlign::Center);
        }
        y += 34.f;

        ganttRect_ = RF(x, y, x + w, y + ganttContentH());
        painter_.Card(ganttRect_, 10.f);
        D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
        gantt_.Draw(painter_, inner, ganttScroll_);
        y += RH(ganttRect_) + 16.f;
    }

    // —— 时段汇总 ——
    {
        float h = 108.f;
        D2D1_RECT_F cr = RF(x, y, x + w, y + h);
        painter_.Card(cr, 10.f);
        D2D1_RECT_F in = Inset(cr, 16.f, 12.f);
        painter_.Text(L"本次统计汇总", RF(in.left, in.top, in.right, in.top + 18.f), 12.f,
                      TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);

        int64_t realSpan = analysis_.spanEnd > analysis_.spanStart
                           ? analysis_.spanEnd - analysis_.spanStart : 0;
        double eff = realSpan > 0 ? (double)analysis_.activePaintUs / (double)realSpan : 0.0;

        auto kv = [&](int col, int row, const std::wstring& k, const std::wstring& v,
                      const D2D1_COLOR_F& c) {
            float cw = (RW(in) - 24.f) / 3.f;
            float kx = in.left + (float)col * (cw + 12.f);
            float ky = in.top + 24.f + (float)row * 30.f;
            painter_.Circle(kx + 4.f, ky + 9.f, 4.f, c);
            painter_.Text(k, RF(kx + 14.f, ky, kx + cw * 0.55f, ky + 18.f), 12.f,
                          TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center);
            painter_.Text(v, RF(kx + cw * 0.5f, ky, kx + cw, ky + 18.f), 12.f,
                          TWeight::Semibold, theme_.label, TAlign::Right, TVAlign::Center);
        };
        kv(0, 0, L"作画时长", FormatDurationUs(analysis_.activePaintUs), theme_.cDeep);
        kv(1, 0, L"有效占比", FormatPercent(eff, 1), theme_.accent);
        kv(2, 0, L"深度 / 浅度块",
           std::to_wstring(analysis_.deepBlocks) + L" / " + std::to_wstring(analysis_.shallowBlocks),
           theme_.cShallow);
        kv(0, 1, L"画笔笔画", std::to_wstring(analysis_.brushTotal), theme_.cBrush);
        kv(1, 1, L"撤回 / 重做",
           std::to_wstring(analysis_.undoTotal) + L" / " + std::to_wstring(analysis_.redoTotal),
           theme_.cUndo);
        kv(2, 1, L"普通操作", std::to_wstring(analysis_.normalTotal), theme_.cNormal);
        kv(0, 2, L"暂停绘画", FormatDurationUs(analysis_.pauseUs), theme_.cPause);
        kv(1, 2, L"打断次数", std::to_wstring(analysis_.interruptCount), theme_.cInterrupt);
        kv(2, 2, L"打断累计", FormatDurationUs(analysis_.interruptUs), theme_.cInterrupt);
        y += h + 20.f;
    }

    return y + scrollY_ - r.top;
}

// ============================================================
//  页：习惯
// ============================================================
float App::DrawHabit(const D2D1_RECT_F& r) {
    float x = r.left + 20.f;
    float y = r.top + 18.f - scrollY_;
    float w = RW(r) - 40.f - 10.f;

    // 子视图分段控件
    habitTabRects_.clear();
    {
        D2D1_RECT_F sr = RF(x, y, x + std::min(w, 460.f), y + 32.f);
        painter_.Segmented(sr, habitTabs_, (int)habit_.GetPanel(), &habitTabRects_);
        y += 46.f;
    }

    // 长程关联：滞后阶数选择
    if (habit_.GetPanel() == HabitView::Panel::LongRange) {
        lagRects_.clear();
        std::vector<std::wstring> items;
        for (int k = 2; k <= MAX_LAG; ++k) items.push_back(L"滞后 " + std::to_wstring(k) + L" 步");
        D2D1_RECT_F sr = RF(x, y, x + std::min(w, 420.f), y + 30.f);
        painter_.Segmented(sr, items, habit_.GetLag() - 2, &lagRects_);
        y += 42.f;
    }

    // 二阶条件：前缀状态选择
    if (habit_.GetPanel() == HabitView::Panel::SecondOrder) {
        prefixRects_.clear();
        std::vector<std::wstring> labels;
        for (int i = 0; i < (int)State::COUNT; ++i) labels.push_back(U2W(StateNameCn((State)i)));

        painter_.Text(L"前缀状态①", RF(x, y, x + 100.f, y + 24.f), 12.f, TWeight::Medium,
                      theme_.label2, TAlign::Left, TVAlign::Center);
        for (int i = 0; i < (int)State::COUNT; ++i) {
            float bw = 92.f;
            D2D1_RECT_F br = RF(x + 104.f + (float)i * (bw + 6.f), y, x + 104.f + (float)i * (bw + 6.f) + bw, y + 26.f);
            prefixRects_.push_back(br);
            bool sel = ((int)habit_.GetPrefixA() == i);
            painter_.FillRound(br, 7.f, sel ? theme_.accent : (theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xF2F2F7)));
            painter_.Text(labels[(size_t)i], br, 11.f, sel ? TWeight::Semibold : TWeight::Regular,
                          sel ? ColHex(0xFFFFFF) : theme_.label2, TAlign::Center, TVAlign::Center);
        }
        y += 34.f;
        painter_.Text(L"前缀状态②", RF(x, y, x + 100.f, y + 24.f), 12.f, TWeight::Medium,
                      theme_.label2, TAlign::Left, TVAlign::Center);
        for (int i = 0; i < (int)State::COUNT; ++i) {
            float bw = 92.f;
            D2D1_RECT_F br = RF(x + 104.f + (float)i * (bw + 6.f), y, x + 104.f + (float)i * (bw + 6.f) + bw, y + 26.f);
            prefixRects_.push_back(br);
            bool sel = ((int)habit_.GetPrefixB() == i);
            painter_.FillRound(br, 7.f, sel ? theme_.accent : (theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xF2F2F7)));
            painter_.Text(labels[(size_t)i], br, 11.f, sel ? TWeight::Semibold : TWeight::Regular,
                          sel ? ColHex(0xFFFFFF) : theme_.label2, TAlign::Center, TVAlign::Center);
        }
        y += 38.f;
    }

    // 图表卡片
    {
        float cardH = habit_.ContentHeight(w - 32.f) + 16.f;
        habitRect_ = RF(x, y, x + w, y + cardH);
        painter_.Card(habitRect_, 10.f);
        D2D1_RECT_F inner = Inset(habitRect_, 16.f, 8.f);
        habit_.Draw(painter_, inner, 0.f);
        y += cardH + 16.f;
    }

    // 序列说明
    {
        float h = 74.f;
        D2D1_RECT_F cr = RF(x, y, x + w, y + h);
        painter_.Card(cr, 10.f);
        D2D1_RECT_F in = Inset(cr, 16.f, 12.f);
        painter_.Text(L"序列口径说明", RF(in.left, in.top, in.right, in.top + 18.f), 12.f,
                      TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);
        painter_.TextWrap(
            L"序列由「普通操作 / 撤回·重做 / 画笔 / 普通间隔 / 暂停绘画」五种状态构成。"
            L"打断绘画不进入序列——跨会话之间不存在行为因果关系，把它算进去会稀释真实的转移规律。"
            L"长程关联用提升度（条件概率 ÷ 边缘概率）衡量，避免把高频状态的必然延续误判为规律。",
            RF(in.left, in.top + 22.f, in.right, in.bottom), 11.f, TWeight::Regular,
            theme_.label2, TAlign::Left, TVAlign::Top);
        y += h + 20.f;
    }

    return y + scrollY_ - r.top;
}

// ============================================================
//  页：录屏 —— 卡片列表（一个绘画文档一张卡）
//
//  按要求：不再有任何说明性卡片，整页就是录屏卡片列表。
//  每张卡 = videos\<文档名>\ 文件夹 = 该绘画文件的全部录制会话。
//  左侧缩略图，右侧「导出」（拼接为单个视频）与「清空」（移入回收站）。
// ============================================================
float App::DrawRecord(const D2D1_RECT_F& r) {
    float x = r.left + 20.f;
    float y = r.top + 18.f - scrollY_;
    float w = RW(r) - 40.f - 10.f;

    // 卡片需跟随录制产生的新文件更新，内部按 0.8s 节流
    ScanRecCards();

    RecorderStats rs = Recorder::I().Snapshot();

    // —— 录屏设置卡：是否录屏（滑块）+ 录制模式（滑块）+ 两个可更改的路径 ——
    {
        const float rowH = 38.f;
        D2D1_RECT_F cr = RF(x, y, x + w, y + 46.f + rowH * 4.f);
        painter_.Card(cr, 10.f);
        D2D1_RECT_F in = Inset(cr, 16.f, 10.f);
        painter_.Text(L"录屏设置", RF(in.left, in.top, in.right, in.top + 20.f), 13.f,
                      TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);
        float ry = in.top + 26.f;

        // 第 1 行：是否录屏（点击滑块交互）
        {
            D2D1_RECT_F rr = RF(in.left, ry, in.right, ry + rowH);
            painter_.Text(L"启用绘画录屏", RF(rr.left, rr.top, rr.left + 200.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            recordSwitchRect_ = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(recordSwitchRect_, Cfg().recordVideo);
            ry += rowH;
        }
        // 第 2 行：录制模式（整屏 ↔ 窗口）
        {
            D2D1_RECT_F rr = RF(in.left, ry, in.right, ry + rowH);
            painter_.Text(L"录制模式", RF(rr.left, rr.top, rr.left + 120.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            painter_.Text(Cfg().captureMode == 1 ? L"窗口" : L"整屏",
                          RF(rr.left + 120.f, rr.top, rr.left + 170.f, rr.bottom), 13.f,
                          TWeight::Medium, theme_.accent, TAlign::Left, TVAlign::Center);
            recordModeSwitchRect_ = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(recordModeSwitchRect_, Cfg().captureMode == 1);
            ry += rowH;
        }
        // 第 3 行：视频缓存路径
        {
            D2D1_RECT_F rr = RF(in.left, ry, in.right, ry + rowH);
            painter_.Text(L"视频缓存路径", RF(rr.left, rr.top, rr.left + 100.f, rr.bottom), 13.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            painter_.Text(U2W(VideoDir()), RF(rr.left + 100.f, rr.top, rr.right - 118.f, rr.bottom),
                          11.f, TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center, true);
            cachePathBtn_ = RF(rr.right - 110.f, rr.top + 5.f, rr.right, rr.bottom - 5.f);
            painter_.FillRound(cachePathBtn_, 7.f, theme_.accentSoft);
            painter_.Text(L"更改…", cachePathBtn_, 11.f, TWeight::Medium, theme_.accent,
                          TAlign::Center, TVAlign::Center);
            ry += rowH;
        }
        // 第 4 行：默认导出路径
        {
            D2D1_RECT_F rr = RF(in.left, ry, in.right, ry + rowH);
            painter_.Text(L"默认导出路径", RF(rr.left, rr.top, rr.left + 100.f, rr.bottom), 13.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            std::wstring ed = U2W(Cfg().exportDir);
            if (ed.empty()) ed = L"系统「视频」文件夹（默认）";
            painter_.Text(ed, RF(rr.left + 100.f, rr.top, rr.right - 118.f, rr.bottom),
                          11.f, TWeight::Regular,
                          Cfg().exportDir.empty() ? theme_.label3 : theme_.label2,
                          TAlign::Left, TVAlign::Center, true);
            exportPathBtn_ = RF(rr.right - 110.f, rr.top + 5.f, rr.right, rr.bottom - 5.f);
            painter_.FillRound(exportPathBtn_, 7.f, theme_.accentSoft);
            painter_.Text(L"更改…", exportPathBtn_, 11.f, TWeight::Medium, theme_.accent,
                          TAlign::Center, TVAlign::Center);
        }
        y += RH(cr) + 14.f;
    }

    // —— 顶部状态行（纯文字，无卡片底）——
    {
        bool cap = rs.capturing;
        painter_.Circle(x + 5.f, y + 11.f, 5.f,
                        cap ? theme_.cUndo : (rs.supported ? ColHex(0x34C759) : theme_.label3));
        std::wstring st;
        if (!Cfg().recordVideo) {
            st = L"录屏已关闭 · 打开上方开关后，在目标软件中作画时自动录制";
        } else if (cap) {
            st = L"正在录制 " + U2W(rs.sessionDoc) + L" · " +
                 FormatDurationUs((int64_t)(rs.clipSeconds * 1e6));
        } else if (rs.supported) {
            st = L"待机 · 在目标软件中作画时自动录制";
        } else {
            st = L"当前系统不支持屏幕捕获";
        }
        painter_.Text(st, RF(x + 16.f, y, x + w * 0.62f, y + 22.f), 13.f, TWeight::Semibold,
                      theme_.label, TAlign::Left, TVAlign::Center, true);
        y += 28.f;

        if (!rs.lastError.empty()) {
            painter_.Text(L"提示：" + U2W(rs.lastError), RF(x, y, x + w, y + 16.f), 10.f,
                          TWeight::Regular, theme_.cUndo, TAlign::Left, TVAlign::Center, true);
            y += 20.f;
        }
        if (!exportBusy_.load() && !exportErr_.empty()) {
            painter_.Text(L"导出失败：" + U2W(exportErr_), RF(x, y, x + w, y + 16.f), 10.f,
                          TWeight::Regular, theme_.cUndo, TAlign::Left, TVAlign::Center, true);
            y += 20.f;
        }
        // 窗口模式遮挡提示：WGC 窗口捕获在窗口被别的窗口压住、或 PS 液化等
        // 模态全屏窗口遮挡主窗口时，会录到黑块（用户已确认接受该代价）。
        if (Cfg().captureMode == 1) {
            painter_.Text(L"窗口模式：只录绘画窗口本身；被其他窗口遮挡或液化等全屏操作时会录到黑块。",
                          RF(x, y, x + w, y + 16.f), 10.f,
                          TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center, true);
            y += 20.f;
        }
    }

    // —— 卡片列表标题 ——
    {
        wchar_t b[128];
        swprintf_s(b, L"录屏卡片 · %d 个绘画文件", (int)recCards_.size());
        painter_.Text(b, RF(x, y, x + w, y + 26.f), 15.f, TWeight::Semibold,
                      theme_.label, TAlign::Left, TVAlign::Center);
        y += 30.f;
    }

    videoRects_.clear();

    if (recCards_.empty()) {
        painter_.TextWrap(
            L"还没有录屏。在 Photoshop / Clip Studio Paint（优动漫）/ SAI2 中开始作画，"
            L"程序会按绘画文件自动归档：同一绘画文件的多次录制归为一张卡片，"
            L"可用「导出」把它们拼接成一个视频。",
            RF(x, y, x + w, y + 60.f), 12.f, TWeight::Regular, theme_.label3,
            TAlign::Left, TVAlign::Top);
        y += 66.f;
        return y + scrollY_ - r.top;
    }

    const float cardH = 104.f;
    const float thumbW = 156.f, thumbH = 88.f;

    for (size_t i = 0; i < recCards_.size(); ++i) {
        RecCard& c = recCards_[i];
        D2D1_RECT_F cr = RF(x, y, x + w, y + cardH);
        c.rect = cr;
        videoRects_.push_back(cr);

        painter_.FillRound(cr, 10.f, theme_.card);
        painter_.StrokeRound(cr, 10.f, theme_.separator, 1.f);

        // —— 左：缩略图 ——
        D2D1_RECT_F tr = RF(cr.left + 12.f, cr.top + (cardH - thumbH) * 0.5f,
                            cr.left + 12.f + thumbW, cr.top + (cardH + thumbH) * 0.5f);
        bool drew = false;
        if (!c.thumbKey.empty()) drew = painter_.DrawBitmap(c.thumbKey, tr, 6.f);
        if (!drew) {
            // 占位图：深色底 + 播放三角（缩略图未生成或生成失败）
            painter_.FillRound(tr, 6.f, theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xE9E9EE));
            float cx = tr.left + RW(tr) * 0.5f, cy = tr.top + RH(tr) * 0.5f;
            painter_.Circle(cx, cy, 13.f, theme_.label3);
            painter_.Text(L"▶", RF(cx - 13.f, cy - 13.f, cx + 13.f, cy + 13.f), 11.f,
                          TWeight::Regular, theme_.card, TAlign::Center, TVAlign::Center);
            if (c.thumbLoading) {
                painter_.Text(L"生成缩略图…", RF(tr.left, tr.bottom - 16.f, tr.right, tr.bottom),
                              9.f, TWeight::Regular, theme_.label3, TAlign::Center, TVAlign::Center);
            }
        } else {
            painter_.StrokeRound(tr, 6.f, theme_.separator, 1.f);
        }
        // 按需排队生成缩略图（幂等，已缓存则立即返回）
        RequestThumb(c);

        // —— 中：文档名与统计 ——
        float tx = tr.right + 14.f;
        float rightLimit = cr.right - 196.f;   // 给右侧两个按钮留位
        if (rightLimit < tx + 60.f) rightLimit = tx + 60.f;

        painter_.Text(c.doc.doc, RF(tx, cr.top + 12.f, rightLimit, cr.top + 34.f), 14.f,
                      TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center, true);

        wchar_t info[256];
        swprintf_s(info, L"%d 段录制 · 共 %s · %s",
                   c.doc.FileCount(),
                   FormatDurationUs((int64_t)(c.doc.totalSeconds * 1e6)).c_str(),
                   FormatBytes(c.doc.totalBytes).c_str());
        painter_.Text(info, RF(tx, cr.top + 36.f, rightLimit, cr.top + 56.f), 11.f,
                      TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center, true);

        // 导出中的进度条
        bool exportingThis = exportBusy_.load() && exportDocName_ == c.doc.doc;
        if (exportingThis) {
            float pv = exportProgress_.load();
            if (pv < 0.f) pv = 0.f;
            if (pv > 1.f) pv = 1.f;
            D2D1_RECT_F bar = RF(tx, cr.top + 62.f, rightLimit - 60.f, cr.top + 68.f);
            painter_.FillRound(bar, 3.f, theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xE9E9EE));
            D2D1_RECT_F fill = RF(bar.left, bar.top, bar.left + std::max(3.f, RW(bar) * pv), bar.bottom);
            painter_.FillRound(fill, 3.f, theme_.accent);
            wchar_t pb[64];
            swprintf_s(pb, L"%.0f%%", pv * 100.f);
            painter_.Text(pb, RF(rightLimit - 56.f, cr.top + 58.f, rightLimit, cr.top + 72.f), 10.f,
                          TWeight::Medium, theme_.accent, TAlign::Right, TVAlign::Center);
        } else if (c.doc.isLegacy) {
            painter_.Text(L"未归档的旧文件（仍可导出）", RF(tx, cr.top + 60.f, rightLimit, cr.top + 78.f),
                          10.f, TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center, true);
        }

        // —— 右：导出 / 清空 两个按钮 ——
        float bw = 82.f, bh = 30.f, gap = 10.f;
        float bx = cr.right - 12.f;
        D2D1_RECT_F clearBtn = RF(bx - bw, cr.top + (cardH - bh) * 0.5f, bx, cr.top + (cardH + bh) * 0.5f);
        bx -= (bw + gap);
        D2D1_RECT_F exportBtn = RF(bx - bw, cr.top + (cardH - bh) * 0.5f, bx, cr.top + (cardH + bh) * 0.5f);
        c.exportBtn = exportBtn;
        c.clearBtn = clearBtn;

        // 导出：正在导出时置灰并显示"导出中"
        bool canExport = !exportBusy_.load();
        painter_.FillRound(exportBtn, 7.f, canExport ? theme_.accent : theme_.label3);
        painter_.Text(exportingThis ? L"导出中…" : L"导出", exportBtn, 12.f, TWeight::Medium,
                      ColHex(0xFFFFFF), TAlign::Center, TVAlign::Center);

        // 清空：红色描边，强调这是删除动作
        painter_.FillRound(clearBtn, 7.f, theme_.card);
        painter_.StrokeRound(clearBtn, 7.f, ColHex(0xFF3B30, 0.7f), 1.f);
        painter_.Text(L"清空", clearBtn, 12.f, TWeight::Medium, ColHex(0xFF3B30),
                      TAlign::Center, TVAlign::Center);

        y += cardH + 12.f;
    }

    y += 4.f;
    return y + scrollY_ - r.top;
}

// ============================================================
//  页：设置
// ============================================================
static void LabelInput(Painter& p, const D2D1_RECT_F& row, const std::wstring& label,
                       const std::wstring& hint, TextInput& in, float inputW = 120.f) {
    const Theme& th = p.Th();
    p.Text(label, RF(row.left, row.top, row.left + 200.f, row.bottom), 14.f,
           TWeight::Regular, th.label, TAlign::Left, TVAlign::Center);
    D2D1_RECT_F ir = RF(row.right - inputW - 150.f, row.top + 5.f, row.right - 150.f, row.bottom - 5.f);
    in.SetRect(ir);
    in.Draw(p, th.label, th.dark ? ColHex(0x2C2C2E) : ColHex(0xF7F7FA), th.separator);
    p.Text(hint, RF(row.right - 142.f, row.top, row.right, row.bottom), 10.f,
           TWeight::Regular, th.label3, TAlign::Left, TVAlign::Center, true);
}

float App::DrawSettings(const D2D1_RECT_F& r) {
    float x = r.left + 20.f;
    float y = r.top + 18.f - scrollY_;
    float w = RW(r) - 40.f - 10.f;

    toggleRects_.clear();
    toggleVals_.clear();
    toggleIds_.clear();

    // 卡片高度 = 上内边距10 + 标题26 + nRows*rowH + 下内边距10，按内容精确计算
    auto card = [&](const std::wstring& title, int nRows, float rowH, D2D1_RECT_F& out) {
        float h = 46.f + (float)nRows * rowH;
        out = RF(x, y, x + w, y + h);
        painter_.Card(out, 10.f);
        D2D1_RECT_F in = Inset(out, 16.f, 10.f);
        painter_.Text(title, RF(in.left, in.top, in.right, in.top + 20.f), 13.f,
                      TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);
        return RF(in.left, in.top + 26.f, in.right, out.bottom - 10.f);
    };
    auto row = [&](const D2D1_RECT_F& body, int i, float rh) {
        return RF(body.left, body.top + (float)i * rh, body.right, body.top + (float)(i + 1) * rh);
    };

    // —— 采集 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"无感记录", 3, 34.f, cr);
        // 总开关
        {
            D2D1_RECT_F rr = row(body, 0, 34.f);
            painter_.Text(L"启用后台记录", RF(rr.left, rr.top, rr.left + 200.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            D2D1_RECT_F sr = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(sr, Cfg().enabled);
            toggleRects_.push_back(sr);
            toggleVals_.push_back(Cfg().enabled);
            toggleIds_.push_back(TOG_ENABLED);
        }
        // 按键合并窗口
        LabelInput(painter_, row(body, 1, 34.f), L"相同按键合并窗口（毫秒）",
                   L"0.8 秒内连按同一键记为一次", inputs_[IN_KEYMERGE]);
        // 目标软件
        {
            D2D1_RECT_F rr = row(body, 2, 34.f);
            std::wstring list;
            for (size_t i = 0; i < Cfg().targets.size(); ++i) {
                if (i) list += L"、";
                list += U2W(Cfg().targets[i]);
            }
            painter_.Text(L"目标软件", RF(rr.left, rr.top, rr.left + 100.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            painter_.Text(list, RF(rr.left + 100.f, rr.top, rr.right - 190.f, rr.bottom), 11.f,
                          TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center, true);
            addTargetBtn_ = RF(rr.right - 180.f, rr.top + 5.f, rr.right, rr.bottom - 5.f);
            painter_.FillRound(addTargetBtn_, 7.f, theme_.accentSoft);
            painter_.Text(L"添加当前前台软件", addTargetBtn_, 11.f, TWeight::Medium,
                          theme_.accent, TAlign::Center, TVAlign::Center);
        }
        y += RH(cr) + 14.f;
    }

    // —— 添加目标软件输入 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"添加目标软件（进程名，如 krita.exe）", 1, 34.f, cr);
        D2D1_RECT_F rr = row(body, 0, 34.f);
        D2D1_RECT_F ir = RF(rr.left, rr.top + 3.f, rr.right - 100.f, rr.bottom - 3.f);
        inputs_[IN_ADDTARGET].SetRect(ir);
        inputs_[IN_ADDTARGET].Draw(painter_, theme_.label,
                                   theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xF7F7FA), theme_.separator);
        addConfirmBtn_ = RF(rr.right - 92.f, rr.top + 3.f, rr.right, rr.bottom - 3.f);
        painter_.FillRound(addConfirmBtn_, 7.f, theme_.accent);
        painter_.Text(L"添加", addConfirmBtn_, 12.f, TWeight::Medium, ColHex(0xFFFFFF),
                      TAlign::Center, TVAlign::Center);
        y += RH(cr) + 14.f;
    }

    // —— 录屏 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"录屏", 5, 34.f, cr);
        {
            D2D1_RECT_F rr = row(body, 0, 34.f);
            painter_.Text(L"启用绘画录屏", RF(rr.left, rr.top, rr.left + 200.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            D2D1_RECT_F sr = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(sr, Cfg().recordVideo);
            toggleRects_.push_back(sr);
            toggleVals_.push_back(Cfg().recordVideo);
            toggleIds_.push_back(TOG_VIDEO);
        }
        // 录制模式（整屏 ↔ 窗口）：用独立成员，不进 toggleRects_。
        // （原先的顾虑是「会挤掉 darkMode 的硬编码索引 i==2」，现已改为按
        // toggleIds_ 分发，该约束不复存在；仍保持独立是为了录屏页复用同一逻辑。）
        {
            D2D1_RECT_F rr = row(body, 1, 34.f);
            painter_.Text(L"录制模式", RF(rr.left, rr.top, rr.left + 120.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            painter_.Text(Cfg().captureMode == 1 ? L"窗口" : L"整屏",
                          RF(rr.left + 120.f, rr.top, rr.left + 170.f, rr.bottom), 13.f,
                          TWeight::Medium, theme_.accent, TAlign::Left, TVAlign::Center);
            settingsModeSwitchRect_ = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(settingsModeSwitchRect_, Cfg().captureMode == 1);
        }
        LabelInput(painter_, row(body, 2, 34.f), L"帧率（fps）", L"建议 30", inputs_[IN_FPS]);
        LabelInput(painter_, row(body, 3, 34.f), L"码率（kbps）", L"越大越清晰、文件越大", inputs_[IN_BITRATE]);
        LabelInput(painter_, row(body, 4, 34.f), L"最短文件（毫秒）", L"更短的自动丢弃", inputs_[IN_MINCLIP]);
        y += RH(cr) + 14.f;
    }

    // —— 分类阈值 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"分类阈值（填 0 表示按你的习惯自动统计）", 4, 34.f, cr);
        LabelInput(painter_, row(body, 0, 34.f), L"暂停绘画下限 n1（秒）",
                   L"空档达到此值才算一次暂停，当前 " + FormatGapSec(analysis_.th.n1Sec),
                   inputs_[IN_N1]);
        LabelInput(painter_, row(body, 1, 34.f), L"打断绘画下限 n2（秒）",
                   L"当前 " + FormatGapSec(analysis_.th.n2Sec), inputs_[IN_N2]);
        LabelInput(painter_, row(body, 2, 34.f), L"深度绘画间隔上限（秒）",
                   L"块内典型间隔小于此值即为深度", inputs_[IN_DEPTH]);
        {
            D2D1_RECT_F rr = row(body, 3, 34.f);
            painter_.TextWrap(U2W(analysis_.th.method), RF(rr.left, rr.top, rr.right, rr.bottom),
                              10.f, TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
        }
        y += RH(cr) + 14.f;
    }

    // —— 外观 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"外观", 1, 34.f, cr);
        D2D1_RECT_F rr = row(body, 0, 34.f);
        painter_.Text(L"深色模式", RF(rr.left, rr.top, rr.left + 200.f, rr.bottom), 14.f,
                      TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
        D2D1_RECT_F sr = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
        painter_.Switch(sr, dark_);
        toggleRects_.push_back(sr);
        toggleVals_.push_back(dark_);
        toggleIds_.push_back(TOG_DARK);
        y += RH(cr) + 14.f;
    }

    // —— 启动 ——
    {
        D2D1_RECT_F cr;
        D2D1_RECT_F body = card(L"启动", 2, 34.f, cr);
        // 第 0 行：开机自动启动开关
        {
            D2D1_RECT_F rr = row(body, 0, 34.f);
            painter_.Text(L"开机自动启动", RF(rr.left, rr.top, rr.left + 200.f, rr.bottom), 14.f,
                          TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
            D2D1_RECT_F sr = Painter::SwitchRect(rr.right, rr.top + RH(rr) * 0.5f, 0.86f);
            painter_.Switch(sr, Cfg().autoStart);
            toggleRects_.push_back(sr);
            toggleVals_.push_back(Cfg().autoStart);
            toggleIds_.push_back(TOG_AUTOSTART);
        }
        // 第 1 行：灰色说明
        {
            D2D1_RECT_F rr = row(body, 1, 34.f);
            painter_.TextWrap(L"登录后自动在托盘运行，不弹窗口、不打扰创作。点托盘图标可随时唤回界面。",
                              RF(rr.left, rr.top, rr.right, rr.bottom), 10.f,
                              TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
        }
        y += RH(cr) + 14.f;
    }

    // —— 数据 ——
    {
        D2D1_RECT_F cr;
        // 行数 2 → 3：第三行放「导出诊断信息」按钮
        D2D1_RECT_F body = card(L"数据与目录", 3, 32.f, cr);
        D2D1_RECT_F rr0 = row(body, 0, 32.f);
        painter_.Text(L"数据目录", RF(rr0.left, rr0.top, rr0.left + 90.f, rr0.bottom), 13.f,
                      TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
        painter_.Text(U2W(DataDir()), RF(rr0.left + 90.f, rr0.top, rr0.right - 126.f, rr0.bottom),
                      11.f, TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center, true);
        // 「打开录屏目录」按钮已移除：录屏缓存路径改到录屏页管理（可更改、可直接打开）。
        openDataBtn_ = RF(rr0.right - 118.f, rr0.top + 4.f, rr0.right, rr0.bottom - 4.f);
        painter_.FillRound(openDataBtn_, 7.f, theme_.accentSoft);
        painter_.Text(L"打开数据目录", openDataBtn_, 11.f, TWeight::Medium, theme_.accent,
                      TAlign::Center, TVAlign::Center);

        std::wstring info = L"事件流按天存为 JSONL（只追加，不改写历史）；录屏存为 MP4。"
                            L"全部数据仅保存在本机，不会上传。";
        painter_.TextWrap(info, row(body, 1, 32.f), 11.f,
                          TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);

        // —— 导出诊断信息 ——
        // 数位设备出问题时（笔没数据 / 坐标偏移 / 压感不对）用它导出一份报告回传。
        // 开发者无法为每个品牌做真机验证，这是唯一的排查通道。
        D2D1_RECT_F rr2 = row(body, 2, 32.f);
        painter_.Text(L"设备诊断", RF(rr2.left, rr2.top, rr2.left + 90.f, rr2.bottom), 13.f,
                      TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
        painter_.TextWrap(L"数位笔没反应或坐标不准时导出，仅含设备信息、不含隐私。",
                          RF(rr2.left + 90.f, rr2.top, rr2.right - 126.f, rr2.bottom), 11.f,
                          TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
        diagBtn_ = RF(rr2.right - 118.f, rr2.top + 4.f, rr2.right, rr2.bottom - 4.f);
        painter_.FillRound(diagBtn_, 7.f, theme_.accentSoft);
        painter_.Text(L"导出诊断信息", diagBtn_, 11.f, TWeight::Medium, theme_.accent,
                      TAlign::Center, TVAlign::Center);
        y += RH(cr) + 14.f;
    }

    // —— 退出程序 ——
    {
        exitBtn_ = RF(x, y, x + w, y + 46.f);
        painter_.FillRound(exitBtn_, 10.f, theme_.dark ? ColHex(0x2C2C2E) : ColHex(0xFFFFFF));
        painter_.StrokeRound(exitBtn_, 10.f, theme_.separator, 1.f);
        painter_.Text(L"退出程序", exitBtn_, 15.f, TWeight::Semibold, ColHex(0xFF3B30),
                      TAlign::Center, TVAlign::Center);
        y += 46.f + 24.f;
    }

    return y + scrollY_ - r.top;
}

// ============================================================
//  输入同步 / 提交
// ============================================================
void App::SyncInputsFromConfig() {
    const Config& c = Cfg();
    auto setInt = [&](int slot, int v) {
        inputs_[(size_t)slot].SetText(std::to_wstring(v));
    };
    auto setDbl = [&](int slot, double v) {
        wchar_t b[32];
        swprintf_s(b, L"%.3g", v);
        inputs_[(size_t)slot].SetText(b);
    };
    setInt(IN_KEYMERGE, c.keyMergeMs);
    setInt(IN_FPS, c.videoFps);
    setInt(IN_BITRATE, c.videoBitrateKbps);
    setInt(IN_MINCLIP, c.minClipMs);
    setDbl(IN_N1, c.n1Sec);
    setDbl(IN_N2, c.n2Sec);
    setDbl(IN_DEPTH, c.depthGapSec);
}

static bool ParseIntW(const std::wstring& s, int& out) {
    std::wstring t;
    for (wchar_t c : s) if (!iswspace(c)) t += c;
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    long v = wcstol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    out = (int)v;
    return true;
}
static bool ParseDoubleW(const std::wstring& s, double& out) {
    std::wstring t;
    for (wchar_t c : s) if (!iswspace(c)) t += c;
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    double v = wcstod(t.c_str(), &end);
    if (end == t.c_str()) return false;
    out = v;
    return true;
}

void App::CommitInputs() {
    if (inputs_.empty()) return;
    Config& c = Cfg();
    int iv; double dv;
    bool changed = false;
    if (ParseIntW(inputs_[IN_KEYMERGE].Text(), iv) && iv != c.keyMergeMs) { c.keyMergeMs = iv; changed = true; }
    if (ParseIntW(inputs_[IN_FPS].Text(), iv) && iv != c.videoFps)         { c.videoFps = iv; changed = true; }
    if (ParseIntW(inputs_[IN_BITRATE].Text(), iv) && iv != c.videoBitrateKbps) { c.videoBitrateKbps = iv; changed = true; }
    if (ParseIntW(inputs_[IN_MINCLIP].Text(), iv) && iv != c.minClipMs)    { c.minClipMs = iv; changed = true; }
    if (ParseDoubleW(inputs_[IN_N1].Text(), dv) && dv != c.n1Sec)          { c.n1Sec = dv < 0 ? 0 : dv; changed = true; }
    if (ParseDoubleW(inputs_[IN_N2].Text(), dv) && dv != c.n2Sec)          { c.n2Sec = dv < 0 ? 0 : dv; changed = true; }
    if (ParseDoubleW(inputs_[IN_DEPTH].Text(), dv) && dv != c.depthGapSec) { c.depthGapSec = dv < 0 ? 0 : dv; changed = true; }

    // 夹取，避免异常值让分析除零或图表退化
    c.keyMergeMs = std::max(0, std::min(10000, c.keyMergeMs));
    c.videoFps = std::max(1, std::min(120, c.videoFps));
    c.videoBitrateKbps = std::max(200, std::min(200000, c.videoBitrateKbps));
    c.minClipMs = std::max(0, std::min(600000, c.minClipMs));
    c.n1Sec = std::max(0.0, std::min(3600.0, c.n1Sec));
    c.n2Sec = std::max(0.0, std::min(86400.0, c.n2Sec));
    c.depthGapSec = std::max(0.0, std::min(3600.0, c.depthGapSec));

    if (changed) {
        settingsDirty_ = true;
        CfgSave();
        SyncInputsFromConfig();
        LogF("[cfg] 设置已更新");
        RequestAnalysis(useFullHistory_);
    }
}

// ============================================================
//  分析（后台线程）
// ============================================================
void App::RequestAnalysis(bool fullHistory) {
    useFullHistory_ = fullHistory;
    if (analysisBusy_.load()) { analysisPending_.store(true); return; }
    analysisBusy_.store(true);
    if (analysisThread_.joinable()) analysisThread_.join();
    analysisThread_ = std::thread([this, fullHistory]() { AnalysisThreadMain(fullHistory); });
}

void App::AnalysisThreadMain(bool fullHistory) {
    int64_t t0 = Clock::NowUs();
    Analysis result;
    Config cfg = Cfg();     // 取快照，避免线程间竞争

    if (fullHistory) {
        std::vector<Op> ops = LoadAllOps(0);
        if (!ops.empty()) {
            Thresholds th = DeriveThresholds(ops, cfg);
            result = Analyze(std::move(ops), th);
        }
    } else if (!days_.empty()) {
        // 选定单日
        std::lock_guard<std::mutex> lk(analysisMtx_);
        std::string day = (daySel_ >= 0 && daySel_ < (int)days_.size())
                          ? days_[(size_t)daySel_] : days_.back();
        if (!AnalyzeDay(day, result, cfg)) result = Analysis();
    }

    {
        std::lock_guard<std::mutex> lk(analysisMtx_);
        sharedAnalysis_ = std::move(result);
        sharedReady_ = true;
    }
    int64_t cost = Clock::NowUs() - t0;
    LogF("[analysis] 完成，用时 %lld ms", (long long)(cost / 1000));

    analysisBusy_.store(false);
    // 只通知 UI 线程；后续「挂起的再分析」由 UI 线程触发，
    // 绝不能在本线程调用 RequestAnalysis —— 那会 join 自己导致死锁
    if (hwnd_) PostMessageW(hwnd_, WM_ANALYSIS_DONE, 0, 0);
}

void App::OnAnalysisDone() {
    {
        std::lock_guard<std::mutex> lk(analysisMtx_);
        if (sharedReady_) {
            analysis_ = std::move(sharedAnalysis_);
            sharedReady_ = false;
        }
    }
    bool firstLoad = !dataReady_;   // 首次拿到有效数据 → 需要复位视图
    dataReady_ = !analysis_.empty;
    if (firstLoad) {
        // 首次装载：SetData 内部会 FitAll
        gantt_.SetData(analysis_);
    } else {
        // 自动刷新：保留用户当前的缩放/平移，不打断正在查看的局部
        gantt_.UpdateData(analysis_);
    }
    gantt_.SetTheme(theme_);
    habit_.SetData(analysis_);
    habit_.SetTheme(theme_);
    if (firstLoad && !gantt_.Empty()) gantt_.FitAll();

    // 处理分析期间挂起的再分析请求（在 UI 线程 join 安全）
    if (analysisPending_.exchange(false)) {
        RequestAnalysis(useFullHistory_);
    }
}

// ============================================================
//  数据
// ============================================================
void App::RefreshDayList() {
    days_ = ListDays();
    if (!days_.empty() && daySel_ >= (int)days_.size()) daySel_ = -1;
}

float App::ganttContentH() const {
    // 卡片高度取窗口高度的约六成，内容超出部分由图内部垂直滚动
    float h = painter_.H() * 0.62f;
    return std::max(340.f, std::min(900.f, h));
}

// ============================================================
//  录屏卡片列表：扫描 / 缩略图 / 导出 / 清空
// ============================================================

// 扫描 videos 目录重建卡片列表（节流 0.8s，录制期间也能跟随新文件更新）
void App::ScanRecCards() {
    int64_t now = Clock::NowUs();
    if (!recCards_.empty() && now - lastCardScanUs_ < CARD_SCAN_MIN_GAP_US) return;
    lastCardScanUs_ = now;

    // 保留每张卡片的缩略图缓存 key（位图已在 Painter 里，避免重复解码）
    std::map<std::wstring, std::pair<std::string, bool>> keep;
    for (const auto& c : recCards_)
        keep[c.doc.doc] = { c.thumbKey, c.thumbFailed };

    std::vector<DocCard> docs = ScanDocCards();
    recCards_.clear();
    recCards_.reserve(docs.size());
    for (auto& d : docs) {
        RecCard rc;
        rc.doc = std::move(d);
        rc.thumbPath = ThumbPathFor(rc.doc);
        auto it = keep.find(rc.doc.doc);
        if (it != keep.end()) {
            rc.thumbKey = it->second.first;
            rc.thumbFailed = it->second.second;
        }
        // 位图缓存绑定在渲染目标上：渲染目标重建（设备丢失）后缓存被清空，
        // 但卡片还持有旧 key，会挡住重新加载。检测失效则丢弃 key，
        // 下面会从磁盘 PNG 重新上传（自愈）。
        if (!rc.thumbKey.empty() && !painter_.HasBitmap(rc.thumbKey))
            rc.thumbKey.clear();
        // 已有缩略图文件但位图还没上传：这里直接读入缓存（解码很快，几百 KB 的 PNG）
        if (rc.thumbKey.empty() && !rc.thumbFailed && !rc.thumbPath.empty()) {
            int tw = 0, th = 0;
            std::vector<uint8_t> px;
            if (LoadPngToBgra(rc.thumbPath, tw, th, px) && !px.empty()) {
                std::string key = "thumb:" + W2U(rc.doc.doc);
                if (painter_.BitmapPut(key, tw, th, px.data())) rc.thumbKey = key;
            } else if (GetFileAttributesW(rc.thumbPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                rc.thumbFailed = true;   // 文件在但读不出来 → 不再重试
            }
        }
        recCards_.push_back(std::move(rc));
    }
}

// 排队生成缩略图（幂等：已有位图/已失败/已在排队 都直接返回）
void App::RequestThumb(RecCard& c) {
    if (!c.thumbKey.empty() || c.thumbFailed || c.thumbLoading) return;
    if (c.doc.files.empty()) return;

    std::wstring out = c.thumbPath;
    if (out.empty()) return;

    // 散文件的缩略图目录需要先建出来
    {
        size_t p = out.find_last_of(L'\\');
        if (p != std::wstring::npos) {
            std::wstring dir = out.substr(0, p);
            if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
                CreateDirectoryW(dir.c_str(), nullptr);
        }
    }

    c.thumbLoading = true;
    EnsureWorker();

    Job j;
    j.kind = JobKind::Thumb;
    j.card = c.doc;
    j.outPath = out;
    j.key = "thumb:" + W2U(c.doc.doc);
    j.doc = c.doc.doc;
    PushJob(std::move(j));
}

// 启动常驻工作线程（只启一次）
void App::EnsureWorker() {
    if (workerStarted_.exchange(true)) return;
    workerQuit_.store(false);
    worker_ = std::thread([this]() { WorkerLoop(); });
}

void App::PushJob(Job&& j) {
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        jobQueue_.push_back(std::move(j));
    }
    jobCv_.notify_one();
}

// 工作线程主体：串行消费队列，完成后投递 WM_JOB_DONE 给界面线程
void App::WorkerLoop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(jobMtx_);
            jobCv_.wait(lk, [this]() { return workerQuit_.load() || !jobQueue_.empty(); });
            if (workerQuit_.load()) return;
            j = std::move(jobQueue_.front());
            jobQueue_.erase(jobQueue_.begin());
        }

        bool ok = false;
        std::string err;

        if (j.kind == JobKind::Thumb) {
            // 逐个尝试：录制被打断可能留下残缺文件，取第一个能解码出首帧的
            for (const auto& f : j.card.files) {
                if (MakeThumbnailFromVideo(f, j.outPath, 320, err)) { ok = true; break; }
            }
            if (!ok && err.empty()) err = "没有可解码的录屏文件";
        } else {
            // 导出：进度回调把百分比写进原子量，界面每帧读取刷新进度条
            exportProgress_.store(0.f);
            ok = ConcatExport(j.card, j.outPath, err, [this](float p) {
                exportProgress_.store(p);
                return exportCancel_.load() == 0;   // false = 请求取消
            });
        }

        {
            std::lock_guard<std::mutex> lk(jobMtx_);
            JobDone d;
            d.kind = j.kind;
            d.ok = ok;
            d.key = j.key;
            d.out = j.outPath;
            d.doc = j.doc;
            d.err = err;
            jobDone_.push_back(d);
        }
        if (hwnd_) PostMessageW(hwnd_, WM_JOB_DONE, 0, 0);
    }
}

// 界面线程：排空完成队列，更新卡片状态并弹出结果提示
void App::OnJobDone() {
    std::vector<JobDone> done;
    {
        std::lock_guard<std::mutex> lk(jobMtx_);
        done.swap(jobDone_);
    }

    for (const JobDone& d : done) {
        if (d.kind == JobKind::Thumb) {
            for (auto& c : recCards_) {
                if (c.doc.doc != d.doc) continue;
                c.thumbLoading = false;
                if (!d.ok) { c.thumbFailed = true; break; }
                // 读入新生成的 PNG 并上传为位图
                int tw = 0, th = 0;
                std::vector<uint8_t> px;
                if (LoadPngToBgra(c.thumbPath, tw, th, px) && !px.empty() &&
                    painter_.BitmapPut(d.key, tw, th, px.data())) {
                    c.thumbKey = d.key;
                } else {
                    c.thumbFailed = true;
                }
                break;
            }
            continue;
        }

        // —— 导出完成 ——
        exportBusy_.store(false);
        if (d.ok) {
            exportErr_.clear();
            std::wstring msg = L"已导出到：\n" + d.out;
            int r = MessageBoxW(hwnd_, msg.c_str(), L"导出完成",
                                MB_YESNO | MB_ICONINFORMATION);
            if (r == IDYES && !d.out.empty()) {
                // 打开所在文件夹并选中导出的文件
                std::wstring cmd = L"/select,\"" + d.out + L"\"";
                ShellExecuteW(hwnd_, L"open", L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
            }
            // 导出产物若落在 videos 目录内会被扫描成新卡片，立即重扫刷新列表
            lastCardScanUs_ = 0;
            ScanRecCards();
        } else {
            // 取消是用户主动行为，不算失败，不留下误导性的错误提示
            if (d.err == "已取消") exportErr_.clear();
            else exportErr_ = d.err;
        }
    }
}

// 导出：把该绘画文件的全部录制拼接为一个视频
void App::StartExport(const DocCard& card) {
    if (exportBusy_.load()) {
        MessageBoxW(hwnd_, L"已有导出任务正在进行，请等它完成。", L"绘画伴侣",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (card.files.empty()) return;

    // 单段录制也允许导出（等于转存一份），但提示一下
    if (card.files.size() == 1) {
        int r = MessageBoxW(hwnd_,
            L"这个绘画文件目前只有 1 段录制，导出结果与源文件内容相同。\n仍要导出吗？",
            L"导出", MB_YESNO | MB_ICONQUESTION);
        if (r != IDYES) return;
    }

    // 默认文件名：文档名_拼接_时间戳.mp4
    std::wstring defName = SanitizeFileName(card.doc, 60) + L"_" +
                           U2W(UsToCompact(Clock::NowUs())) + L".mp4";

    // 初始目录优先用用户配置的默认导出目录；未配置时回退系统「视频」库。
    // 两者都不用录屏缓存目录：导出是成品交付物，若默认存回 videos\
    // 会被下次扫描当成「未归档散文件」又变成一张卡片。
    wchar_t initDir[MAX_PATH * 2] = {0};
    bool haveInit = false;
    std::wstring cfgDir = ConfiguredExportDir();
    if (!cfgDir.empty() && DirExistsW(cfgDir)) {
        wcsncpy_s(initDir, cfgDir.c_str(), _TRUNCATE);
        haveInit = true;
    }
    if (!haveInit) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Videos, 0, nullptr, &pidl)) && pidl) {
            if (SHGetPathFromIDListW(pidl, initDir)) haveInit = true;
            CoTaskMemFree(pidl);
        }
    }
    if (!haveInit) wcsncpy_s(initDir, VideoDirW().c_str(), _TRUNCATE);

    wchar_t fileBuf[MAX_PATH * 2] = {0};
    wcsncpy_s(fileBuf, defName.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"MP4 视频 (*.mp4)\0*.mp4\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = (DWORD)(sizeof(fileBuf) / sizeof(wchar_t));
    ofn.lpstrInitialDir = initDir;
    ofn.lpstrTitle = L"导出拼接后的录屏";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;   // 用户取消

    std::wstring outPath = fileBuf;

    exportBusy_.store(true);
    exportProgress_.store(0.f);
    exportCancel_.store(0);
    exportErr_.clear();
    exportDocName_ = card.doc;
    EnsureWorker();

    Job j;
    j.kind = JobKind::Export;
    j.card = card;
    j.outPath = outPath;
    j.doc = card.doc;
    PushJob(std::move(j));
    Invalidate();
}

// 清空：把该卡片的全部录制移入回收站（可恢复），不永久删除
void App::StartClear(const DocCard& card) {
    if (card.files.empty() && card.folder.empty()) return;

    std::wstring msg;
    if (card.isLegacy) {
        wchar_t b[512];
        swprintf_s(b, L"将把「%s」的 %d 个录屏文件移入回收站（可还原）。\n\n这些是旧版未归档的散文件，"
                      L"清空后不会删除其它文件。确定吗？",
                   card.doc.c_str(), card.FileCount());
        msg = b;
    } else {
        wchar_t b[512];
        swprintf_s(b, L"将把「%s」的整个录制文件夹移入回收站（可还原）：\n%s\n\n"
                      L"共 %d 段录制、%s。确定吗？",
                   card.doc.c_str(), card.folder.c_str(),
                   card.FileCount(), FormatBytes(card.totalBytes).c_str());
        msg = b;
    }

    // 二次确认 + 明确的按钮文字，避免误点造成数据丢失
    int r = MessageBoxW(hwnd_, msg.c_str(), L"清空录屏", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (r != IDYES) return;

    std::string err;
    bool ok;
    if (card.isLegacy) {
        // 散文件：逐个移入回收站，不删 videos 根目录
        ok = true;
        for (const auto& f : card.files) {
            std::string e;
            if (!RecyclePath(f, e)) { ok = false; err = e; }
        }
    } else {
        ok = RecyclePath(card.folder, err);
    }

    // 缩略图位图缓存一并丢弃，避免残留已删除文档的图
    painter_.BitmapDrop("thumb:" + W2U(card.doc));

    if (!ok) {
        LogF("[cards] 清空失败 doc=%s err=%s", W2U(card.doc).c_str(), err.c_str());
        MessageBoxW(hwnd_, (L"清空失败：" + U2W(err)).c_str(), L"绘画伴侣",
                    MB_OK | MB_ICONERROR);
    }
    lastCardScanUs_ = 0;   // 立即重扫，让卡片消失
    ScanRecCards();
    Invalidate();
}

// ============================================================
//  交互
// ============================================================
void App::SetTab(int t) {
    if (t < 0 || t > 3) return;
    if (tab_ == t) return;
    tab_ = t;
    scrollY_ = 0.f;
    ganttScroll_ = 0.f;
    CommitInputs();
    Cfg().lastTab = t;
    if (t == 2) { /* 录屏页需要实时状态 */ }
    Invalidate();
}

void App::ToggleDarkMode() {
    dark_ = !dark_;
    theme_ = dark_ ? Theme::Dark() : Theme::Light();
    painter_.SetTheme(theme_);
    gantt_.SetTheme(theme_);
    habit_.SetTheme(theme_);
    Cfg().darkMode = dark_;
    CfgSave();
    Invalidate();
}

void App::OnWheel(float x, float y, float delta) {
    D2D1_RECT_F content = ContentRect();
    if (!PtIn(x, y, content)) return;

    // 总览页：甘特区滚轮 = 以鼠标为中心缩放（放大到一定程度自动
    // 从概括色块淡入为逐笔操作明细）；拖拽 = 横向平移
    if (tab_ == 0 && PtIn(x, y, ganttRect_)) {
        D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
        gantt_.OnWheel(x, y, delta, inner);
        Invalidate();
        return;
    }

    float page = std::max(60.f, RH(content) * 0.18f);
    scrollY_ += (delta > 0 ? -page : page);
    float maxS = std::max(0.f, contentH_ - RH(content));
    if (scrollY_ < 0) scrollY_ = 0;
    if (scrollY_ > maxS) scrollY_ = maxS;
    Invalidate();
}

LRESULT App::OnMouse(UINT msg, WPARAM wp, LPARAM lp) {
    // 消息携带的是物理像素客户区坐标，而所有命中矩形都在 DIP 空间，
    // 必须先换算，否则缩放比 >100% 时点击会整体偏移（详见 ToDipX 注释）。
    float x = ToDipX(GET_X_LPARAM(lp));
    float y = ToDipY(GET_Y_LPARAM(lp));

    switch (msg) {
    case WM_MOUSEMOVE: {
        if (draggingScroll_) {
            float dy = y - dragStartY_;
            float maxS = std::max(0.f, contentH_ - RH(ContentRect()));
            float track = scrollTrackH_ - scrollThumbH_;
            if (track > 1.f) {
                scrollY_ = dragStartScroll_ - (dy / track) * maxS;
                if (scrollY_ < 0) scrollY_ = 0;
                if (scrollY_ > maxS) scrollY_ = maxS;
            }
            Invalidate();
            return 0;
        }
        if (ganttDragging_) {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.OnMove(x, y, inner);
            if (gantt_.IsDragging()) { Invalidate(); return 0; }
        }
        // 悬停：图表 tooltip / 光标形状
        bool needRepaint = false;
        if (tab_ == 0 && PtIn(x, y, ganttRect_)) {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.OnMove(x, y, inner);
            needRepaint = true;
        } else if (tab_ == 1 && PtIn(x, y, habitRect_)) {
            D2D1_RECT_F inner = Inset(habitRect_, 16.f, 8.f);
            habit_.OnMove(x, y, inner);
            needRepaint = true;
        } else {
            if (gantt_.Tip().show) { gantt_.OnLeave(); needRepaint = true; }
            if (habit_.Tip().show) { habit_.OnLeave(); needRepaint = true; }
        }
        // 光标
        bool overClickable = OverClickable(x, y);
        HCURSOR cur = LoadCursorW(nullptr, overClickable ? IDC_HAND : IDC_ARROW);
        SetCursor(cur);
        if (needRepaint) Invalidate();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        // 滚动条
        if (scrollThumbRect_.right > 0 && PtIn(x, y, scrollTrackRect_)) {
            draggingScroll_ = true;
            dragStartY_ = y;
            dragStartScroll_ = scrollY_;
            SetCapture(hwnd_);
            return 0;
        }
        // 侧边栏
        if (PtIn(x, y, SideRect())) { OnNavClick(x, y); return 0; }
        // 标题栏按钮
        if (PtIn(x, y, TitleRect())) { OnTitleBarClick(x, y); return 0; }
        // 内容区
        OnContentClick(x, y);
        if (tab_ == 0 && PtIn(x, y, ganttRect_)) {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.OnDown(x, y, inner);
            ganttDragging_ = true;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (draggingScroll_) {
            draggingScroll_ = false;
            ReleaseCapture();
            return 0;
        }
        if (ganttDragging_) {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.OnUp(x, y, inner);
            ganttDragging_ = false;
            Invalidate();
            return 0;
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (PtIn(x, y, SideRect())) { ShowTrayMenu(); return 0; }
        break;
    default: break;
    }
    (void)wp;
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

bool App::OverClickable(float x, float y) {
    for (const auto& r : navRects_) if (PtIn(x, y, r)) return true;
    if (PtIn(x, y, authorLinkRect_)) return true;   // 侧栏底部作者主页链接，各页均可点
    for (const auto& r : titleBtnRects_) if (PtIn(x, y, r)) return true;
    if (tab_ == 1) {
        for (const auto& r : habitTabRects_) if (PtIn(x, y, r)) return true;
        for (const auto& r : lagRects_) if (PtIn(x, y, r)) return true;
        for (const auto& r : prefixRects_) if (PtIn(x, y, r)) return true;
    }
    if (tab_ == 2) {
        for (const auto& r : videoRects_) if (PtIn(x, y, r)) return true;   // 整卡可点=导出
        if (PtIn(x, y, recordSwitchRect_)) return true;
        if (PtIn(x, y, recordModeSwitchRect_)) return true;
        if (PtIn(x, y, cachePathBtn_)) return true;
        if (PtIn(x, y, exportPathBtn_)) return true;
    }
    if (tab_ == 3) {
        for (const auto& r : toggleRects_) if (PtIn(x, y, r)) return true;
        if (PtIn(x, y, settingsModeSwitchRect_)) return true;
        if (PtIn(x, y, addTargetBtn_)) return true;
        if (PtIn(x, y, addConfirmBtn_)) return true;
        if (PtIn(x, y, openDataBtn_)) return true;
        if (PtIn(x, y, diagBtn_)) return true;
        if (PtIn(x, y, exitBtn_)) return true;
    }
    return false;
}

void App::OnNavClick(float x, float y) {
    // 底部作者主页链接优先判定（它在侧栏内，导航项循环不会命中它）
    if (PtIn(x, y, authorLinkRect_)) { OpenAuthorPage(); return; }
    for (size_t i = 0; i < navRects_.size(); ++i)
        if (PtIn(x, y, navRects_[i])) { SetTab((int)i); return; }
}

void App::OnTitleBarClick(float x, float y) {
    for (size_t i = 0; i < titleBtnRects_.size(); ++i) {
        if (!PtIn(x, y, titleBtnRects_[i])) continue;
        int id = (i < titleBtnIds_.size()) ? titleBtnIds_[i] : 0;
        switch (id) {
        case 1: gantt_.FitAll(); ganttScroll_ = 0.f; Invalidate(); break;
        case 2: {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.ZoomAt(inner.left + RW(inner) * 0.5f, 1.25f, inner);
            Invalidate();
            break;
        }
        case 3: {
            D2D1_RECT_F inner = Inset(ganttRect_, 10.f, 8.f);
            gantt_.ZoomAt(inner.left + RW(inner) * 0.5f, 0.8f, inner);
            Invalidate();
            break;
        }
        case 4: RefreshDayList(); CommitInputs(); RequestAnalysis(useFullHistory_); Invalidate(); break;
        case 6: CommitInputs(); Invalidate(); break;
        case 7: {
            Cfg().LoadDefaults();
            CfgSave();
            SyncInputsFromConfig();
            dark_ = Cfg().darkMode;
            theme_ = dark_ ? Theme::Dark() : Theme::Light();
            painter_.SetTheme(theme_);
            RequestAnalysis(useFullHistory_);
            Invalidate();
            break;
        }
        case 8: ToggleDarkMode(); break;
        default: break;
        }
        return;
    }
}

void App::OnContentClick(float x, float y) {
    // 先处理文本输入焦点
    bool hitInput = false;
    for (int i = 0; i < IN_COUNT; ++i) {
        if (inputs_[(size_t)i].OnClick(x, y)) { focusInput_ = i; hitInput = true; break; }
    }
    if (!hitInput && focusInput_ >= 0) {
        CommitInputs();
        focusInput_ = -1;
    }
    if (hitInput) { Invalidate(); return; }

    if (tab_ == 1) {
        for (size_t i = 0; i < habitTabRects_.size(); ++i)
            if (PtIn(x, y, habitTabRects_[i])) {
                habit_.SetPanel((HabitView::Panel)i);
                Invalidate();
                return;
            }
        for (size_t i = 0; i < lagRects_.size(); ++i)
            if (PtIn(x, y, lagRects_[i])) { habit_.SetLag((int)i + 2); Invalidate(); return; }
        size_t n = (size_t)State::COUNT;
        if (prefixRects_.size() >= n * 2) {
            for (size_t i = 0; i < n; ++i)
                if (PtIn(x, y, prefixRects_[i])) { habit_.SetPrefix((State)i, habit_.GetPrefixB()); Invalidate(); return; }
            for (size_t i = 0; i < n; ++i)
                if (PtIn(x, y, prefixRects_[n + i])) { habit_.SetPrefix(habit_.GetPrefixA(), (State)i); Invalidate(); return; }
        }
        return;
    }

    if (tab_ == 2) {
        // —— 录屏设置控件 ——
        if (PtIn(x, y, recordSwitchRect_)) { ToggleRecordVideo(); return; }
        if (PtIn(x, y, recordModeSwitchRect_)) { ToggleCaptureMode(); return; }
        if (PtIn(x, y, cachePathBtn_))     { ChooseVideoCacheDir(); return; }
        if (PtIn(x, y, exportPathBtn_))    { ChooseExportDir(); return; }

        // —— 卡片 ——
        // 先判「清空」：它是删除动作且区域小，必须优先于整卡命中，
        // 否则点清空会被下面的「整卡=导出」吃掉。
        for (size_t i = 0; i < recCards_.size(); ++i) {
            RecCard& c = recCards_[i];
            if (PtIn(x, y, c.clearBtn)) { StartClear(c.doc); return; }
        }
        // 卡片其余任意处（含「导出」按钮）= 导出该文档的全部录制。
        // 导出进行中时：点正在导出的那张卡 = 取消；点其它卡 = 提示等待。
        for (size_t i = 0; i < recCards_.size(); ++i) {
            RecCard& c = recCards_[i];
            if (!PtIn(x, y, c.rect)) continue;
            if (exportBusy_.load()) {
                if (exportDocName_ == c.doc.doc) {
                    exportCancel_.store(1);
                    exportErr_ = "正在取消…";
                } else {
                    MessageBoxW(hwnd_, L"已有导出任务正在进行，可点该卡片的「导出中…」按钮取消。",
                                L"绘画伴侣", MB_OK | MB_ICONINFORMATION);
                }
            } else {
                StartExport(c.doc);
            }
            return;
        }
        return;
    }

    if (tab_ == 3) {
        for (size_t i = 0; i < toggleRects_.size() && i < toggleVals_.size(); ++i) {
            if (!PtIn(x, y, toggleRects_[i])) continue;
            bool nv = !toggleVals_[i];
            // 按 id 分发（不按数组下标）：新增开关只需带 id 入列，无需重算其它分支。
            int id = (i < toggleIds_.size()) ? toggleIds_[i] : -1;
            switch (id) {
            case TOG_ENABLED:   Cfg().enabled = nv; break;
            case TOG_VIDEO:     Cfg().recordVideo = nv; break;
            case TOG_AUTOSTART: ToggleAutoStart(); return;  // 内部已存配置 + Invalidate
            case TOG_DARK:      ToggleDarkMode(); return;   // 内部已存配置 + Invalidate
            default: break;
            }
            CfgSave();
            Invalidate();
            return;
        }
        if (PtIn(x, y, settingsModeSwitchRect_)) { ToggleCaptureMode(); return; }
        if (PtIn(x, y, addTargetBtn_)) { AddTargetFromForeground(); return; }
        if (PtIn(x, y, addConfirmBtn_)) {
            std::wstring t = inputs_[IN_ADDTARGET].Text();
            std::string a = ToLowerA(W2U(t));
            if (!a.empty()) {
                if (a.find('.') == std::string::npos) a += ".exe";
                bool dup = false;
                for (const auto& s : Cfg().targets) if (s == a) { dup = true; break; }
                if (!dup) { Cfg().targets.push_back(a); CfgSave(); }
                inputs_[IN_ADDTARGET].SetText(L"");
                Invalidate();
            }
            return;
        }
        if (PtIn(x, y, openDataBtn_)) { OpenDataFolder(); return; }
        if (PtIn(x, y, diagBtn_)) { ExportDiagnostics(); return; }
        if (PtIn(x, y, exitBtn_)) {
            CommitInputs();
            if (hwnd_) DestroyWindow(hwnd_);
            return;
        }
    }
}

LRESULT App::OnKeyMsg(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (focusInput_ >= 0 && focusInput_ < IN_COUNT) {
            if (inputs_[(size_t)focusInput_].OnKey(wp, lp)) { Invalidate(); return 0; }
            if (wp == VK_RETURN || wp == VK_ESCAPE) { CommitInputs(); Invalidate(); return 0; }
        }
        if (wp == VK_ESCAPE) { ShowMainWindow(false); return 0; }
        if (wp >= VK_F1 && wp <= VK_F4 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            SetTab((int)(wp - VK_F1));
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::AddTargetFromForeground() {
    ForegroundState fg = Capture::I().FgSnapshot();
    // 不能把自己加进去
    if (fg.exe.empty() || fg.exe.find("paintingbuddy") != std::string::npos) {
        std::wstring t = inputs_[IN_ADDTARGET].Text();
        inputs_[IN_ADDTARGET].SetText(t.empty() ? L"" : t);
        return;
    }
    inputs_[IN_ADDTARGET].SetText(U2W(fg.exe));
    Invalidate();
}

void App::OpenDataFolder() {
    ShellExecuteW(hwnd_, L"open", U2W(DataDir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::OpenAuthorPage() {
    // 交给系统默认浏览器打开，不指定具体浏览器
    ShellExecuteW(hwnd_, L"open", L"https://space.bilibili.com/1254809606",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

// ------------------------------------------------------------
// 导出诊断信息
//
// 用途：开发者只有 Wacom，无法为 Huion / XP-Pen / Gaomon / 汉王等品牌
// 做真机验证。用户遇到「笔没数据 / 坐标偏移 / 压感不对」时，靠这份文件
// 回传即可定位 —— 这是「无法测试其他品牌」这一约束的必要补偿。
//
// 隐私：README 承诺「全部数据仅保存在本机，不会上传」。pb.log 里的
// [fg] 行含前台窗口标题与进程名、[rec] 行含绘画文档名，都属于用户隐私。
// 因此这里**只筛 [pen] / [cap] 两类设备诊断行**，不含任何用户内容；
// 也不导出 JSONL 事件流。导出文件默认落在数据目录，由用户自行决定是否发送。
// ------------------------------------------------------------
void App::ExportDiagnostics() {
    std::string out;
    char buf[512];

    time_t t = time(nullptr);
    struct tm lt; localtime_s(&lt, &t);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);

    out += "==== 绘画伴侣 设备诊断信息 ====\n";
    out += std::string("生成时间: ") + timeBuf + "\n";

    // 系统版本：GetVersionEx 在 Win8.1+ 需要 manifest 才准，改用 RtlGetVersion
    OSVERSIONINFOW ov{}; ov.dwOSVersionInfoSize = sizeof(ov);
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
    RtlGetVersionFn pRtl = (RtlGetVersionFn)(void*)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (pRtl) pRtl(&ov);
    snprintf(buf, sizeof(buf), "系统: Windows %lu.%lu build %lu\n",
             ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    out += buf;
    snprintf(buf, sizeof(buf), "DPI: %d\n", WindowDpi(hwnd_));
    out += buf;

    // 显示器与虚拟桌面：坐标映射歧义的关键上下文
    out += "显示器:\n";
    std::vector<std::string> mons = EnumDisplayNames();
    for (size_t i = 0; i < mons.size(); ++i) {
        snprintf(buf, sizeof(buf), "  [%zu] %s\n", i, mons[i].c_str());
        out += buf;
    }
    snprintf(buf, sizeof(buf), "虚拟桌面: x=%d y=%d w=%d h=%d\n",
             GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    out += buf;

    // —— 数据目录写入健康状态 ——
    // 为什么必须导出：pb.log 与当天 .jsonl 的写入失败原本完全静默，而唯一记录
    // 该失败的日志又写不进同一个坏目录，磁盘上留不下任何证据。用户只看到
    // 「录屏正常、总览图与习惯全空」。把这些状态放进内存并随诊断导出，
    // 才让「写入端坏了」和「采集端没数据」可区分。
    //
    // 隐私：数据目录路径含 Windows 用户名（C:\Users\<用户名>\AppData\Local），
    // 与本文件开头「不含用户内容」的约定冲突，故**不导出明文路径**，改为导出
    // 「路径中是否含非 ASCII 字符」这一布尔判据 —— 它正是该缺陷的触发条件
    // （窄字符 API 按 ACP 解释 UTF-8 路径），足以定位问题且不泄露用户名。
    {
        const DataDirHealth& h = DataDirHealthState();
        const std::string dir = DataDir();
        bool nonAscii = false;
        for (unsigned char c : dir) if (c >= 0x80) { nonAscii = true; break; }

        out += "\n数据目录写入状态:\n";
        snprintf(buf, sizeof(buf),
                 "  根目录创建=%s  logs子目录创建=%s  logs可写探测=%s  GetLastError=%lu\n",
                 h.baseOk ? "成功" : "失败", h.logsOk ? "成功" : "失败",
                 h.writable ? "通过" : "失败", (unsigned long)h.lastError);
        out += buf;
        snprintf(buf, sizeof(buf), "  目录路径含非ASCII字符=%s\n", nonAscii ? "是" : "否");
        out += buf;
        snprintf(buf, sizeof(buf), "  因写入失败丢弃的事件条数=%llu\n",
                 (unsigned long long)Events().DroppedEvents());
        out += buf;
        if (!h.note.empty()) { out += "  !! " + h.note + "\n"; }
        if (nonAscii && (!h.writable || Events().DroppedEvents() > 0)) {
            out += "  !! 高度可疑：路径含非 ASCII 字符且写入失败。历史上这类环境的\n"
                   "     日志/事件写入曾按 ACP 解释 UTF-8 路径而全部失败（录屏不受影响，\n"
                   "     因其走宽字符 API）。请把本文件连同「pb.log 是否存在」一并回传。\n";
        }
    }

    // 采集层计数快照
    CaptureCounters cc = Capture::I().Snapshot();
    out += "\n采集状态:\n";
    snprintf(buf, sizeof(buf),
             "  running=%d targetActive=%d devices=%llu\n"
             "  penRawReports=%llu penSamples=%llu penStrokes=%llu penPoints=%llu\n"
             "  mouseAsPenStrokes=%llu（最后一层回退：鼠标笔画记为笔，无压感）\n"
             "  keys=%llu mouseStrokes=%llu mouseClicks=%llu undo=%llu redo=%llu\n",
             (int)cc.running, (int)cc.targetActive, (unsigned long long)cc.devices,
             (unsigned long long)cc.penRawReports, (unsigned long long)cc.penSamples,
             (unsigned long long)cc.penStrokes, (unsigned long long)cc.penPoints,
             (unsigned long long)cc.mouseAsPenStrokes,
             (unsigned long long)cc.keys, (unsigned long long)cc.mouseStrokes,
             (unsigned long long)cc.mouseClicks, (unsigned long long)cc.undoOps,
             (unsigned long long)cc.redoOps);
    out += buf;
    // 先读 pb.log 并扫描：区分「当前会话」计数与「历史」笔活动。
    // 报告1 的教训——cc.penRawReports 是当前会话累计，历史日志里却有上一会话的
    // OK 行，直接说「从未收到」会与下方日志自相矛盾、误导排查。
    std::string logText;
    bool haveLog = ReadWholeFile(DataDir() + "\\pb.log", logText);
    PenInput::PenLogScan scan = haveLog ? PenInput::ScanPenLog(logText)
                                        : PenInput::PenLogScan{};
    out += "  （以上计数为「当前会话」累计；下方 pb.log 摘录含历史会话，已分段标注）\n";
    if (cc.mouseAsPenStrokes > 0) {
        snprintf(buf, sizeof(buf),
                 "  !! 有 %llu 条笔画来自「最后一层回退」：已识别到数位笔设备，但本会话从未\n"
                 "     收到落笔数据，故把鼠标左键笔画记为画笔笔画。这类笔画**没有压感与橡皮**\n"
                 "     信息。常见原因：绘画软件（如 Clip Studio Paint）开启了 Wintab，数位驱动把\n"
                 "     笔尖数据交给软件独占消费，不再向本程序投递。可在绘图软件里改回 Windows Ink\n"
                 "     以获得压感；或在 config.json 把 mouseAsPenFallback 置为 false 关闭此回退。\n",
                 (unsigned long long)cc.mouseAsPenStrokes);
        out += buf;
    }
    if (cc.penRawReports > 0 && cc.penSamples == 0) {
        out += "  !! 当前会话收到报文但一条都没解析成功 —— 看下面的 [pen] 日志找原因\n";
    } else if (cc.penRawReports == 0) {
        if (scan.everGotReport) {
            out += "  !! 当前会话尚无笔报文，但历史记录显示曾成功解析过笔\n"
                   "     （本会话可能还没落笔、笔走了 Windows Ink 通道、或刚重启）\n";
        } else {
            out += "  !! 当前及历史均无成功解析的笔报文 —— 笔可能走了 Windows Ink 通道，\n"
                   "     或 usage 未被注册（看下面的 registered digitizer usages 行）\n";
        }
    } else if (cc.penSamples > 0 && cc.penStrokes == 0 && cc.mouseAsPenStrokes == 0) {
        // 诊断盲点：采样非零、笔画为零，此前不触发任何提示，用户只看到「画了
        // 却没有任何记录」而日志一片正常。实证见 diagnostics_20261018_155346：
        // penSamples=759 且全为 tip=0 悬停、mouseStrokes=182、undo=11（确实在作画）。
        out += "  !! 收到了笔报文、也解析成功，但**没有任何落笔**（采样全为悬停）——\n"
               "     笔尖接触数据很可能被绘画软件独占（如 CSP 开启 Wintab），本程序只能拿到悬停。\n"
               "     此时鼠标笔画会被最后一层回退记为画笔（见上方 mouseAsPenStrokes），但无压感。\n"
               "     若要压感：在绘图软件里把笔输入从 Wintab 改回 Windows Ink。\n";
    }
    out += "  已识别设备:\n";
    for (const auto& n : cc.deviceNames) out += "    " + n + "\n";
    if (cc.deviceNames.empty()) out += "    （无）\n";

    // 覆写文件：存在则一并附上，因为它是人工干预的证据
    std::string ovPath = DataDir() + "\\pen_overrides.json";
    std::string ovText;
    bool hasOv = ReadWholeFile(ovPath, ovText);
    out += "\n覆写文件 pen_overrides.json: ";
    out += hasOv ? ("存在（" + std::to_string(FileSizeBytes(ovPath)) + " 字节）\n")
                 : "不存在（走全自动路径）\n";
    if (hasOv) out += ovText + "\n";

    // pb.log 中的设备诊断行（只筛 [pen] / [cap]，排除隐私行）。
    // logText 已在上面读过，这里复用；保留**最后** 400 行（最新会话最有诊断价值，
    // 旧逻辑取前 400 行，日志一长就把当前会话截掉了）；并按会话分段标注。
    out += "\npb.log 设备诊断摘录（仅 [pen]/[cap]，不含窗口标题与文档名）:\n";
    if (haveLog) {
        std::vector<std::string> keep;
        size_t pos = 0;
        while (pos < logText.size()) {
            size_t nl = logText.find('\n', pos);
            std::string line = logText.substr(pos, nl == std::string::npos
                                                    ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? logText.size() : nl + 1;
            if (line.find("] [pen]") != std::string::npos ||
                line.find("] [cap]") != std::string::npos) {
                keep.push_back(line);
            }
        }
        const size_t kMax = 400;
        size_t start = keep.size() > kMax ? keep.size() - kMax : 0;
        if (start > 0) out += "  （日志较长，仅保留最后 400 行 [pen]/[cap] 记录）\n";
        // 最后一个 registered 行 = 当前会话起点
        size_t lastReg = (size_t)-1;
        for (size_t i = 0; i < keep.size(); ++i)
            if (keep[i].find("registered digitizer usages") != std::string::npos) lastReg = i;
        for (size_t i = start; i < keep.size(); ++i) {
            if (keep[i].find("registered digitizer usages") != std::string::npos)
                out += (i == lastReg) ? "  ---- 当前会话（最新）----\n"
                                      : "  ---- 历史会话 ----\n";
            out += keep[i] + "\n";
        }
        if (keep.empty()) out += "  （无 [pen]/[cap] 记录）\n";
    } else {
        out += "  （pb.log 读取失败或不存在）\n";
    }

    out += "\n==== 报告结束 ====\n";
    out += "回传本文件即可协助定位数位设备兼容性问题。\n";

    // 写入数据目录（与 pb.log 同处，用户已知道在哪）
    char fname[64];
    strftime(fname, sizeof(fname), "diagnostics_%Y%m%d_%H%M%S.txt", &lt);
    std::string path = DataDir() + "\\" + fname;
    bool ok = WriteWholeFileAtomic(path, out);
    LogF("[ui] 导出诊断信息 -> %s (%s, %zu 字节)",
         path.c_str(), ok ? "成功" : "失败", out.size());

    if (ok) {
        std::wstring msg = L"已导出到数据目录：\n" + U2W(fname) +
            L"\n\n内容仅含设备与采集诊断信息，不含窗口标题、绘画文档名等隐私。\n"
            L"已为你打开所在文件夹。";
        MessageBoxW(hwnd_, msg.c_str(), L"导出诊断信息", MB_OK | MB_ICONINFORMATION);
        ShellExecuteW(hwnd_, L"open", U2W(DataDir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        MessageBoxW(hwnd_, L"导出失败，请检查数据目录是否可写。",
                    L"导出诊断信息", MB_OK | MB_ICONWARNING);
    }
}

// 录屏页滑块：切换「是否录屏」。
// 关键点：录屏线程是否运行必须与开关即时同步 ——
//   · 打开：若线程没在跑就启动（否则要等下次重启程序才生效）
//   · 关闭：线程自身会在下一轮轮询读到 recordVideo=false 并收尾当前文件，
//     这里不必强杀（强杀会让分片来不及正常收尾）。
void App::ToggleRecordVideo() {
    bool nv = !Cfg().recordVideo;
    Cfg().recordVideo = nv;
    CfgSave();
    if (nv && !Recorder::I().Running()) {
        if (Recorder::I().IsSupported()) {
            // 与 main.cpp 相同的启动方式：录屏需要采集层提供的活动状态
            Recorder::I().Start(&Recorder::MakeActivityInfo);
        }
    }
    LogF("[ui] 录屏开关 -> %s", nv ? "开" : "关");
    Invalidate();
}

// 切换捕获模式（整屏 ↔ 窗口）。
// 录制进行中禁止切换：切模式要重建捕获项与帧池，运行中改会让当前片段与
// 新片段的画面来源不一致。与「更改缓存目录」同样的约束（见 ChooseVideoCacheDir）。
void App::ToggleCaptureMode() {
    if (Cfg().recordVideo) {
        if (hwnd_) MessageBoxW(hwnd_, L"正在录制中，请先关闭录屏开关再切换录制模式。",
                               L"绘画伴侣", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int nv = (Cfg().captureMode == 1) ? 0 : 1;
    Cfg().captureMode = nv;
    CfgSave();
    LogF("[ui] 录制模式 -> %s", nv == 1 ? "窗口" : "整屏");
    Invalidate();
}

// 文件夹选择对话框（返回空串表示用户取消）。
// 用 IFileOpenDialog（Vista+）而非已废弃的 SHBrowseForFolder：
// 后者拿不到可直接输入的路径、且在高分屏下渲染模糊。
static std::wstring PickFolder(HWND owner, const std::wstring& title, const std::wstring& startDir) {
    // IFileOpenDialog 要求调用线程已初始化 COM，而主界面线程并未全程初始化
    // （此前只有离屏预览工具初始化过）。用 RAII 保证**每个返回路径**都配对还原；
    // 若线程已被其它方式初始化（RPC_E_CHANGED_MODE / S_FALSE）则不归我们注销。
    struct CoScope {
        bool owned = false;
        CoScope()  { owned = SUCCEEDED(CoInitializeEx(nullptr,
                                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }
        ~CoScope() { if (owned) CoUninitialize(); }
    } coScope;

    std::wstring result;
    winrt::com_ptr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dlg.put()))) || !dlg) return result;
    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts)))
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    dlg->SetTitle(title.c_str());
    if (!startDir.empty() && DirExistsW(startDir)) {
        winrt::com_ptr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr,
                                                  IID_PPV_ARGS(start.put()))) && start)
            dlg->SetFolder(start.get());
    }
    if (FAILED(dlg->Show(owner))) return result;
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.put())) || !item) return result;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = path;
        CoTaskMemFree(path);
    }
    return result;
}

void App::ChooseVideoCacheDir() {
    std::wstring cur = VideoDirW();
    std::wstring picked = PickFolder(hwnd_, L"选择录屏缓存目录", cur);
    if (picked.empty()) return;                       // 取消

    std::string pickedU = W2U(picked);
    if (pickedU == Cfg().videoCacheDir) return;       // 没变

    // 录制中不让改：会话正绑在旧目录的文件夹里，中途换根会造成
    // 「同一文档分散在两个目录」，卡片扫描也会看到两半。
    if (Recorder::I().Running() && Recorder::I().Snapshot().capturing) {
        MessageBoxW(hwnd_, L"正在录制中，请先关闭录屏开关再更改缓存目录。",
                    L"绘画伴侣", MB_OK | MB_ICONINFORMATION);
        return;
    }

    Cfg().videoCacheDir = pickedU;
    ApplyVideoDirOverride(Cfg().videoCacheDir);
    CfgSave();

    // 验证是否真的生效（不可写时 ApplyVideoDirOverride 会保留原值）
    if (VideoDir() != pickedU) {
        MessageBoxW(hwnd_, L"该目录不可用或不可写，录屏缓存目录未更改。",
                    L"绘画伴侣", MB_OK | MB_ICONWARNING);
    } else {
        // 目录变了 → 卡片列表必须重扫，缩略图缓存也要作废
        painter_.BitmapClear();
        recCards_.clear();
        ScanRecCards();
    }
    Invalidate();
}

void App::ChooseExportDir() {
    std::wstring cur = U2W(Cfg().exportDir);
    if (cur.empty()) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Videos, 0, nullptr, &pidl)) && pidl) {
            wchar_t buf[MAX_PATH] = {0};
            if (SHGetPathFromIDListW(pidl, buf)) cur = buf;
            CoTaskMemFree(pidl);
        }
    }
    std::wstring picked = PickFolder(hwnd_, L"选择默认导出目录", cur);
    if (picked.empty()) return;
    std::string pickedU = W2U(picked);
    if (pickedU == Cfg().exportDir) return;
    Cfg().exportDir = pickedU;
    CfgSave();
    Invalidate();
}

// ============================================================
//  托盘
// ============================================================
void App::TrayCreate() {
    if (!hwnd_) return;
    // 尺寸按窗口当前 DPI 取，不能用 SM_CXSMICON 的系统级值：
    // Per-Monitor V2 进程跨屏拖动后系统级值不反映实际显示器，图标会被拉伸。
    trayIcon_ = LoadAppIcon(SmallIconSizeForDpi(hwnd_));
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = trayIcon_ ? trayIcon_ : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"绘画伴侣 · 正在无感记录");
    Shell_NotifyIconW(NIM_ADD, &nid);
    trayActive_ = true;
}

void App::TrayDestroy() {
    if (!trayActive_ || !hwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayActive_ = false;
    if (trayIcon_) { DestroyIcon(trayIcon_); trayIcon_ = nullptr; }
}

void App::TrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!trayActive_ || !hwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    // 请求 8 秒。Win10+ 的通知时长实际由系统「通知和操作」设置决定
    // （uTimeout 已不再是精确控制项），这里只表达意图。
    nid.uTimeout = 8000;
    wcscpy_s(nid.szInfoTitle, title ? title : L"绘画伴侣");
    wcscpy_s(nid.szInfo, text ? text : L"");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 翻转「开机自启」：注册表 + 配置 + 重绘。设置页开关与托盘菜单共用此入口，
// 保证两处状态始终一致。
// 失败时**直接 return、不动配置** —— 配置保持原值，设置页开关（读 Cfg().autoStart）
// 会维持原样重绘，不会出现「UI 显示已开启、注册表里其实没有」的背离。
// 失败最常见的成因是安全软件拦截 Run 键写入，故用气泡把原因告知用户。
void App::ToggleAutoStart() {
    bool want = !Cfg().autoStart;
    if (!AutoStartApply(want)) {
        TrayBalloon(L"设置失败", L"开机自动启动写入注册表失败，可能被安全软件拦截。");
        LogF("[autostart] 用户切换失败：目标=%d", (int)want);
        return;
    }
    Cfg().autoStart = want;
    CfgSave();
    LogF("[autostart] 用户切换：开机自启 -> %s", want ? "开" : "关");
    Invalidate();
}

void App::ShowMainWindow(bool show) {
    if (!hwnd_) return;
    if (show) {
        ShowWindow(hwnd_, SW_SHOW);
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        Invalidate();
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void App::ShowTrayMenu() {
    if (!hwnd_) return;
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"打开绘画伴侣");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (Capture::I().IsRunning() ? 0 : MF_CHECKED), 2,
                Capture::I().IsRunning() ? L"暂停记录" : L"恢复记录");
    // 「开机启动」：勾选状态读注册表真实值（AutoStartEnabled），而非仅读配置，
    // 这样即使注册表被外部改动（安全软件清理、手动删除），菜单也能如实反映。
    AppendMenuW(menu, MF_STRING | (AutoStartEnabled() ? MF_CHECKED : 0), 4, L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"退出");
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
    case 1: ShowMainWindow(true); break;
    case 2: {
        if (Capture::I().IsRunning()) { Capture::I().Stop(); }
        else { Capture::I().Start(); }
        break;
    }
    case 4: ToggleAutoStart(); break;
    case 3: PostMessageW(hwnd_, WM_QUIT_EXIT, 0, 0); break;
    default: break;
    }
}

// ============================================================
//  运行
// ============================================================
// 离屏渲染：把侧栏 + 标题栏 + 指定页面画进 PNG，用于无头验证布局。
// 直接让成员 painter_ 切到离屏目标，复用全部现有绘制函数（它们本就用 painter_）。
// 仅供无头测试进程使用；调用后 painter_ 处于离屏模式。
bool App::RenderPagePng(int tab, const std::string& path, int W, int H, float dpi) {
    if (tab < 0 || tab > 3) return false;
    if (!painter_.CreateOffscreen((UINT)W, (UINT)H, dpi)) return false;
    painter_.SetTheme(theme_);

    int oldTab = tab_;
    tab_ = tab;
    scrollY_ = 0.f;
    ganttScroll_ = 0.f;

    // 布局用 DIP 尺寸（painter_.W()/H()），与真实窗口 OnPaint 完全同口径。
    // W/H 是物理像素；dpi>96 时 DIP 宽 = W*96/dpi，正好复现高分屏下
    // 客户区能容纳的逻辑尺寸变小 → 内容裁切的情况，便于无头回归。
    float w = painter_.W(), h = painter_.H();
    painter_.Begin(&theme_.bg);
    DrawSidebar(RF(0.f, 0.f, sideW_, h));
    DrawTitleBar(RF(sideW_, 0.f, w, titleH_));
    {
        D2D1_RECT_F content = RF(sideW_, titleH_, w, h);
        painter_.PushClip(content);
        switch (tab) {
        case 0: DrawOverview(content); break;
        case 1: DrawHabit(content);    break;
        case 2: DrawRecord(content);   break;
        case 3: DrawSettings(content); break;
        default: break;
        }
        painter_.PopClip();
    }
    painter_.End();

    tab_ = oldTab;
    return painter_.SavePng(path);
}

// 无头预览初始化：仅准备渲染所需状态，不建窗口、不装钩子、不启动采集/录屏
void App::InitForHeadlessPreview(bool dark, const Analysis* seedAnalysis) {
    dark_ = dark;
    theme_ = dark ? Theme::Dark() : Theme::Light();
    painter_.SetTheme(theme_);
    inputs_.clear();
    for (int i = 0; i < IN_COUNT; ++i) inputs_.push_back(TextInput());
    SyncInputsFromConfig();
    // 标签顺序必须与 HabitView::Panel 枚举序号一致（点击按索引直接转成 Panel）
    habitTabs_ = { L"习惯概览", L"一阶转移", L"长程关联", L"二阶条件" };
    if (seedAnalysis && !seedAnalysis->empty) {
        analysis_ = *seedAnalysis;
        dataReady_ = true;
        gantt_.SetData(analysis_);
        gantt_.SetTheme(theme_);
        habit_.SetData(analysis_);
        habit_.SetTheme(theme_);
    }
}

int App::Run(HINSTANCE hInst, int nCmdShow, bool trayMode) {
    hInst_ = hInst;

    // 必须用 WNDCLASSEXW 而不是 WNDCLASSW：只有 EX 版本才有 hIconSm 成员。
    // 原来用的 WNDCLASSW 根本没有这个字段，所以标题栏那个 16px 小图标一直是
    // 系统拿 32px 大图标降采样填的，高分屏下明显发虚 —— 不是「忘了设」，
    // 是那个结构体压根设不了。cbSize 必须填对，否则 RegisterClassEx 失败。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = PB_WNDCLASS;
    // 大图标（任务栏运行中按钮、Alt+Tab）与小图标（标题栏左上角、任务栏
    // 小尺寸场景）都要显式给。注册窗口类时尚无 hwnd，此处按系统 DPI 取值；
    // 窗口真正落到某个显示器后由 OnCreate / OnDpiChanged 用 WM_SETICON
    // 换成该显示器 DPI 下的尺寸（见 RefreshIconsForDpi）。
    wc.hIcon = LoadAppIcon(LargeIconSizeForDpi(nullptr));
    wc.hIconSm = LoadAppIcon(SmallIconSizeForDpi(nullptr));
    if (!RegisterClassExW(&wc)) {
        LogF("[app] RegisterClassEx 失败 %lu", GetLastError());
        return 1;
    }

    // 窗口初始尺寸：以「设计客户区」为基准，按 DPI 放大后夹到工作区内，
    // 再由客户区反算窗口外框尺寸。
    //
    // 修掉三个跨机器缺陷：
    // ① 原来直接把 1240×820 当窗口外框传给 CreateWindowEx，客户区被边框与
    //    标题栏吃掉约 16×39，布局本就紧张时更容易溢出；
    // ② 原来用 SM_CYSCREEN（含任务栏的整屏高度），1366×768 一类机器上
    //    820 高的窗口底部会被屏幕和任务栏一起切掉，且没有任何收缩逻辑；
    // ③ 原来用全屏尺寸而非工作区定位，任务栏会压住窗口下沿。
    float dpi = (float)WindowDpi(nullptr);
    float sc = dpi / 96.f;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = work.top = 0;
        work.right  = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int workW = work.right - work.left, workH = work.bottom - work.top;

    // 目标客户区（物理像素），不小于布局最小可用尺寸
    int minCw = (int)(MIN_LAYOUT_W * sc), minCh = (int)(MIN_LAYOUT_H * sc);
    int cw = std::max(minCw, std::min((int)(1240.f * sc), workW));
    int ch = std::max(minCh, std::min((int)(820.f * sc), workH));

    // 客户区 → 窗口外框。AdjustWindowRectExForDpi 在 Win10 1607+ 才有，
    // 动态取以兼容更早系统；取不到就退回不带 DPI 的版本（外框差几像素，
    // 不影响客户区已达到的最小可用尺寸）。
    RECT rc{ 0, 0, cw, ch };
    typedef BOOL (WINAPI *AdjustForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustForDpiFn adjFn = (AdjustForDpiFn)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (adjFn) adjFn(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)dpi);
    else       AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int px = work.left + std::max(0, (workW - w) / 2);
    int py = work.top  + std::max(0, (workH - h) / 2);

    hwnd_ = CreateWindowExW(0, PB_WNDCLASS, L"绘画伴侣",
                            WS_OVERLAPPEDWINDOW, px, py, w, h,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        LogF("[app] CreateWindow 失败 %lu", GetLastError());
        return 1;
    }
    // 窗口创建时没有 WS_VISIBLE，默认就是隐藏的。
    // trayMode（开机自启，--tray）：跳过首次显示 → 无窗口、有托盘、采集照常跑。
    // 托盘图标在 OnCreate 里已建好（见 TrayCreate 调用），故此处无需额外处理。
    if (!trayMode) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
    } else {
        // 开机时桌面上什么都没有，用户容易以为程序没启动 → 延迟弹一次气泡。
        // 不在这里直接弹：开机瞬间 explorer 可能还没准备好接收托盘通知。
        LogF("[app] 开机自启：静默进托盘，不显示窗口");
        SetTimer(hwnd_, TID_TRAY_BUBBLE, TRAY_BUBBLE_DELAY_MS, nullptr);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT_EXIT) {
            // 托盘/按钮请求退出：销毁窗口 → WM_DESTROY → PostQuitMessage 结束循环
            if (hwnd_) DestroyWindow(hwnd_);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

} // namespace pb
