#include "Recorder.h"
#include "Config.h"
#include "Store.h"
#include "Video.h"
#include "Capture.h"     // MakeActivityInfo 需读采集层快照

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// inspectable.h 的 COM 版 ::IInspectable 与 WinRT 投影类型同名，
// 事件处理器签名必须显式限定，否则非限定名会解析到 COM 版本。
using WinInspectable = winrt::Windows::Foundation::IInspectable;
// interop 头把该接口放在全局 Windows::Graphics::... 命名空间下
using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

namespace pb {

// ============================================================
//  主窗口识别辅助（仅录屏线程使用）
//
//  背景：绘画软件的分离浮窗（CSP 色轮/工具属性、PS 液化/面板）与主窗口
//  同属一个进程，isTarget 只按进程名判定，无法区分。若直接用前台窗口标题
//  当文档名，点一下浮窗就会切出一张垃圾卡片（实测 videos\液化 (shijian\）。
//
//  判据：同进程「最大可见顶级窗口」= 绘画主窗口。
//   · 必须先过滤 IsWindowVisible —— NVOpenGLPbuffer 伪窗口
//     (__wglDummyWindowFodder / NVOGLDC invisible) 面积最大但不可见，
//     还会瞬时抢前台产生空标题。
//   · 最小化窗口 rect 落在 -32000 附近，按面积判会误选，故排除 left<-10000。
//  实测面积：CSP 主窗口 750000 > 工具属性 137776 > 色轮 97228；
//           PS 主窗口 1712448 >> 面板 10000~360787 量级。
//
//  注意：PS 液化是全屏式模态窗口，面积可能超过主窗口，故本函数只在
//  「未锁定」时调用；一旦锁定主窗口就沿用，不在液化打开期间重算
//  （见 Impl::ResolveAndLockMain）。
// ============================================================
namespace {

DWORD PidOfHwnd(HWND h) {
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid;
}

// 读取窗口标题。跨进程调用会向目标窗口发消息，目标忙时可能阻塞；
// 因此只在录屏线程调用 —— 阻塞只延迟录制决策，不影响采集线程的笔数据。
std::wstring ReadWndTitle(HWND h) {
    if (!h) return std::wstring();
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return std::wstring();
    std::wstring s((size_t)n + 1, L'\0');
    int got = GetWindowTextW(h, &s[0], n + 1);
    if (got <= 0) return std::wstring();
    s.resize((size_t)got);
    return s;
}

struct MainWndSearch {
    DWORD     pid = 0;
    HWND      best = nullptr;
    long long bestArea = -1;
};

BOOL CALLBACK EnumMainWndProc(HWND h, LPARAM lp) {
    auto* s = reinterpret_cast<MainWndSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;                 // 排除伪窗口/隐藏面板
    RECT r{};
    if (!GetWindowRect(h, &r)) return TRUE;
    if (r.left < -10000 || r.top < -10000) return TRUE;   // 最小化：rect 落在 -32000 附近
    long long w = (long long)(r.right - r.left);
    long long hh = (long long)(r.bottom - r.top);
    long long area = w * hh;
    if (area <= 0) return TRUE;
    if (area > s->bestArea) { s->bestArea = area; s->best = h; }
    return TRUE;
}

// 返回同 pid 的最大可见顶级窗口；找不到返回 nullptr。
HWND FindMainWindow(DWORD pid) {
    if (!pid) return nullptr;
    MainWndSearch s; s.pid = pid;
    EnumWindows(EnumMainWndProc, reinterpret_cast<LPARAM>(&s));
    return s.best;
}

// CSP 家族进程名（小写全名精确匹配，与 Config.cpp 的 IsTarget 口径一致）：
//   · clipstudiopaint.exe  CLIP STUDIO PAINT 本体（实测主窗口标题恒为产品名）
//   · clipstudio.exe       CSP 启动器（实测标题「CLIP STUDIO」、面积极小）
//   · udmpaintpro.exe      优动漫 PAINT（CSP 中国版，独立打包）
// 这三者结构上都拿不到文档名，故录屏归到同一张固定卡片。
bool IsCspFamilyExe(const std::string& exeLower) {
    return exeLower == "clipstudiopaint.exe"
        || exeLower == "clipstudio.exe"
        || exeLower == "udmpaintpro.exe";
}

// 判断从标题解析出的文档名是否为「CSP 家族产品名形态」或空/兜底名。
// 归一后（小写、去所有空白）匹配下列之一即视为产品名，而非真实文档名：
//   clipstudiopaint / clipstudio / 优动漫paint / 优动漫 / 绘画（空标题兜底）。
// 之所以还要看这个：万一某版本 CSP 主窗口标题真的带了文档名，
// 解析结果不会是这些产品名，此时应保留真实文档名而非强行归一。
bool IsCspProductNameOrEmpty(const std::wstring& doc) {
    std::wstring s;
    s.reserve(doc.size());
    for (wchar_t c : doc) {
        if (iswspace(c)) continue;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        s.push_back(c);
    }
    if (s.empty()) return true;
    return s == L"clipstudiopaint" || s == L"clipstudio"
        || s == L"优动漫paint" || s == L"优动漫"
        || s == L"绘画";
}

// 统一的文档名解析：先按标题提取，再对 CSP 家族做归一。
//
// **Loop 与 StartSession 必须共用本函数**：Loop 用它算 curDoc 做「是否换文档」
// 判断，StartSession 用它算 sessionDoc 绑定会话。若两处解析口径不同（例如一处
// 归一、一处没归一），启动器场景下 curDoc=「CLIP STUDIO PAINT」而 sessionDoc=
// 「CLIP STUDIO」，docChanged 会恒真 → 反复 EndSession/StartSession 产生碎片。
std::wstring ResolveDocName(HWND hwnd, const std::wstring& title) {
    std::wstring d = Recorder::DocNameFromTitle(title);
    std::string exe = ToLowerA(GetProcessExeName(PidOfHwnd(hwnd)));
    // CSP 家族（本体/启动器/优动漫）结构上拿不到文档名，标题只会是产品名。
    // 统一归到一张固定卡片，避免启动器「CLIP STUDIO」与本体「CLIP STUDIO PAINT」
    // 各成一张卡。若某版本标题真带了文档名（非产品名形态），则保留真实文档名。
    if (IsCspFamilyExe(exe) && IsCspProductNameOrEmpty(d))
        return L"CLIP STUDIO PAINT";
    return d;
}

} // namespace

// ============================================================
//  录屏（会话版：一个绘画文件 = 一个视频文件）
//
//  设计原则
//  ────────
//  1) 「先拿真帧，再建写入器」：不在开录时猜尺寸建 SinkWriter，而是等
//     第一个「确实要写入」的帧到达后，从 ID3D11Texture2D::GetDesc() 取
//     真实宽高再建。尺寸猜错这一整类 bug 从此不可能发生。
//  2) 非压缩 RGB32 输入必须提供 SAMPLE_SIZE / DEFAULT_STRIDE /
//     FIXED_SIZE_SAMPLES，否则 WriteSample 每帧返回 E_INVALIDARG
//     (0x80070057)——这正是此前反复失败的真正原因。
//  3) 会话按「绘画文档」绑定，不按空闲切片：
//       · 结束条件只有：换到另一个文档 / 关闭录制 / 窗口尺寸变化 /
//         捕获项关闭 / 退出程序。
//       · 停顿、切到别的软件再切回来，都继续写同一个文件 ——
//         停顿期的帧被丢弃，视频里自然看不到等待。
//       · 活跃判定用「采样级时刻 + 笔画进行中」，因此数秒的连续
//         长运笔绝不会被中途切断（此前只在笔画收尾时更新活跃时刻，
//         长笔画走到一半就被判空闲，实测被切在 1.20s）。
//  4) 低资源占用：帧池 2 个缓冲；写入器惰性创建；无活动的帧直接
//     丢弃不进编码器；轮询 120ms；D3D 设备进程内只建一次。
//  5) 文件名取「文档名」而非整条窗口标题：
//       "shijian.psd @ 11.1% (图层 199, RGB/8) *" -> shijian_时间戳.mp4
// ============================================================
struct Recorder::Impl {
    Recorder*              self = nullptr;
    HANDLE                 thread = nullptr;
    DWORD                  threadId = 0;
    volatile LONG          runFlag = 0;
    volatile LONG          finalizeReq = 0;
    std::function<ActivityInfo()> src;

    CRITICAL_SECTION       cs;         // 只保护 stats（UI 线程读取，绝不长时间持有）
    CRITICAL_SECTION       writerCs;   // 保护 writer/pool/session/capturing 生命周期
    RecorderStats          stats;

    Impl()  { InitializeCriticalSection(&cs); InitializeCriticalSection(&writerCs); }
    ~Impl() { DeleteCriticalSection(&cs); DeleteCriticalSection(&writerCs); }

    struct CsLock {
        CRITICAL_SECTION& c;
        CsLock(CRITICAL_SECTION& x) : c(x) { EnterCriticalSection(&c); }
        ~CsLock() { LeaveCriticalSection(&c); }
    };

    // —— D3D / WGC ——
    com_ptr<ID3D11Device>        d3dDevice;
    com_ptr<ID3D11DeviceContext> d3dCtx;
    IDirect3DDevice              wgcDevice{ nullptr };
    GraphicsCaptureItem          item{ nullptr };
    Direct3D11CaptureFramePool   pool{ nullptr };
    GraphicsCaptureSession       session{ nullptr };
    winrt::event_token           frameToken;
    winrt::event_token           closedToken;
    HWND                         capturedHwnd = nullptr;
    bool                         capturing = false;      // 捕获会话是否已启动
    // —— 主窗口锁定（仅录屏线程访问，无需加锁）——
    // 首次识别出绘画主窗口后记住它，只在其被销毁或切到另一进程时才重算，
    // 避免面板/液化抢焦点导致主窗口易主、文档名来回跳。
    HWND                         lockedMainHwnd = nullptr;
    DWORD                        lockedPid = 0;
    std::string                  lockedExe;   // 锁定主窗口的进程名（小写），CSP 家族归一用
    // —— 窗口捕获模式会话状态（captureMode==1 时有效）——
    bool                         sessionWindowMode = false;
    // 会话绑定时的窗口原生尺寸（= 帧池尺寸）。窗口 resize 后 WGC 把新内容
    // 缩放到该固定尺寸交付，故纹理尺寸不可作切分依据；改用 item.Size() 轮询
    // 与 frame.ContentSize()（二者都反映真实原生尺寸，probe_pool 已实测）。
    int                          sessionNativeW = 0, sessionNativeH = 0;
    // 尺寸变动防抖：记录候选新尺寸首次出现时刻，稳定满 kSizeStableUs 才切分。
    int64_t                      sizeWatchUs = 0;
    int                          sizeWatchW = 0, sizeWatchH = 0;
    static constexpr int64_t     kSizeStableUs = 300000;   // 300ms

    // —— MF Sink Writer（惰性创建：首个可写帧到达后才建）——
    com_ptr<IMFSinkWriter>       writer;

    // —— CPU 拷贝（消除零拷贝竞争导致的黑条）——
    // 此前用 MFCreateDXGISurfaceBuffer 零拷贝把帧池纹理直接交给编码器，
    // 而 WriteSample 是异步入队：编码器还在读那块 GPU 表面时，
    // 帧池（只有 2 个缓冲）已经把它复用给下一帧覆写 → 撕裂/黑边。
    // 暂停时帧被高速跳过、恢复写入的瞬间最容易撞上，因此表现为
    // 「每一笔暂停时闪一下黑条」（实测 1138 帧中仅 2~3 帧、且集中在
    // 画面底部与最右边缘，正是竞争与行距错位的样子）。
    // 改为同步拷到 CPU 紧凑缓冲后立即归还帧，编码器只读我们自己的内存。
    com_ptr<ID3D11Texture2D>     staging;      // 可映射的暂存纹理（复用一个，尺寸变才重建）
    int                          stgW = 0, stgH = 0;
    // 分片 MP4（fragmented MP4）所需的字节流与 sink。
    // 用分片容器后，文件头就写好初始化信息（moov/init segment），
    // 录制过程中即可播放、进程被强杀也不会得到无索引的坏文件。
    com_ptr<IMFByteStream>       byteStream;
    com_ptr<IMFMediaSink>        sink;
    DWORD                        streamIndex = 0;
    int                          encW = 0, encH = 0;     // 写入器实际使用的尺寸
    LONGLONG                     frameDur = 333333;      // 100ns 单位
    long long                    writtenFrames = 0;
    std::wstring                 curFileW;
    std::wstring                 pendingTitle;           // 开录时记下，建写入器时用
    std::wstring                 sessionDoc;             // 当前会话绑定的绘画文档名

    // —— 活跃判定 ——
    // actUs / strokeActive 由采集层实时更新（采样级），lastOpUs 只在操作完成时更新。
    // 判活跃必须优先看前两者，否则长运笔途中会被误判为空闲。
    int64_t                      lastOpUs = 0;
    int64_t                      actUs = 0;
    bool                         strokeActive = false;
    int64_t                      lastWrittenUs = 0;
    // 无输入活动多久后停止写帧（视频里因此不含停顿）。
    // 固定内部常量，不再作为用户可配参数：会话本身不因空闲而结束，
    // 空闲只影响「这一帧写不写」，所以这个窗口只需覆盖正常的操作间隙即可。
    static constexpr int64_t     kActiveWindowUs = 1200000;   // 1.2s
    int64_t                      captureStartUs = 0;

    // —— 熔断 ——
    int                          consecutiveEmptyClips = 0;
    bool                         tripped = false;
    int64_t                      trippedUs = 0;   // 熔断置位时刻，用于自动恢复
    int                          writeFailCount = 0;
    // 熔断后静默多久自动恢复重试。
    // 为什么必须可恢复：窗口模式下拖动窗口会连续产生 0 帧片段，若熔断永不
    // 复位（旧行为），一次拖动就能凑满 3 次而永久停录、只能重启程序。
    static constexpr int64_t     kTripRecoverUs = 5000000;   // 5s

    void SetError(const char* fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
        CsLock lk(cs);
        stats.lastError = buf;
        LogF("[rec] %s", buf);
    }

    // ------------------------------------------------------------
    //  文件路径：videos\<文档名>\<文档名>_<会话起始时间戳>.mp4
    //  一个绘画文档对应一个缓存文件夹（= 界面上的一张卡片），
    //  同一文档的多次录制会话都落在该文件夹内，便于「导出时拼接」。
    // ------------------------------------------------------------
    std::wstring BuildClipPath(const std::wstring& doc, int64_t startUs) {
        std::wstring clean = SanitizeFileName(doc, 60);
        std::wstring folder = DocFolderPath(clean);      // 不存在则创建
        std::string ts = UsToCompact(startUs);
        std::wstring base = clean + L"_" + U2W(ts);
        std::wstring path = folder + L"\\" + base + L".mp4";
        // 极少数情况下（同名文件已存在，例如上次运行留下的）追加序号避免覆盖
        for (int i = 2; i < 1000; ++i) {
            DWORD attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) break;
            wchar_t n[16];
            swprintf_s(n, L"_%d", i);
            path = folder + L"\\" + base + n + L".mp4";
        }
        return path;
    }

    // ------------------------------------------------------------
    //  设备初始化（进程内一次）
    // ------------------------------------------------------------
    bool EnsureDevice() {
        if (wgcDevice) return true;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       fl, (UINT)std::size(fl), D3D11_SDK_VERSION,
                                       d3dDevice.put(), &got, d3dCtx.put());
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   fl, (UINT)std::size(fl), D3D11_SDK_VERSION,
                                   d3dDevice.put(), &got, d3dCtx.put());
            if (FAILED(hr)) { SetError("D3D11CreateDevice 失败 0x%08lX", (unsigned long)hr); return false; }
            LogF("[rec] 使用 WARP 软件设备（硬件 D3D 不可用）");
        }
        com_ptr<IDXGIDevice> dxgi;
        try { dxgi = d3dDevice.as<IDXGIDevice>(); } catch (...) { dxgi = nullptr; }
        if (!dxgi) { SetError("查询 IDXGIDevice 失败"); return false; }
        // C 风格 API，出参必须是 COM 版 ::IInspectable**
        com_ptr<::IInspectable> insp;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
        if (FAILED(hr) || !insp) { SetError("CreateDirect3D11DeviceFromDXGIDevice 失败 0x%08lX", (unsigned long)hr); return false; }
        wgcDevice = insp.as<IDirect3DDevice>();
        if (!wgcDevice) { SetError("IDirect3DDevice 转换失败"); return false; }
        return true;
    }

    // ------------------------------------------------------------
    //  开启会话：捕获**整个主显示器**（不再捕获单个窗口）
    //
    //  为什么改为整屏：
    //    · 窗口捕获要处理「窗口尺寸变化 → 结束会话 → 按新尺寸重开」，
    //      而绘画软件在缩放画布、展开面板、切换 DPI 时尺寸频繁变化，
    //      导致录屏被反复切断（用户报告的「录制窗口出现很多问题」）。
    //    · 窗口被遮挡或合成异常时，WGC 可能交付带黑区的帧 ——
    //      实测缓存视频里确实有零星的底部黑行（1138 帧中 2~3 帧），
    //      整屏捕获不存在遮挡概念，从根上消除这一类黑条。
    //    · 整屏尺寸只在改分辨率时才变，会话可以长时间稳定运行。
    //  文件名仍按「绘画文档」组织（取前台绘画窗口标题），
    //  所以「一个绘画文件 = 一张卡片」的产品语义不变。
    // ------------------------------------------------------------
    bool StartSession(HWND hwnd, const std::wstring& title) {
        if (capturing) return true;
        if (!EnsureDevice()) return false;

        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem newItem{ nullptr };
        HRESULT hr;
        bool windowMode = (Cfg().captureMode == 1) && hwnd && IsWindow(hwnd);

        if (windowMode) {
            // —— 窗口捕获：直接捕获锁定的绘画主窗口 ——
            // 画面只有该窗口，不含桌面其他内容。代价（界面文案已告知用户）：
            //   · 窗口被别的窗口压住时会录到黑块（WGC 窗口捕获的硬限制）
            //   · PS 液化等模态全屏窗口期间主窗口被完全遮挡 → 录不到画面
            //   · 跨显示器拖动会触发 DPI 变化导致尺寸抖动（由 Loop 的防抖切分兜住）
            hr = interop->CreateForWindow(hwnd, guid_of<IGraphicsCaptureItem>(),
                                          winrt::put_abi(newItem));
            if (FAILED(hr) || !newItem) {
                SetError("CreateForWindow 失败 0x%08lX，回退整屏捕获", (unsigned long)hr);
                LogF("[rec] CreateForWindow 失败 0x%08lX，本次会话回退整屏捕获", (unsigned long)hr);
                windowMode = false;   // 回退到整屏
            }
        }

        if (!windowMode) {
            // —— 整屏捕获：取主窗口所在显示器；拿不到就退回主显示器 ——
            HMONITOR mon = nullptr;
            if (hwnd && IsWindow(hwnd)) mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!mon) mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            if (!mon) { SetError("无法确定要捕获的显示器"); return false; }
            hr = interop->CreateForMonitor(mon, guid_of<IGraphicsCaptureItem>(),
                                           winrt::put_abi(newItem));
            if (FAILED(hr) || !newItem) {
                SetError("CreateForMonitor 失败 0x%08lX", (unsigned long)hr);
                return false;
            }
        }
        item = newItem;

        auto sz = item.Size();
        int trueW = (int)sz.Width, trueH = (int)sz.Height;
        if (trueW < 16 || trueH < 16) {
            SetError("%s尺寸过小 %dx%d", windowMode ? "窗口" : "显示器", trueW, trueH);
            item = nullptr; return false;
        }

        // 帧池按偶数尺寸建：H264 要求偶数，且写入器尺寸将从真实纹理取得，二者必然一致
        SizeInt32 poolSz{ trueW & ~1, trueH & ~1 };
        if (poolSz.Width < 16 || poolSz.Height < 16) { item = nullptr; return false; }

        // 2 个缓冲：够平滑且省显存（低资源占用）
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            wgcDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSz);
        session = pool.CreateCaptureSession(item);
        try { session.IsBorderRequired(false); } catch (...) {}   // 关闭黄色边框

        // 记录本会话的捕获模式与原生尺寸：窗口模式下 Loop 靠轮询 item.Size()
        // 与它比对来发现 resize，进而防抖切分。整屏模式尺寸恒定，不会触发切分。
        sessionWindowMode = windowMode;
        sessionNativeW = poolSz.Width;
        sessionNativeH = poolSz.Height;
        sizeWatchUs = 0; sizeWatchW = 0; sizeWatchH = 0;

        capturedHwnd = hwnd;
        pendingTitle = title;
        // 用与 Loop 相同的 ResolveDocName：两处口径必须一致，否则 CSP 家族场景下
        // curDoc 与 sessionDoc 会不同，docChanged 恒真导致反复切会话产生碎片。
        sessionDoc = ResolveDocName(hwnd, title);
        captureStartUs = Clock::NowUs();
        writtenFrames = 0;
        lastWrittenUs = 0;
        writeFailCount = 0;

        {   // 生命周期锁：置位 capturing 与注册回调必须原子，避免帧回调读到半初始化状态
            CsLock wlk(writerCs);
            frameToken = pool.FrameArrived({ this, &Impl::OnFrame });
            // Closed 事件定义在 GraphicsCaptureItem 上，不在 session 上
            closedToken = item.Closed({ this, &Impl::OnItemClosed });
            capturing = true;
            session.StartCapture();
        }

        {
            CsLock lk(cs);
            stats.capturing = true;
            stats.writing = false;
            stats.clipsCreated++;
            stats.width = poolSz.Width;
            stats.height = poolSz.Height;
            stats.clipSeconds = 0.0;
            stats.sessionDoc = W2U(sessionDoc);
            stats.currentFile.clear();   // 文件名在建写入器时才确定
        }
        LogF("[rec] session started %dx%d (pool) doc=%s",
             poolSz.Width, poolSz.Height, W2U(sessionDoc).c_str());
        return true;
    }

    // ------------------------------------------------------------
    //  惰性创建写入器：尺寸取自真实纹理，属性齐全
    // ------------------------------------------------------------
    bool InitWriter(int texW, int texH) {
        // H264 要求偶数
        int w = texW & ~1, h = texH & ~1;
        if (w < 16 || h < 16) { SetError("纹理尺寸无效 %dx%d", texW, texH); return false; }

        // 一个绘画文档 = 一个视频文件：文件名取会话绑定的文档名与会话起始时刻，
        // 因此同一文档在整个会话期间始终写同一个文件，不会每笔一个新文件。
        curFileW = BuildClipPath(sessionDoc, captureStartUs);

        int fps = Cfg().videoFps;
        if (fps < 1 || fps > 120) fps = 30;
        UINT bitrate = (UINT)Cfg().videoBitrateKbps * 1000u;
        if (bitrate < 200000u) bitrate = 200000u;
        frameDur = 10000000LL / fps;

        com_ptr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.put(), 2))) { SetError("MFCreateAttributes 失败"); return false; }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);
        // 保留默认节流：编码器积压时 WriteSample 自然回压，避免帧堆积吃内存

        UINT64 frameSize = ((UINT64)(UINT32)w << 32) | (UINT32)h;
        UINT64 frameRate = ((UINT64)(UINT32)fps << 32) | 1;

        // —— 输出类型（H264）——
        com_ptr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(outType.put()))) { SetError("MFCreateMediaType(out) 失败"); return false; }
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        outType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        outType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        // 关键帧间隔 = 1 秒（毫秒单位），与下面的分片时长对齐。
        // 解码必须从关键帧起，所以「中断损失」的上界由两者中较大者决定；
        // 只缩短分片而不管关键帧，实测仍会丢掉将近一个 GOP（约 0.8 秒）。
        // 代价是文件略大（关键帧更密），换来崩溃时最多只丢约 1 秒。
        outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, 1000);

        // ============================================================
        //  容器选择：分片 MP4 优先，失败回退普通 MP4
        //
        //  为什么必须是分片 MP4：普通 MP4 的索引（moov）在 Finalize() 时
        //  才写到文件尾。录制中若进程被强杀/断电，文件就没有索引，
        //  任何播放器都打不开 —— 用户表现为「关闭软件后才能读出来」，
        //  且这类残缺文件会让导出与缩略图都失败。
        //  分片 MP4 把初始化信息写在文件头、之后按片段追加，
        //  因此**随时中断都保留已写内容、随时都能打开播放**。
        // ============================================================
        com_ptr<IMFSinkWriter> w2;
        bool fragmented = false;
        {
            com_ptr<IMFByteStream> bs;
            // 必须用 DELETE_IF_EXIST：实测（tests/mfpath_probe.cpp）
            // MF_OPENMODE_RESET_IF_EXIST 不会创建文件，文件不存在时直接返回
            // 0x80070002，会让分片路径每次都失败并静默回退成普通 MP4。
            // DELETE_IF_EXIST 语义正是「已存在则删掉重建」，符合新建录制的意图。
            HRESULT hrBs = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST,
                                        MF_FILEFLAGS_NONE, curFileW.c_str(), bs.put());
            if (SUCCEEDED(hrBs) && bs) {
                com_ptr<IMFAttributes> sinkAttr;
                if (SUCCEEDED(MFCreateAttributes(sinkAttr.put(), 2)) && sinkAttr) {
                    // moov 写在 mdat 之前 → 未收尾也能被播放器识别
                    HRESULT hrA = sinkAttr->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, TRUE);
                    // 每 1 秒一个片段，与上面的关键帧间隔对齐：
                    // 片段越短，中断时丢的内容越少；太长则文件头之外
                    // 迟迟没有可播数据。1s 是流畅性与开销的折中。
                    HRESULT hrB = sinkAttr->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, 10000000ULL);
                    if (FAILED(hrA) || FAILED(hrB))
                        LogF("[rec] 分片属性设置失败 moov=0x%08lX frag=0x%08lX（继续，用 sink 默认值）",
                             (unsigned long)hrA, (unsigned long)hrB);
                }
                com_ptr<IMFMediaSink> fSink;
                HRESULT hrSink = MFCreateFMPEG4MediaSink(bs.get(), outType.get(), nullptr, fSink.put());
                if (SUCCEEDED(hrSink) && fSink) {
                    HRESULT hrW = MFCreateSinkWriterFromMediaSink(fSink.get(), attrs.get(), w2.put());
                    if (SUCCEEDED(hrW) && w2) {
                        byteStream = bs;      // 持有引用，Finalize 后再释放
                        sink = fSink;
                        fragmented = true;
                    }
                }
            }
        }
        if (!fragmented) {
            // 回退：按 URL 建普通 MP4 写入器（行为与改造前一致）
            LogF("[rec] 分片 MP4 不可用，回退为普通 MP4");
            HRESULT hr = MFCreateSinkWriterFromURL(curFileW.c_str(), nullptr, attrs.get(), w2.put());
            if (FAILED(hr) || !w2) {
                SetError("MFCreateSinkWriterFromURL 失败 0x%08lX", (unsigned long)hr);
                return false;
            }
        }

        // 流索引：两条路径不同。
        //   · 分片路径：MFCreateFMPEG4MediaSink 传入 outType 时已经建好该流，
        //     再调 AddStream 会多出一条没有任何样本的空流，Finalize 时报
        //     MF_E_SINK_HEADERS_NOT_FOUND(0xC00D4A45)。故直接用 index 0。
        //   · 回退路径：MFCreateSinkWriterFromURL 建的写入器还没有流，需要 AddStream。
        DWORD idx = 0;
        if (!fragmented) {
            HRESULT hrAdd = w2->AddStream(outType.get(), &idx);
            if (FAILED(hrAdd)) { SetError("AddStream(H264) 失败 0x%08lX", (unsigned long)hrAdd); return false; }
        }

        // —— 输入类型（BGRA8 = MFVideoFormat_RGB32）——
        // 关键：非压缩输入必须给 SAMPLE_SIZE / DEFAULT_STRIDE / FIXED_SIZE_SAMPLES，
        // 缺失会导致 WriteSample 每帧返回 E_INVALIDARG(0x80070057)。
        com_ptr<IMFMediaType> inType;
        if (FAILED(MFCreateMediaType(inType.put()))) { SetError("MFCreateMediaType(in) 失败"); return false; }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        inType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        inType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        inType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 1);
        inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
        inType->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)w * (UINT32)h * 4u);
        // 正数 stride = top-down，BGRA32 每像素 4 字节
        inType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));

        HRESULT hr = w2->SetInputMediaType(idx, inType.get(), nullptr);
        if (FAILED(hr)) { SetError("SetInputMediaType(RGB32) 失败 0x%08lX", (unsigned long)hr); return false; }

        hr = w2->BeginWriting();
        if (FAILED(hr)) { SetError("BeginWriting 失败 0x%08lX", (unsigned long)hr); return false; }

        writer = w2;
        streamIndex = idx;
        encW = w; encH = h;
        writtenFrames = 0;

        {
            CsLock lk(cs);
            stats.width = w; stats.height = h;
            stats.writing = true;
            stats.currentFile = W2U(curFileW);
        }
        LogF("[rec] writer ready %dx%d @%dfps -> %s", w, h, fps, W2U(curFileW).c_str());
        return true;
    }

    // ------------------------------------------------------------
    //  会话收尾：结束当前绘画文档的录制并关闭视频文件
    //  触发条件只有：换到另一个文档 / 关闭录制 / 捕获项关闭 /
    //  窗口尺寸变化 / 退出程序。「停顿、空闲」不再触发收尾。
    // ------------------------------------------------------------
    void EndSession(const char* reason) {
        if (!capturing) return;

        double videoSec = (double)writtenFrames * (double)frameDur / 1e7;
        std::wstring fileW;
        std::wstring docW;
        long long frames = writtenFrames;

        {   // 生命周期锁：writer 的收尾与会话拆除必须与 OnFrame 互斥
            CsLock wlk(writerCs);
            if (!capturing) return;
            capturing = false;
            frames = writtenFrames;
            videoSec = (double)writtenFrames * (double)frameDur / 1e7;
            fileW = curFileW;
            docW = sessionDoc;
            if (writer) {
                // 0 帧时不能 Finalize：无任何样本会返回 0xC00D4A44
                if (frames > 0) {
                    HRESULT hr = writer->Finalize();
                    if (FAILED(hr)) LogF("[rec] Finalize 失败 0x%08lX", (unsigned long)hr);
                } else {
                    LogF("[rec] 0 帧，跳过 Finalize");
                }
                writer = nullptr;
            }
            // 释放顺序必须 writer → sink → byteStream：
            // 字节流最后释放，文件句柄才会真正关闭、缓冲内容落盘。
            if (sink) { try { sink->Shutdown(); } catch (...) {} sink = nullptr; }
            if (byteStream) {
                try { byteStream->Flush(); } catch (...) {}
                byteStream = nullptr;
            }
            TeardownCapture();
            curFileW.clear();
            sessionDoc.clear();
            writtenFrames = 0;
        }

        {   // 统计更新走轻量锁
            CsLock lk(cs);
            stats.capturing = false;
            stats.writing = false;
            stats.clipSeconds = videoSec;
            stats.currentFile.clear();
            stats.sessionDoc.clear();
        }

        // 丢弃空/过短文件（本程序刚生成、从未被用户使用的临时输出）
        int64_t minMs = (int64_t)Cfg().minClipMs;
        if (frames == 0 || videoSec * 1000.0 < (double)minMs) {
            if (!fileW.empty()) {
                // 刚释放的字节流可能还被系统短暂持有，首次 DeleteFileW 会失败；
                // 不重试就会留下只有文件头、无索引的残缺 mp4（几十~几百字节）。
                // 这类文件播放器打不开、导出时又会被当成第一段而报「无法确定视频分辨率」。
                bool deleted = false;
                for (int attempt = 0; attempt < 5 && !deleted; ++attempt) {
                    if (attempt > 0) Sleep(60);
                    deleted = DeleteFileW(fileW.c_str()) != FALSE;
                }
                if (!deleted)
                    LogF("[rec] 删除过短文件失败（err=%lu），残留：%s",
                         GetLastError(), W2U(fileW).c_str());
            }
            if (frames == 0) {
                if (++consecutiveEmptyClips >= 3 && !tripped) {
                    tripped = true;
                    trippedUs = Clock::NowUs();
                    LogF("[rec] 连续 %d 个空文件，已熔断自动录制（%lld 秒后自动重试）。详见上方错误。",
                         consecutiveEmptyClips, (long long)(kTripRecoverUs / 1000000));
                    CsLock lk(cs);
                    stats.lastError = "录屏连续空文件已熔断，稍后自动重试";
                }
            }
            LogF("[rec] file discarded (%s), %lld frames, %.2fs",
                 reason ? reason : "?", frames, videoSec);
        } else {
            consecutiveEmptyClips = 0;
            CsLock lk(cs);
            stats.recentFiles.push_back(W2U(fileW));
            if (stats.recentFiles.size() > 20) stats.recentFiles.erase(stats.recentFiles.begin());
            LogF("[rec] file saved (%s) doc=%s %lld frames, %.2fs -> %s",
                 reason ? reason : "?", W2U(docW).c_str(),
                 frames, videoSec, W2U(fileW).c_str());
        }
    }

    void TeardownCapture() {
        if (pool && frameToken)  { try { pool.FrameArrived(frameToken); } catch (...) {} }
        if (item && closedToken) { try { item.Closed(closedToken); } catch (...) {} }
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        session = nullptr; pool = nullptr; item = nullptr;
        frameToken = {}; closedToken = {};
        capturedHwnd = nullptr;
        // 暂存纹理只在会话内使用；停止录制就归还，避免不录屏时白占显存
        staging = nullptr; stgW = 0; stgH = 0;
    }

    // ------------------------------------------------------------
    //  帧到达
    // ------------------------------------------------------------
    void OnFrame(Direct3D11CaptureFramePool const&, WinInspectable const&) {
        if (!capturing || !pool) return;

        Direct3D11CaptureFrame frame{ nullptr };
        try { frame = pool.TryGetNextFrame(); } catch (...) { return; }
        if (!frame) return;

        int64_t nowUs = Clock::NowUs();
        {
            CsLock lk(cs);
            stats.framesCaptured++;
        }

        // —— 是否「正在操作」：决定这一帧写不写 ——
        // 活跃判定优先看「采样级时刻 actUs」与「笔画进行中 strokeActive」，
        // 而不是「已完成操作时刻 lastOpUs」。一次连续运笔可持续数秒，
        // 期间不产生任何完成的操作；若只看 lastOpUs，长运笔的帧会被
        // 当作空闲全部丢弃，画面就"被切断"了（实测被切在 1.20s）。
        // 三个信号任一新鲜即视为活跃：笔画进行中 / 刚有采样 / 刚完成操作。
        bool active = strokeActive
                   || (actUs != 0 && (nowUs - actUs) <= kActiveWindowUs)
                   || (lastOpUs != 0 && (nowUs - lastOpUs) <= kActiveWindowUs);
        // 无活动：直接丢弃，不进编码器（视频里因此没有停顿，且零编码开销）
        if (!active) {
            CsLock lk(cs);
            stats.framesSkipped++;
            return;
        }

        // —— 取真实纹理（尺寸以此为准）——
        auto surface = frame.Surface();
        if (!surface) return;
        IUnknown* unkSurface = winrt::get_unknown(surface);
        if (!unkSurface) return;

        DxgiInterfaceAccess* accessRaw = nullptr;
        if (FAILED(unkSurface->QueryInterface(IID_PPV_ARGS(&accessRaw))) || !accessRaw) return;
        com_ptr<DxgiInterfaceAccess> access;
        access.attach(accessRaw);

        com_ptr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.put()))) || !tex) return;

        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        int texW = (int)desc.Width, texH = (int)desc.Height;

        // 生命周期锁：与 EndSession/TeardownCapture 互斥，避免边写边拆会话
        CsLock wlk(writerCs);
        if (!capturing) return;

        // 尺寸变化：结束片段，下一轮按新尺寸重开
        if (writer && (texW != encW || texH != encH)) {
            LogF("[rec] size changed %dx%d -> %dx%d", encW, encH, texW, texH);
            InterlockedExchange(&finalizeReq, 1);
            return;
        }

        // 帧率限流：距上次写入不足一帧时长则丢弃
        if (writer && lastWrittenUs && (nowUs - lastWrittenUs) < (frameDur / 10)) {
            CsLock lk(cs);
            stats.framesSkipped++;
            return;
        }

        // 惰性创建写入器：此刻才知道真实尺寸
        if (!writer) {
            if (!InitWriter(texW, texH)) {
                InterlockedExchange(&finalizeReq, 1);
                return;
            }
        }

        // ============================================================
        //  同步拷贝到 CPU 紧凑缓冲（取代零拷贝）
        //
        //  为什么必须拷贝：WriteSample 是异步入队的。零拷贝时把帧池纹理
        //  直接交给编码器，而帧池只有 2 个缓冲 —— 编码器还在读那块 GPU
        //  表面，WGC 就把它复用给下一帧覆写了，画面出现撕裂与黑边。
        //  「暂停 → 恢复写入」的瞬间最容易撞上（暂停期间帧被高速跳过、
        //  缓冲轮转最快），所以表现为**每一笔暂停时闪一下黑条**：
        //  实测 1138 帧里只有 2~3 帧异常，且集中在画面底部与最右边缘。
        //  拷贝到自己拥有的内存后，编码器读什么与帧池何时覆写彻底解耦。
        // ============================================================
        if (!staging || stgW != texW || stgH != texH) {
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            // 暂存纹理必须是单采样，否则不可 Map；
            // 源若是 MSAA，下面用 ResolveSubresource 归一化后再读。
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            com_ptr<ID3D11Texture2D> st;
            HRESULT hrS = d3dDevice->CreateTexture2D(&sd, nullptr, st.put());
            if (FAILED(hrS) || !st) {
                LogF("[rec] 创建暂存纹理失败 0x%08lX (%dx%d)", (unsigned long)hrS, texW, texH);
                InterlockedExchange(&finalizeReq, 1);
                return;
            }
            staging = st; stgW = texW; stgH = texH;
        }
        if (desc.SampleDesc.Count > 1) {
            // MSAA 源：CopyResource 会失败，必须解析多重采样
            d3dCtx->ResolveSubresource(staging.get(), 0, tex.get(), 0, desc.Format);
        } else {
            d3dCtx->CopyResource(staging.get(), tex.get());
        }

        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hrMap = d3dCtx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hrMap)) {
            if (++writeFailCount <= 3)
                LogF("[rec] Map 暂存纹理失败 0x%08lX", (unsigned long)hrMap);
            InterlockedExchange(&finalizeReq, 1);
            return;   // frame 析构时自动归还帧池，无需显式 Close
        }

        const int cw = encW, ch = encH;
        const size_t dstPitch = (size_t)cw * 4u;
        const size_t need = dstPitch * (size_t)ch;

        com_ptr<IMFMediaBuffer> buf;
        HRESULT hrb = MFCreateMemoryBuffer((DWORD)need, buf.put());
        if (FAILED(hrb) || !buf) {
            LogF("[rec] MFCreateMemoryBuffer 失败 0x%08lX", (unsigned long)hrb);
            d3dCtx->Unmap(staging.get(), 0);
            InterlockedExchange(&finalizeReq, 1);
            return;
        }

        {
            BYTE* dstRaw = nullptr; DWORD dstMax = 0, dstCur = 0;
            if (FAILED(buf->Lock(&dstRaw, &dstMax, &dstCur)) || !dstRaw || dstMax < need) {
                d3dCtx->Unmap(staging.get(), 0);
                InterlockedExchange(&finalizeReq, 1);
                return;
            }
            // 按**真实 RowPitch** 逐行搬：假设 pitch == w*4 是错的。
            // D3D11 的行距由驱动决定，常按 16/256 字节对齐，而
            // 1726*4=6904 两者都不满足；一旦有填充，编码器按 w*4 解读
            // 就会逐行错位累积，形成黑色条状。
            const size_t srcPitch = (size_t)map.RowPitch;
            const uint8_t* s = (const uint8_t*)map.pData;
            // 防御：D3D11 保证 RowPitch >= 宽度*4，但两者来自不同来源，
            // 万一不成立就会读越界，宁可丢帧也不要踩内存。
            if (srcPitch >= dstPitch) {
                for (int y = 0; y < ch; ++y) {
                    const uint8_t* srow = s + (size_t)y * srcPitch;
                    uint8_t* drow = dstRaw + (size_t)y * dstPitch;
                    memcpy(drow, srow, dstPitch);
                    // alpha 强制不透明：捕获表面部分区域 A=0，
                    // 某些编码器/播放器会当成透明 → 黑。
                    for (int x = 3; x < cw * 4; x += 4) drow[x] = 0xFF;
                }
            } else {
                LogF("[rec] 行距异常 srcPitch=%zu < dstPitch=%zu，丢弃该帧", srcPitch, dstPitch);
            }
            buf->Unlock();
            if (srcPitch >= dstPitch) buf->SetCurrentLength((DWORD)need);
            else { d3dCtx->Unmap(staging.get(), 0); return; }
        }
        d3dCtx->Unmap(staging.get(), 0);
        // 已拷到自己内存，尽早归还帧给帧池。
        // Close() 可能抛异常，而这里运行在帧回调线程上，必须包住。
        try { frame.Close(); } catch (...) {}

        com_ptr<IMFSample> sample;
        if (FAILED(MFCreateSample(sample.put())) || !sample) {
            InterlockedExchange(&finalizeReq, 1);
            return;
        }
        if (FAILED(sample->AddBuffer(buf.get()))) {
            InterlockedExchange(&finalizeReq, 1);
            return;
        }

        // 时间戳按「已写入帧序号」连续排布 → 播放时看不到被跳过的空闲
        LONGLONG t = writtenFrames * frameDur;
        sample->SetSampleTime(t);
        sample->SetSampleDuration(frameDur);

        HRESULT hr = writer->WriteSample(streamIndex, sample.get());
        if (FAILED(hr)) {
            if (++writeFailCount <= 3)
                LogF("[rec] WriteSample 失败 0x%08lX (第 %d 次, %dx%d)",
                     (unsigned long)hr, writeFailCount, encW, encH);
            InterlockedExchange(&finalizeReq, 1);
            return;
        }

        writtenFrames++;
        lastWrittenUs = nowUs;
        {
            CsLock lk(cs);
            stats.framesWritten++;
            stats.clipSeconds = (double)writtenFrames * (double)frameDur / 1e7;
        }
    }

    void OnItemClosed(GraphicsCaptureItem const&, WinInspectable const&) {
        InterlockedExchange(&finalizeReq, 1);
    }

    // ------------------------------------------------------------
    //  解析并锁定「绘画主窗口」
    //
    //  输入：当前前台窗口句柄（可能主窗口，也可能是浮窗/面板/液化）。
    //  输出：outMain  = 应当用于文档名与窗口捕获的主窗口句柄；
    //        outTitle = 该主窗口的标题（文档名的唯一可信来源）。
    //  返回 false 表示当前无法确定主窗口（调用方应冻结，不新建卡片）。
    //
    //  规则：
    //   · 若已锁定且锁定的主窗口仍存活、且与前台同进程 → 沿用锁定值，
    //     不重新按面积挑（否则 PS 液化这种全屏模态窗口面积更大，会顶掉真主窗口）。
    //   · 否则在同进程内找最大可见顶级窗口作为新主窗口并锁定。
    //   · 文档名一律取「主窗口标题」，绝不取前台浮窗标题 —— 这同时消除了
    //     空标题退化成 exe 名、浮窗名（色轮/液化）被当文档名两类问题。
    // ------------------------------------------------------------
    bool ResolveAndLockMain(HWND fgHwnd, HWND& outMain, std::wstring& outTitle) {
        DWORD pid = PidOfHwnd(fgHwnd);
        if (!pid) { outMain = nullptr; outTitle.clear(); return false; }

        // 已锁定且仍有效（存活 + 同进程）→ 沿用，避免液化/面板把主窗口顶掉
        bool reuse = lockedMainHwnd
                  && IsWindow(lockedMainHwnd)
                  && lockedPid == pid;
        HWND main = nullptr;
        if (reuse) {
            main = lockedMainHwnd;
        } else {
            main = FindMainWindow(pid);
            if (!main) main = fgHwnd;   // 兜底：找不到就退回前台窗口本身
            lockedMainHwnd = main;
            lockedPid = pid;
            lockedExe = ToLowerA(GetProcessExeName(pid));
        }

        if (!main || !IsWindow(main)) { outMain = nullptr; outTitle.clear(); return false; }

        outMain = main;
        outTitle = ReadWndTitle(main);   // 主窗口标题（可能为空，交由 IsPlausibleDocName 判定）
        return true;
    }

    // ------------------------------------------------------------
    //  主循环
    // ------------------------------------------------------------
    void Loop() {
        runFlag = 1;
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); } catch (...) {}
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

        LogF("[rec] thread started");
        while (runFlag) {
            Sleep(120);
            ActivityInfo ai;
            if (src) ai = src();

            int64_t nowUs = Clock::NowUs();
            {
                CsLock lk(cs);
                lastOpUs     = ai.lastOpUs;
                actUs        = ai.actUs;
                strokeActive = ai.strokeActive;
                stats.running = true;
            }

            // 熔断自动恢复：静默足够久后清除熔断，允许重新尝试录制。
            // 恢复后重置计数，从零重新判定，避免旧的连续空片段计数立刻再次触发。
            if (tripped && trippedUs && (nowUs - trippedUs) >= kTripRecoverUs) {
                tripped = false;
                trippedUs = 0;
                consecutiveEmptyClips = 0;
                LogF("[rec] 熔断已自动恢复，重新允许录制");
                CsLock lk(cs);
                stats.lastError.clear();
            }

            // 是否「正在操作」：笔画进行中，或最近有输入采样/完成操作。
            // 用采样级信号（actUs / strokeActive），长运笔途中也算活跃。
            bool active = ai.strokeActive
                || (ai.actUs > 0 && (nowUs - ai.actUs) <= kActiveWindowUs)
                || (ai.lastOpUs > 0 && (nowUs - ai.lastOpUs) <= kActiveWindowUs);

            // 收尾请求（捕获项关闭 / 窗口尺寸变化 / 手动保存）
            if (InterlockedCompareExchange(&finalizeReq, 0, 1) == 1)
                EndSession("finalize-request");

            // ------------------------------------------------------------
            //  解析「绘画主窗口」与其文档名
            //
            //  文档名一律取**主窗口标题**，不取前台窗口标题：前台可能是分离
            //  浮窗（CSP 色轮/工具属性）、模态窗口（PS 液化）或伪窗口，用它们
            //  的标题会切出垃圾卡片。主窗口一旦锁定，浮窗抢焦点也不会影响它。
            //  这一步同时修复了两个历史缺陷：
            //   · 空标题退化成 exe 名当文档名（主窗口标题非空）
            //   · PS 标签页切文档时 hwnd 不变、只有标题变，旧逻辑读不到新标题
            //     （这里每轮对锁定的主窗口重读标题，切换能被正确识别）
            // ------------------------------------------------------------
            HWND mainHwnd = nullptr;
            std::wstring mainTitle;
            bool haveMain = false;
            if (ai.targetActive && ai.fgHwnd)
                haveMain = ResolveAndLockMain((HWND)ai.fgHwnd, mainHwnd, mainTitle);

            // 文档名：解析后还需通过形态校验，不合格（浮窗名/括号不配对/含.exe/
            // 非法字符）则视为「无有效文档名」，冻结当前归属而不新建卡片。
            std::wstring curDoc;
            bool curDocValid = false;
            if (haveMain && mainHwnd) {
                std::wstring d = ResolveDocName(mainHwnd, mainTitle);
                if (Recorder::IsPlausibleDocName(d)) { curDoc = d; curDocValid = true; }
            }

            if (!capturing) {
                // 绘画软件在前台 + 正在操作 + 拿到了有效主窗口文档名 → 开会话。
                // 捕获对象一律用锁定的主窗口（整屏模式据其定位显示器，窗口模式直接捕获它）。
                if (active && !tripped && Cfg().recordVideo && curDocValid && mainHwnd)
                    StartSession(mainHwnd, mainTitle);
            } else {
                // 会话结束的仅有条件：
                //   · 关闭录制（含界面上的录屏开关）
                //   · 换到「另一个有效绘画文档」（主窗口标题变了，且新文档名校验通过）
                //   · 显示器分辨率变化 / 捕获项关闭 / 窗口尺寸变化（由 finalizeReq 触发）
                //
                // 明确【不】结束会话的情况：
                //   · 停顿、空闲（帧在 OnFrame 里被丢弃，视频里看不到等待）
                //   · 切到别的软件再切回来（前台不是绘画软件时 actUs 不再刷新，
                //     帧自然被跳过 = 暂停；切回来继续写同一个文件）
                //   · 浮窗/面板/液化抢焦点：主窗口锁定不变，文档名不变 → 不切
                //   · 主窗口标题暂时读到无效值（如液化标题「液化 (shijian」）：
                //     curDocValid=false → 冻结，不切，等标题恢复有效
                bool recOff = !Cfg().recordVideo;
                bool docChanged = curDocValid && curDoc != sessionDoc;
                if (recOff)          EndSession("record-disabled");
                else if (docChanged) EndSession("doc-switched");
            }

            // ------------------------------------------------------------
            //  窗口捕获模式：轮询 item.Size() 检测窗口尺寸变化，防抖后切分
            //
            //  探测（tests/probe_pool.cpp）已证明：窗口 resize 后 WGC 把新内容
            //  缩放到固定帧池尺寸交付，纹理尺寸恒定，不能用作文档切分信号；
            //  但 item.Size() 与 frame.ContentSize() 都反映真实原生尺寸。
            //  这里在录屏线程轮询 item.Size()（本就每 120ms 一轮），尺寸稳定
            //  满 300ms 才收尾重开 —— 拖动窗口过程中不切，避免瞬间产生大量
            //  0 帧片段触发熔断。
            // ------------------------------------------------------------
            if (capturing && sessionWindowMode && item) {
                try {
                    SizeInt32 s = item.Size();
                    int w = s.Width & ~1, h = s.Height & ~1;
                    if (w != sessionNativeW || h != sessionNativeH) {
                        if (sizeWatchW == w && sizeWatchH == h) {
                            // 同一候选尺寸持续；稳定够久才切分
                            if (sizeWatchUs && (nowUs - sizeWatchUs) >= kSizeStableUs) {
                                LogF("[rec] 窗口尺寸稳定 %dx%d -> %dx%d，切分为新片段",
                                     sessionNativeW, sessionNativeH, w, h);
                                sizeWatchUs = 0; sizeWatchW = 0; sizeWatchH = 0;
                                EndSession("size-split");
                                // 下一轮 !capturing 分支会用新尺寸重开，归入同一文档卡片
                            }
                        } else {
                            // 出现新的候选尺寸，重置防抖计时（拖动过程中不断刷新）
                            sizeWatchW = w; sizeWatchH = h; sizeWatchUs = nowUs;
                        }
                    } else {
                        // 尺寸回到会话原生值，清除候选
                        sizeWatchUs = 0; sizeWatchW = 0; sizeWatchH = 0;
                    }
                } catch (...) {}
            }
        }

        if (capturing) EndSession("exit");
        TeardownCapture();
        MFShutdown();
        LogF("[rec] thread stopped");
    }
};

// ============================================================
//  Recorder 外壳
// ============================================================
Recorder& Recorder::I() {
    static Recorder inst;
    return inst;
}

Recorder::Recorder() { impl_ = new Impl(); impl_->self = this; }
Recorder::~Recorder() { Stop(); delete impl_; impl_ = nullptr; }

// 从窗口标题提取「文档名」，用于录屏文件名。
//   PS  : "shijian.psd @ 11.1% (图层 199, RGB/8) *" -> "shijian"
//   CSP : "作品_线稿.csp - Clip Studio Paint"        -> "作品_线稿"
//   SAI : "涂鸦_01.sai2 - PaintTool SAI"             -> "涂鸦_01"
std::wstring Recorder::DocNameFromTitle(const std::wstring& title) {
    std::wstring t = title;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    size_t b = 0;
    while (b < t.size() && iswspace(t[b])) ++b;
    t = t.substr(b);
    if (t.empty()) return L"绘画";

    // 优先按 PS 的 " @ " 缩放标记切；否则按 " - " 应用名分隔切
    size_t cut = std::wstring::npos;
    size_t at = t.find(L" @ ");
    if (at != std::wstring::npos) {
        cut = at;
    } else {
        size_t dash = t.find(L" - ");
        if (dash != std::wstring::npos) cut = dash;
    }
    if (cut != std::wstring::npos && cut > 0) t = t.substr(0, cut);

    // 先剥离 PS 未保存标记等尾部符号，再去扩展名。
    // 顺序很关键：若先去扩展名，"shijian.psd *" 的最后一段是 "psd *"（含空格），
    // 会被判为「不像扩展名」而跳过，导致结果残留 ".psd"。
    auto stripTail = [](std::wstring& s) {
        while (!s.empty() && (s.back() == L'*' || s.back() == L'+' || iswspace(s.back())))
            s.pop_back();
    };
    stripTail(t);

    // 去掉扩展名（仅当点号在合理位置、且其后不含空格）
    size_t dot = t.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0 && dot + 1 < t.size()) {
        std::wstring ext = t.substr(dot + 1);
        if (ext.size() <= 8 && ext.find(L' ') == std::wstring::npos)
            t = t.substr(0, dot);
    }

    // 去扩展名可能又露出尾部空白，再剥一次
    stripTail(t);

    if (t.empty()) t = L"绘画";
    return t;
}

// 判断 DocNameFromTitle 的结果是否「像一个真实绘画文档名」，可用于建卡/切卡。
//
// 这是主窗口锁定之外的**安全网**：正常路径下文档名取自主窗口标题，浮窗/面板
// 名根本不会进入解析。但万一锁定失效（例如 PS 浮动文档模式，或用户在液化
// 打开期间才首次触发录制、FindMainWindow 误选了全屏液化窗口），畸形名字会被
// 这里挡下 —— 返回 false 时录屏层冻结当前归属、不新建垃圾卡片。
//
// 只做「几乎不可能误伤真实文件名」的结构性判断：
bool Recorder::IsPlausibleDocName(const std::wstring& doc) {
    if (doc.empty()) return false;

    // 纯空白
    bool allSpace = true;
    for (wchar_t c : doc) if (!iswspace(c)) { allSpace = false; break; }
    if (allSpace) return false;

    // 含 ".exe"：空标题退化成了进程名（clipstudiopaint.exe -> clipstudiopaint），
    // 或伪窗口/启动器标题泄漏。ASCII 手动小写，避免额外头文件依赖。
    {
        std::wstring lower = doc;
        for (auto& c : lower) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (lower.find(L".exe") != std::wstring::npos) return false;
    }

    // 括号不配对：典型是模态窗口标题被 DocNameFromTitle 从 " @ " 处拦腰切断的残片，
    // 如 "液化 (shijian.psd @ 26.4%)" -> "液化 (shijian"（有左括号无右括号）。
    // 真实文件名里的括号通常成对出现，故此检查误伤率极低。
    {
        int half = 0, full = 0;
        for (wchar_t c : doc) {
            if (c == L'(') ++half; else if (c == L')') --half;
            else if (c == L'（') ++full; else if (c == L'）') --full;
        }
        if (half != 0 || full != 0) return false;
    }

    // 已观测到的浮窗/面板名（主窗口锁定失效时的兜底）。刻意只列 UI 面板专有词，
    // 不列「图层/画布/发现」这类可能被用作真实文件名的词，以免误伤。
    // 注意：CSP 主窗口标题 "CLIP STUDIO PAINT" 必须放行，不在黑名单内。
    static const wchar_t* const kPanels[] = { L"色轮", L"工具属性", L"环境设置" };
    for (const wchar_t* p : kPanels) if (doc == p) return false;

    return true;
}

// 由采集层组装「当前活动状态」。录屏线程靠轮询它来判断是否在作画、
// 以及当前绘画文档是哪个（决定文件归档到哪张卡片）。
ActivityInfo Recorder::MakeActivityInfo() {
    ActivityInfo ai;
    CaptureCounters cc = Capture::I().Snapshot();
    ForegroundState fg = Capture::I().FgSnapshot();
    ai.targetActive = cc.targetActive;
    ai.lastOpUs     = cc.lastOpUs;
    // 采样级活跃信号：运笔途中持续刷新，供录屏判断「正在操作」。
    // 注意：纯移动（未按住键/笔尖未接触）不会刷新它 —— 那样录屏会把
    // 无意义的鼠标划过也当成作画，视频里混入大量停顿。
    ai.actUs        = Capture::I().ActivityUs();
    ai.strokeActive = Capture::I().StrokeActive();
    ai.fgHwnd       = (void*)fg.hwnd;
    ai.fgTitle      = U2W(fg.title);
    if (ai.fgTitle.empty()) ai.fgTitle = U2W(fg.exe);
    return ai;
}

bool Recorder::IsSupported() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    bool ok = false;
    HANDLE h = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        bool* o = (bool*)p;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            *o = GraphicsCaptureSession::IsSupported();
            winrt::uninit_apartment();
        } catch (...) { *o = false; }
        return 0;
    }, &ok, 0, nullptr);
    if (h) { WaitForSingleObject(h, 4000); CloseHandle(h); }
    cached = ok ? 1 : 0;
    return ok;
}

bool Recorder::Start(std::function<ActivityInfo()> src) {
    if (impl_->runFlag) return true;
    impl_->src = std::move(src);
    if (!IsSupported()) {
        impl_->stats.supported = false;
        impl_->stats.lastError = "当前系统不支持 Windows.Graphics.Capture 窗口捕获";
        LogF("[rec] WGC not supported");
        return false;
    }
    impl_->stats.supported = true;
    impl_->runFlag = 1;
    impl_->thread = CreateThread(nullptr, 0, ThreadProc, impl_, 0, &impl_->threadId);
    if (!impl_->thread) { impl_->runFlag = 0; return false; }
    return true;
}

void Recorder::Stop() {
    if (!impl_ || !impl_->runFlag) return;
    InterlockedExchange(&impl_->runFlag, 0);
    if (impl_->thread) {
        WaitForSingleObject(impl_->thread, 6000);
        CloseHandle(impl_->thread);
        impl_->thread = nullptr;
    }
    {
        Recorder::Impl::CsLock lk(impl_->cs);
        impl_->stats.running = false;
        impl_->stats.capturing = false;
    }
}

bool Recorder::Running() const { return impl_ && impl_->runFlag != 0; }

void Recorder::FinalizeCurrentClip() {
    if (impl_) InterlockedExchange(&impl_->finalizeReq, 1);
}

RecorderStats Recorder::Snapshot() {
    Recorder::Impl::CsLock lk(impl_->cs);
    return impl_->stats;
}

DWORD WINAPI Recorder::ThreadProc(LPVOID p) {
    Impl* impl = (Impl*)p;
    impl->Loop();
    return 0;
}

} // namespace pb
