#pragma once
// ============================================================
//  主窗口：iOS 风格外壳
//
//  布局
//  ────────────────────────────────────────────────
//  ┌──────────┬──────────────────────────────────┐
//  │ 侧边导航  │  标题栏（含操作按钮）                 │
//  │  总览     ├──────────────────────────────────┤
//  │  习惯     │                                  │
//  │  录屏     │        内容区（可滚动）             │
//  │  设置     │                                  │
//  └──────────┴──────────────────────────────────┘
//
//  · 分析在后台线程跑，界面只显示「正在统计…」，不冻结
//  · 采集状态实时刷新（前台软件、数位笔设备、计数）
//  · 支持最小化到托盘，符合「无感记录」定位
// ============================================================
#include "Common.h"
#include "Ui.h"
#include "Charts.h"
#include "Analyzer.h"
#include "Config.h"
#include "Video.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace pb {

// ------------------------------------------------------------
//  轻量内联文本输入（自绘，无系统控件）
// ------------------------------------------------------------
class TextInput {
public:
    void SetText(const std::wstring& s) { text_ = s; caret_ = (int)s.size(); }
    const std::wstring& Text() const { return text_; }
    void SetRect(const D2D1_RECT_F& r) { rect_ = r; }
    const D2D1_RECT_F& Rect() const { return rect_; }
    void Focus(bool f) { focus_ = f; if (!f) caret_ = (int)text_.size(); }
    bool Focused() const { return focus_; }

    void Draw(Painter& p, const D2D1_COLOR_F& fg, const D2D1_COLOR_F& bg,
              const D2D1_COLOR_F& border, bool readOnly = false);

    bool OnKey(WPARAM vk, LPARAM lp);
    bool OnChar(WPARAM ch);
    bool OnClick(float x, float y);

private:
    float CaretX(Painter* p);
    std::wstring  text_;
    int           caret_ = 0;
    D2D1_RECT_F   rect_ = RF(0, 0, 0, 0);
    bool          focus_ = false;
    int           scroll_ = 0;
    Painter*      lastPainter_ = nullptr;
};

// ------------------------------------------------------------
//  应用
// ------------------------------------------------------------
class App {
public:
    static App& I();
    // trayMode=true（开机自启，命令行带 --tray）：不显示窗口，直接静默进托盘，
    // 并弹一次气泡告知用户程序已在后台运行（否则桌面上什么都没有，容易以为没启动）。
    int Run(HINSTANCE hInst, int nCmdShow, bool trayMode = false);

    // 离屏渲染指定页面（0总览 1习惯 2录屏 3设置）到 PNG，用于无头验证布局
    // W/H 为物理像素；dpi 指定模拟的显示缩放（96=100%、120=125%、144=150%）。
    // 传 dpi 后布局按 W*96/dpi 的 DIP 尺寸排布，可复现高分屏下的裁切/错位。
    bool RenderPagePng(int tab, const std::string& path, int W, int H, float dpi = 96.f);
    // 无头预览初始化：准备主题/输入框等最小状态，不建窗口、不启动采集
    void InitForHeadlessPreview(bool dark, const Analysis* seedAnalysis);

    // 自定义消息 / 定时器（供 WndProc 静态函数使用）
    static const UINT WM_ANALYSIS_DONE = WM_APP + 10;
    static const UINT WM_TRAY          = WM_APP + 11;
    static const UINT WM_QUIT_EXIT     = WM_APP + 12;
    // 录屏后台任务（缩略图/导出）结束通知：工作线程完成后投递，
    // 由界面线程刷新卡片与提示（线程间只传 WPARAM/LPARAM，不碰界面状态）
    static const UINT WM_JOB_DONE      = WM_APP + 13;
    static const UINT TID_POLL = 1;
    static const UINT TID_ANIM = 2;
    // 总览图「概括 ↔ 明细」淡入淡出的驱动定时器。
    // 仅在动画进行中开启（约 60fps），动画结束立即关闭，避免常态空转耗电。
    static const UINT TID_FADE = 3;
    static const UINT FADE_FRAME_MS = 16;
    // 开机自启（--tray）后弹气泡告知「已在后台运行」的延迟定时器。
    // 必须延迟：开机瞬间 explorer.exe 可能还没准备好接收托盘通知，
    // 立即弹会静默丢失，用户看到的就是「桌面上什么都没有」。一次性，弹完即关。
    static const UINT TID_TRAY_BUBBLE = 4;
    static const UINT TRAY_BUBBLE_DELAY_MS = 6000;

private:
    App() = default;
    // 兜底回收后台工作线程：窗口正常销毁走 OnDestroy；但离屏预览等
    // 场景从不建窗口、OnDestroy 不会执行，若线程仍 joinable，进程退出时
    // std::thread 析构会调用 std::terminate（表现为 0xC0000409 fail-fast）。
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // —— 窗口消息 ——
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnCreate(HWND hwnd);
    void OnPaint();
    void OnSize(UINT w, UINT h);
    void OnDpiChanged(UINT dpi);
    // DPI 变化后按新尺寸重取窗口大小图标与托盘图标。
    // 窗口类注册时只有系统 DPI，跨屏拖到另一台缩放不同的显示器后，
    // 旧尺寸的图标会被系统拉伸显示；这里主动换成目标显示器的尺寸。
    void RefreshIconsForDpi();
    void OnTimer(WPARAM id);
    LRESULT OnMouse(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnKeyMsg(UINT msg, WPARAM wp, LPARAM lp);
    void OnWheel(float x, float y, float delta);
    void OnDestroy();
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

    // —— 布局 ——
    D2D1_RECT_F ContentRect() const;
    D2D1_RECT_F TitleRect() const;
    D2D1_RECT_F SideRect() const;

    // 物理像素 → DIP。
    // Win32 消息里的鼠标坐标（GET_X/Y_LPARAM、ScreenToClient 结果）永远是
    // 物理像素；而全部绘制矩形与命中矩形都在 DIP 空间（见 Painter::W()/H()
    // 的说明）。两个空间不换算就比较，缩放比 >100% 时命中区会整体偏移：
    // 150% 下点「总览」实际命中「习惯」，点「设置」无响应，且侧栏视觉右半
    // 部分被判成内容区。统一在消息入口换算一次，下游命中测试无需逐处修改。
    float ToDipX(int px) const { return (float)px / painter_.DpiScale(); }
    float ToDipY(int py) const { return (float)py / painter_.DpiScale(); }

    // —— 各页绘制 ——
    float DrawOverview(const D2D1_RECT_F& r);
    float DrawHabit(const D2D1_RECT_F& r);
    float DrawRecord(const D2D1_RECT_F& r);
    float DrawSettings(const D2D1_RECT_F& r);
    void  DrawSidebar(const D2D1_RECT_F& r);
    void  DrawTitleBar(const D2D1_RECT_F& r);
    void  DrawScrollbar(const D2D1_RECT_F& contentRect, float contentH);
    float ganttContentH() const;

    // —— 分析（后台线程） ——
    void RequestAnalysis(bool fullHistory);
    void AnalysisThreadMain(bool fullHistory);
    void OnAnalysisDone();

    // —— 设置输入 ——
    void SyncInputsFromConfig();
    void CommitInputs();

    // —— 数据 ——
    void RefreshDayList();

    // —— 交互辅助 ——
    bool OverClickable(float x, float y);
    void OnNavClick(float x, float y);
    void OnTitleBarClick(float x, float y);
    void OnContentClick(float x, float y);
    void SetTab(int t);
    void ToggleDarkMode();
    void OpenDataFolder();
    void OpenAuthorPage();          // 打开侧栏底部「@虞花」B 站主页
    void ExportDiagnostics();       // 导出设备诊断信息（供兼容性问题回传）
    void ToggleRecordVideo();       // 录屏页滑块：开/关录屏（并即时启停录屏线程）
    void ToggleCaptureMode();       // 录屏页/设置页滑块：整屏 ↔ 窗口 捕获模式（录制中禁止切换）
    void ChooseVideoCacheDir();     // 选择录屏缓存目录
    void ChooseExportDir();         // 选择默认导出目录
    void AddTargetFromForeground();

    // —— 托盘 ——
    void TrayCreate();
    void TrayDestroy();
    void TrayBalloon(const wchar_t* title, const wchar_t* text);  // 气泡提示（NIM_MODIFY）
    void ShowMainWindow(bool show);
    void ShowTrayMenu();
    void ToggleAutoStart();         // 翻转开机自启：写注册表 + 存配置 + 重绘（设置页与托盘菜单共用）

    // —— 成员 ——
    HINSTANCE   hInst_ = nullptr;
    HWND        hwnd_ = nullptr;
    Painter     painter_;
    Theme       theme_;
    bool        dark_ = false;

    int         tab_ = 0;
    float       sideW_ = 208.f;
    float       titleH_ = 56.f;

    // 滚动
    float       scrollY_ = 0;
    float       contentH_ = 0;
    bool        draggingScroll_ = false;
    float       dragStartY_ = 0, dragStartScroll_ = 0;
    D2D1_RECT_F scrollTrackRect_ = RF(0,0,0,0), scrollThumbRect_ = RF(0,0,0,0);
    float       scrollMax_ = 0, scrollTrackTop_ = 0, scrollTrackH_ = 0, scrollThumbH_ = 0;

    // 分析
    std::thread         analysisThread_;
    std::mutex          analysisMtx_;
    std::atomic<bool>   analysisBusy_{ false };
    std::atomic<bool>   analysisPending_{ false };
    Analysis            analysis_;
    Analysis            sharedAnalysis_;
    bool                sharedReady_ = false;
    GanttView           gantt_;
    HabitView           habit_;
    bool                useFullHistory_ = true;
    bool                dataReady_ = false;

    // —— 自动刷新（边画边更新图表）——
    // 采集层没有把「新操作」直接推给界面，这里用轮询：周期性比较
    // 「采集层最近一次操作时刻」与「上次触发刷新时的水位线」，发现推进
    // 且距上次刷新超过节流间隔，就触发一次后台重算。此前完全缺少这个
    // 机制 —— RequestAnalysis 只在启动/改设置/点「重新统计」时调用，
    // 导致用户连续操作再多，两个图表也纹丝不动（正是用户报告的第 4 个问题）。
    int64_t             lastRefreshOpUs_ = 0;    // 上次触发刷新时的操作时间水位线
    int64_t             lastAutoRefreshUs_ = 0;  // 上次自动刷新时刻（节流用）
    static const int64_t AUTO_REFRESH_MIN_GAP_US = 2000000; // 自动刷新最小间隔 2s

    // 日期
    std::vector<std::string> days_;
    int                      daySel_ = -1;

    // 图表矩形（命中测试）
    D2D1_RECT_F ganttRect_ = RF(0,0,0,0);
    D2D1_RECT_F habitRect_ = RF(0,0,0,0);
    bool        ganttDragging_ = false;
    float       ganttScroll_ = 0.f;   // 总览图内部垂直滚动

    // 输入
    std::vector<TextInput> inputs_;
    int                    focusInput_ = -1;
    bool                   settingsDirty_ = false;

    // 控件矩形缓存
    std::vector<D2D1_RECT_F> navRects_;
    std::vector<D2D1_RECT_F> titleBtnRects_;
    std::vector<int>         titleBtnIds_;
    std::vector<std::wstring> habitTabs_;
    std::vector<D2D1_RECT_F> habitTabRects_;
    std::vector<D2D1_RECT_F> lagRects_;
    std::vector<D2D1_RECT_F> prefixRects_;
    std::vector<D2D1_RECT_F> videoRects_;
    std::vector<D2D1_RECT_F> toggleRects_;
    std::vector<bool>        toggleVals_;
    // 与 toggleRects_ / toggleVals_ 并行入列。点击分发**按 id**，不按数组下标：
    // 原先按 i==0/1/2 硬编码分发，导致「录制模式」滑块不得不刻意留在 toggleRects_
    // 之外（见 settingsModeSwitchRect_ 注释），每加一个开关都要重算所有下标。
    std::vector<int>         toggleIds_;
    static const int TOG_ENABLED   = 0;   // 无感记录 总开关
    static const int TOG_VIDEO     = 1;   // 录屏 总开关
    static const int TOG_AUTOSTART = 2;   // 开机自动启动
    static const int TOG_DARK      = 3;   // 深色模式
    D2D1_RECT_F addTargetBtn_ = RF(0,0,0,0);
    D2D1_RECT_F addConfirmBtn_= RF(0,0,0,0);
    D2D1_RECT_F openDataBtn_  = RF(0,0,0,0);
    D2D1_RECT_F diagBtn_      = RF(0,0,0,0);
    D2D1_RECT_F exitBtn_      = RF(0,0,0,0);

    // 侧栏底部作者主页链接「@虞花」的可点区域（DrawSidebar 每帧写入）
    D2D1_RECT_F authorLinkRect_ = RF(0,0,0,0);

    // —— 录屏页控件 ——
    D2D1_RECT_F recordSwitchRect_ = RF(0,0,0,0);   // 「是否录屏」滑块开关
    D2D1_RECT_F recordModeSwitchRect_ = RF(0,0,0,0); // 录屏页「窗口模式」滑块开关
    D2D1_RECT_F settingsModeSwitchRect_ = RF(0,0,0,0); // 设置页「窗口模式」滑块（独立成员，不进 toggleRects_ 以免索引移位）
    D2D1_RECT_F cachePathBtn_     = RF(0,0,0,0);   // 更改视频缓存路径
    D2D1_RECT_F exportPathBtn_    = RF(0,0,0,0);   // 更改默认导出路径

    // 托盘
    HICON trayIcon_ = nullptr;
    bool  trayActive_ = false;

    // 按 DPI 重取后、通过 WM_SETICON 挂到窗口上的图标（本对象持有并负责销毁）。
    // 初值为 nullptr，表示窗口当前用的是窗口类注册时那两个图标 ——
    // 类图标不在这里销毁：WM_SETICON 在窗口尚未设过 per-window 图标时
    // 返回的是类图标句柄，销毁它会让窗口类的 hIcon 字段悬空。
    HICON iconLarge_ = nullptr;
    HICON iconSmall_ = nullptr;

    // —— 录屏卡片列表 ——
    // 一张卡片 = 一个绘画文档的缓存文件夹（旧版散文件聚合为一张）。
    struct RecCard {
        DocCard        doc;          // 扫描结果（路径/文件数/时长/大小）
        std::string    thumbKey;     // 缩略图在 Painter 位图缓存中的 key
        std::wstring   thumbPath;    // 缩略图 PNG 路径（可能为空）
        bool           thumbLoading = false;   // 正在后台生成缩略图
        bool           thumbFailed  = false;   // 生成失败（画占位图）
        D2D1_RECT_F    exportBtn = RF(0,0,0,0);
        D2D1_RECT_F    clearBtn  = RF(0,0,0,0);
        D2D1_RECT_F    rect      = RF(0,0,0,0);
    };
    std::vector<RecCard> recCards_;
    int64_t              lastCardScanUs_ = 0;      // 上次扫描时刻（节流）
    static const int64_t CARD_SCAN_MIN_GAP_US = 800000;  // 扫描最小间隔 0.8s

    // —— 后台任务：单工作线程 + 队列 ——
    // 缩略图要解码视频首帧、导出要解码重编码整段视频，都必须在后台做，
    // 否则界面会冻结。用一条常驻工作线程串行消费队列，避免为每张卡片
    // 反复起停线程；退出时置停止标志并 join，保证不会有游离线程。
    enum class JobKind : uint8_t { Thumb = 0, Export = 1 };
    struct Job {
        JobKind      kind = JobKind::Thumb;
        DocCard      card;        // 导出用：该卡片的全部文件；缩略图用：逐个尝试取首帧
        std::wstring outPath;     // 缩略图 PNG / 导出 MP4
        std::string  key;         // 缩略图在位图缓存中的 key
        std::wstring doc;         // 归属文档名（完成后回找卡片）
    };
    std::mutex               jobMtx_;
    std::condition_variable  jobCv_;
    std::vector<Job>         jobQueue_;
    std::thread              worker_;
    std::atomic<bool>        workerQuit_{ false };
    std::atomic<bool>        workerStarted_{ false };

    // 导出状态（界面读取显示进度条）
    std::atomic<bool>        exportBusy_{ false };
    std::atomic<float>       exportProgress_{ 0.f };
    std::atomic<long>        exportCancel_{ 0 };
    std::wstring             exportDocName_;
    std::string              exportErr_;

    // 任务完成信息队列：工作线程写入、界面线程取走。
    // PostMessage 只能携带整数，而完成信息含字符串（缩略图 key、导出路径、
    // 错误原因），故放队列里；用 jobMtx_ 保护，连续完成也不会互相覆盖。
    struct JobDone {
        JobKind      kind = JobKind::Thumb;
        bool         ok = false;
        std::string  key;      // 缩略图缓存 key
        std::wstring out;      // 生成的文件路径
        std::wstring doc;      // 归属文档
        std::string  err;      // 失败原因
    };
    std::vector<JobDone>     jobDone_;

    void EnsureWorker();
    void PushJob(Job&& j);
    void WorkerLoop();
    void StopWorker();                         // 请求取消并 join 工作线程（幂等）
    void OnJobDone();                          // WM_JOB_DONE 处理：排空完成队列
    void ScanRecCards();                       // 扫描 videos 目录重建卡片
    void RequestThumb(RecCard& c);             // 排队生成某张卡片的缩略图
    void StartExport(const DocCard& card);     // 启动导出（拼接为单个视频）
    void StartClear(const DocCard& card);      // 清空卡片（移入回收站）

    // 实时状态
    int64_t lastStatusPollUs_ = 0;
    bool    captureOn_ = false;
    bool    fadeTimerOn_ = false;   // 甘特图淡入淡出定时器是否已开启
};

} // namespace pb
