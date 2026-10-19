// ============================================================
//  WGC 帧池缩放行为探测（一次性诊断程序，不参与产品构建）
//
//  要回答的问题：帧池按「建会话时的固定尺寸」创建，窗口 resize 之后，
//  WGC 交付的帧纹理到底是
//    (a) 原生新尺寸  → Recorder::OnFrame 里现有的 texW/texH 比对可用
//    (b) 被缩放到池尺寸 → 该比对永不触发，必须改用 item.SizeChanged
//                         或轮询 GetWindowRect 作为切分信号
//
//  做法：自建一个测试窗口，用 SetWindowPos 确定性地改尺寸（含同比例、
//  异比例、竖屏三类），每个阶段记录 item.Size()、SizeChanged 是否触发、
//  以及实际交付纹理的 GetDesc 尺寸。不依赖用户的绘画软件。
//
//  输出全部为 ASCII，避免控制台/文件编码干扰读数。
// ============================================================
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

using WinInspectable = winrt::Windows::Foundation::IInspectable;
using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

// ============================================================
//  全局探测状态
// ============================================================
static HWND g_hwnd = nullptr;

static com_ptr<ID3D11Device>        g_d3dDevice;
static com_ptr<ID3D11DeviceContext> g_d3dCtx;
static IDirect3DDevice              g_wgcDevice{ nullptr };
static GraphicsCaptureItem          g_item{ nullptr };
static Direct3D11CaptureFramePool   g_pool{ nullptr };
static GraphicsCaptureSession       g_session{ nullptr };
static winrt::event_token           g_frameToken;
static winrt::event_token           g_closedToken;

// 帧池创建时使用的尺寸（固定不变，这是关键前提）
static SizeInt32 g_poolSz{ 0, 0 };

// 当前阶段标签
static const char* g_phase = "init";

// 本阶段统计
static int       g_frames = 0;          // 本阶段收到的帧数
static int       g_texW = 0, g_texH = 0; // 交付纹理尺寸（取首帧）
static bool      g_texVaried = false;    // 本阶段内纹理尺寸是否出现过多种值
static bool      g_itemClosed = false;

// —— ContentSize：帧自带的「内容原生尺寸」，是本探测的核心信号 ——
// SDK 10.0.26100 的 GraphicsCaptureItem **没有** SizeChanged 事件
// （只有 Closed），因此尺寸变化只能靠这两条路之一发现：
//   (a) 每帧 frame.ContentSize()
//   (b) 轮询 item.Size()
static int       g_csW = 0, g_csH = 0;  // 首帧 ContentSize
static bool      g_csVaried = false;    // 本阶段内 ContentSize 是否变过
static int       g_csMaxW = 0, g_csMaxH = 0;

// 轮询 item.Size() 观察到的变化次数（全程累计）
static int       g_itemSizePollChanges = 0;
static SizeInt32 g_itemSizePollLast{ 0, 0 };

static void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
}

// ============================================================
//  测试窗口
// ============================================================
static const wchar_t* const kCls = L"PBPoolProbeWnd";

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        // 画个纯色 + 对角线，便于肉眼确认内容确实是本窗口
        HBRUSH br = CreateSolidBrush(RGB(40, 120, 200));
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 0));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 0, 0, nullptr);
        LineTo(dc, rc.right, rc.bottom);
        SelectObject(dc, old);
        DeleteObject(pen);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;   // 自己全量绘制，避免闪烁干扰帧统计
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND CreateTestWindow(int w, int h) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kCls;
    if (!RegisterClassExW(&wc)) { Log("RegisterClassExW failed err=%lu", GetLastError()); return nullptr; }

    // 用客户区尺寸反推窗口尺寸，保证纹理尺寸可预期
    RECT rc{ 0, 0, w, h };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZE), FALSE, 0);

    HWND hwnd = CreateWindowExW(0, kCls, L"PB Pool Probe",
                                WS_OVERLAPPEDWINDOW,
                                100, 100, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("CreateWindowExW failed err=%lu", GetLastError()); return nullptr; }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return hwnd;
}

// 按客户区尺寸改窗口大小（返回实际客户区尺寸）
static bool ResizeClient(int cw, int ch, int& outW, int& outH) {
    RECT rc{ 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZE), FALSE, 0);
    BOOL ok = SetWindowPos(g_hwnd, nullptr, 0, 0,
                           rc.right - rc.left, rc.bottom - rc.top,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!ok) { Log("SetWindowPos failed err=%lu", GetLastError()); return false; }
    RECT c{};
    GetClientRect(g_hwnd, &c);
    outW = c.right - c.left;
    outH = c.bottom - c.top;
    return true;
}

// ============================================================
//  D3D / WGC 初始化
// ============================================================
static bool InitD3D() {
    D3D11_CREATE_DEVICE_FLAG flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                               D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fl, (UINT)std::size(fl), D3D11_SDK_VERSION,
                                   g_d3dDevice.put(), &got, g_d3dCtx.put());
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               fl, (UINT)std::size(fl), D3D11_SDK_VERSION,
                               g_d3dDevice.put(), &got, g_d3dCtx.put());
        if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08lX", (unsigned long)hr); return false; }
        Log("using WARP software device");
    }
    com_ptr<IDXGIDevice> dxgi;
    try { dxgi = g_d3dDevice.as<IDXGIDevice>(); } catch (...) { dxgi = nullptr; }
    if (!dxgi) { Log("QueryInterface IDXGIDevice failed"); return false; }
    com_ptr<::IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    if (FAILED(hr) || !insp) { Log("CreateDirect3D11DeviceFromDXGIDevice failed 0x%08lX", (unsigned long)hr); return false; }
    g_wgcDevice = insp.as<IDirect3DDevice>();
    if (!g_wgcDevice) { Log("cast to IDirect3DDevice failed"); return false; }
    return true;
}

// ============================================================
//  帧回调：只测尺寸，不编码
// ============================================================
static void OnFrame(Direct3D11CaptureFramePool const&, WinInspectable const&) {
    Direct3D11CaptureFrame frame{ nullptr };
    try { frame = g_pool.TryGetNextFrame(); } catch (...) { return; }
    if (!frame) return;

    try {
        // ContentSize 是 WGC 报告的「内容原生尺寸」，先取它：
        // 即使纹理被池缩放，ContentSize 仍应反映真实窗口大小。
        SizeInt32 cs = frame.ContentSize();
        if (g_frames == 0) { g_csW = cs.Width; g_csH = cs.Height; }
        else if (cs.Width != g_csW || cs.Height != g_csH) { g_csVaried = true; }
        if (cs.Width > g_csMaxW) g_csMaxW = cs.Width;
        if (cs.Height > g_csMaxH) g_csMaxH = cs.Height;

        auto surface = frame.Surface();
        if (surface) {
            IUnknown* unk = winrt::get_unknown(surface);
            if (unk) {
                DxgiInterfaceAccess* raw = nullptr;
                if (SUCCEEDED(unk->QueryInterface(IID_PPV_ARGS(&raw))) && raw) {
                    com_ptr<DxgiInterfaceAccess> access;
                    access.attach(raw);
                    com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(tex.put()))) && tex) {
                        D3D11_TEXTURE2D_DESC d{};
                        tex->GetDesc(&d);
                        int w = (int)d.Width, h = (int)d.Height;
                        if (g_frames == 0) { g_texW = w; g_texH = h; }
                        else if (w != g_texW || h != g_texH) { g_texVaried = true; }
                    }
                }
            }
        }
        if (g_frames < 3 || g_frames % 25 == 0) {
            Log("    frame[%s] #%d  tex=%dx%d  ContentSize=%dx%d",
                g_phase, g_frames, g_texW, g_texH, cs.Width, cs.Height);
        }
        g_frames++;
    } catch (...) {}
    try { frame.Close(); } catch (...) {}
}

static void OnItemClosed(GraphicsCaptureItem const&, WinInspectable const&) {
    g_itemClosed = true;
    Log("    >>> item.Closed fired [phase=%s]", g_phase);
}

// 轮询 item.Size()：这是 SizeChanged 缺失后的替代信号，
// 录屏线程本来就每 120ms 轮询一次，加这个调用零额外结构成本。
static void PollItemSize() {
    if (!g_item) return;
    try {
        SizeInt32 s = g_item.Size();
        if (g_itemSizePollLast.Width != 0 &&
            (s.Width != g_itemSizePollLast.Width || s.Height != g_itemSizePollLast.Height)) {
            g_itemSizePollChanges++;
            Log("    >>> item.Size() POLL changed: %dx%d -> %dx%d (#%d) [phase=%s]",
                g_itemSizePollLast.Width, g_itemSizePollLast.Height,
                s.Width, s.Height, g_itemSizePollChanges, g_phase);
        }
        g_itemSizePollLast = s;
    } catch (...) {}
}

// ============================================================
//  启动捕获：帧池按当前 item.Size() 固定创建（复刻 Recorder 的做法）
// ============================================================
static bool StartCapture(HWND hwnd) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem newItem{ nullptr };
    HRESULT hr = interop->CreateForWindow(hwnd, guid_of<IGraphicsCaptureItem>(),
                                          winrt::put_abi(newItem));
    if (FAILED(hr) || !newItem) {
        Log("CreateForWindow FAILED 0x%08lX", (unsigned long)hr);
        return false;
    }
    g_item = newItem;

    SizeInt32 sz = g_item.Size();
    Log("item.Size() at session start = %dx%d", sz.Width, sz.Height);

    // 偶数对齐，与 Recorder 一致
    g_poolSz.Width  = sz.Width  & ~1;
    g_poolSz.Height = sz.Height & ~1;
    if (g_poolSz.Width < 16 || g_poolSz.Height < 16) {
        Log("item size too small %dx%d", g_poolSz.Width, g_poolSz.Height);
        return false;
    }
    Log("frame pool created with FIXED size %dx%d (buffers=2)", g_poolSz.Width, g_poolSz.Height);

    try {
        g_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_wgcDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, g_poolSz);
    } catch (const std::exception& e) {
        Log("CreateFreeThreaded threw: %s", e.what());
        return false;
    }
    if (!g_pool) { Log("frame pool creation returned null"); return false; }

    try {
        g_session = g_pool.CreateCaptureSession(g_item);
        try { g_session.IsBorderRequired(false); } catch (...) {}
    } catch (const std::exception& e) {
        Log("CreateCaptureSession threw: %s", e.what());
        return false;
    }
    if (!g_session) { Log("session null"); return false; }

    g_frameToken = g_pool.FrameArrived({ &OnFrame });
    g_closedToken = g_item.Closed({ &OnItemClosed });
    // 注意：SDK 10.0.26100 的 GraphicsCaptureItem 没有 SizeChanged 事件，
    // 这里刻意不注册；尺寸变化改由 PollItemSize() 与帧 ContentSize 探测。
    g_itemSizePollLast = g_poolSz;

    g_session.StartCapture();
    Log("capture started");
    return true;
}

// ============================================================
//  消息泵：跑指定毫秒，期间正常派发（WGC 回调在 free-threaded 池上，
//  但窗口消息必须泵，否则 resize 不生效）。
//  每次循环顺带轮询 item.Size()，模拟录屏线程 120ms 轮询的行为。
// ============================================================
static bool PumpFor(int ms) {
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)ms;
    MSG m;
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) return false;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        PollItemSize();
        Sleep(8);
    }
    return true;
}

// ============================================================
//  一个探测阶段
// ============================================================
struct PhaseResult {
    std::string name;
    int clientW = 0, clientH = 0;
    int frames = 0;
    int texW = 0, texH = 0;
    bool texVaried = false;
    int csW = 0, csH = 0;          // 首帧 ContentSize
    bool csVaried = false;         // 本阶段内 ContentSize 是否变过
    int pollChanges = 0;           // 本阶段内 item.Size() 轮询到的变化次数
    SizeInt32 itemSizeEnd{ 0, 0 };
};

static PhaseResult RunPhase(const char* name, int clientW, int clientH, int observeMs) {
    g_phase = name;
    g_frames = 0; g_texW = 0; g_texH = 0; g_texVaried = false;
    g_csW = 0; g_csH = 0; g_csVaried = false;

    int ow = 0, oh = 0;
    bool resized = ResizeClient(clientW, clientH, ow, oh);

    Log("");
    Log("---- PHASE %s : request client %dx%d ----", name, clientW, clientH);
    if (!resized) Log("  (resize FAILED)");
    else Log("  actual client after SetWindowPos = %dx%d", ow, oh);

    SizeInt32 before = g_item ? g_item.Size() : SizeInt32{ 0, 0 };
    Log("  item.Size() right after resize = %dx%d  (pool is FIXED %dx%d)",
        before.Width, before.Height, g_poolSz.Width, g_poolSz.Height);

    int pollBefore = g_itemSizePollChanges;
    PumpFor(observeMs);

    SizeInt32 after = g_item ? g_item.Size() : SizeInt32{ 0, 0 };

    PhaseResult r;
    r.name = name;
    r.clientW = ow; r.clientH = oh;
    r.frames = g_frames;
    r.texW = g_texW; r.texH = g_texH;
    r.texVaried = g_texVaried;
    r.csW = g_csW; r.csH = g_csH;
    r.csVaried = g_csVaried;
    r.pollChanges = g_itemSizePollChanges - pollBefore;
    r.itemSizeEnd = after;
    Log("  result: frames=%d  deliveredTex=%dx%d (varied=%s)",
        r.frames, r.texW, r.texH, r.texVaried ? "YES" : "no");
    Log("  result: ContentSize first=%dx%d (variedWithinPhase=%s)",
        r.csW, r.csH, r.csVaried ? "YES" : "no");
    Log("  result: item.Size() end=%dx%d  poll-detected changes in phase=%d",
        after.Width, after.Height, r.pollChanges);
    return r;
}

// ============================================================
int main() {
    // 让窗口尺寸按物理像素走，避免 DPI 虚拟化混淆读数
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Log("=======================================================");
    Log(" WGC frame-pool scaling probe");
    Log("=======================================================");

    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (const std::exception& e) { Log("init_apartment threw: %s", e.what()); }

    if (!GraphicsCaptureSession::IsSupported()) {
        Log("GraphicsCaptureSession::IsSupported() == FALSE, cannot probe");
        return 2;
    }
    Log("GraphicsCaptureSession::IsSupported() == TRUE");

    g_hwnd = CreateTestWindow(800, 600);
    if (!g_hwnd) return 2;
    RECT c0{}; GetClientRect(g_hwnd, &c0);
    Log("test window created, client = %dx%d", c0.right - c0.left, c0.bottom - c0.top);

    if (!InitD3D()) return 2;

    PumpFor(400);   // 让窗口先完成首次绘制

    if (!StartCapture(g_hwnd)) {
        Log("capture could not start; aborting probe");
        DestroyWindow(g_hwnd);
        return 2;
    }

    std::vector<PhaseResult> results;

    // 基线：不动，确认能稳定收到帧
    results.push_back(RunPhase("baseline-800x600", 800, 600, 1200));

    // 同比例放大（4:3 -> 4:3）
    results.push_back(RunPhase("same-ratio-1200x900", 1200, 900, 1200));

    // 异比例（4:3 -> 16:9）
    results.push_back(RunPhase("diff-ratio-1280x720", 1280, 720, 1200));

    // 缩小 + 竖屏
    results.push_back(RunPhase("portrait-600x1000", 600, 1000, 1200));

    // 连续快速 resize（模拟拖动窗口），观察是否每步都触发
    Log("");
    Log("---- PHASE rapid-resize : 12 steps, 60ms apart ----");
    g_phase = "rapid-resize";
    g_frames = 0; g_texW = 0; g_texH = 0; g_texVaried = false;
    g_csW = 0; g_csH = 0; g_csVaried = false;
    int beforeRapid = g_itemSizePollChanges;
    for (int i = 0; i < 12; ++i) {
        int w = 700 + i * 40, h = 500 + i * 30;
        int ow = 0, oh = 0;
        ResizeClient(w, h, ow, oh);
        PumpFor(60);
    }
    PumpFor(600);
    Log("  rapid-resize: frames=%d  deliveredTex=%dx%d (varied=%s)",
        g_frames, g_texW, g_texH, g_texVaried ? "YES" : "no");
    Log("  rapid-resize: ContentSize first=%dx%d (varied=%s)",
        g_csW, g_csH, g_csVaried ? "YES" : "no");
    Log("  rapid-resize: item.Size() poll detected %d change(s) during 12 steps",
        g_itemSizePollChanges - beforeRapid);

    PhaseResult rr;
    rr.name = "rapid-resize";
    rr.frames = g_frames; rr.texW = g_texW; rr.texH = g_texH;
    rr.texVaried = g_texVaried;
    rr.csW = g_csW; rr.csH = g_csH; rr.csVaried = g_csVaried;
    rr.pollChanges = g_itemSizePollChanges - beforeRapid;
    rr.itemSizeEnd = g_item ? g_item.Size() : SizeInt32{ 0, 0 };
    results.push_back(rr);

    // 回到原始尺寸，确认还能收到帧（会话未被打断）
    results.push_back(RunPhase("back-to-800x600", 800, 600, 1200));

    // ============ 汇总 ============
    Log("");
    Log("=======================================================");
    Log(" SUMMARY  (pool was FIXED at %dx%d for the whole run)",
        g_poolSz.Width, g_poolSz.Height);
    Log("=======================================================");
    Log("%-24s %10s %14s %12s %10s",
        "phase", "client", "deliveredTex", "item.Size", "texVaried");
    for (const auto& r : results) {
        char cli[32], tex[32], its[32];
        snprintf(cli, sizeof(cli), "%dx%d", r.clientW, r.clientH);
        snprintf(tex, sizeof(tex), "%dx%d", r.texW, r.texH);
        snprintf(its, sizeof(its), "%dx%d", r.itemSizeEnd.Width, r.itemSizeEnd.Height);
        Log("%-24s %10s %14s %12s %10s",
            r.name.c_str(), cli, tex, its, r.texVaried ? "YES" : "no");
    }

    Log("");
    Log("item.Size() poll total changes = %d ; item.Closed = %s",
        g_itemSizePollChanges, g_itemClosed ? "FIRED" : "not fired");

    // ============ 判定 ============
    Log("");
    Log("=======================================================");
    Log(" VERDICT");
    Log("=======================================================");

    bool texFollowsNative = false;   // 交付纹理跟随原生尺寸变化
    bool texStuckToPool   = false;   // 交付纹理始终等于池尺寸
    bool csFollowsNative  = false;   // ContentSize 跟随窗口原生尺寸
    for (const auto& r : results) {
        if (r.frames <= 0) continue;
        if (r.texW == g_poolSz.Width && r.texH == g_poolSz.Height) texStuckToPool = true;
        else texFollowsNative = true;
        if (r.csW != g_poolSz.Width || r.csH != g_poolSz.Height) csFollowsNative = true;
    }

    Log("[1] SDK has NO GraphicsCaptureItem::SizeChanged event (only Closed).");
    Log("    -> split signal must come from polling item.Size() or per-frame ContentSize.");

    if (g_itemSizePollChanges > 0)
        Log("[2] polling item.Size() DID detect %d change(s) -> item.Size() poll is a WORKABLE split signal",
            g_itemSizePollChanges);
    else
        Log("[2] polling item.Size() detected NO change -> item.Size() is unreliable here");

    if (texFollowsNative && !texStuckToPool)
        Log("[3] delivered TEXTURE follows native window size -> existing texW/texH compare in OnFrame WORKS");
    else if (texStuckToPool && !texFollowsNative)
        Log("[3] delivered TEXTURE is SCALED to fixed pool size -> texW/texH compare NEVER fires, MUST use another signal");
    else
        Log("[3] MIXED texture behaviour (both pool-size and native-size seen) -> treat tex compare as UNRELIABLE");

    if (csFollowsNative)
        Log("[4] frame.ContentSize() reflects NATIVE window size -> usable as a per-frame split signal");
    else
        Log("[4] frame.ContentSize() stayed at pool size -> not useful as a size signal");

    if (rr.texVaried)
        Log("[5] texture size VARIED within a single phase -> frames are not size-stable mid-session");
    else
        Log("[5] texture size stayed constant within each phase");

    Log("[6] session survived all resizes without item.Closed: %s",
        g_itemClosed ? "NO (closed fired)" : "YES");

    // 清理
    try { if (g_pool && g_frameToken) g_pool.FrameArrived(g_frameToken); } catch (...) {}
    try { if (g_item && g_closedToken) g_item.Closed(g_closedToken); } catch (...) {}
    try { if (g_session) g_session.Close(); } catch (...) {}
    try { if (g_pool) g_pool.Close(); } catch (...) {}
    g_session = nullptr; g_pool = nullptr; g_item = nullptr;

    if (g_hwnd) DestroyWindow(g_hwnd);
    PumpFor(150);
    try { winrt::uninit_apartment(); } catch (...) {}
    return 0;
}
