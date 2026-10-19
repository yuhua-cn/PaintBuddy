#pragma once
// ============================================================
//  采集层：无感记录
//    · 前台软件识别（仅 PS / CSP / 优动漫 PAINT / SAI2 处于前台时记录输入；
//      优动漫是独立发行的中国版 CSP，进程名与 CSP 不同，见 Config.cpp 清单）
//    · 键盘：低级钩子，Ctrl+Z=撤回，Ctrl+Y / Ctrl+Shift+Z=重做，
//            其余按键+修饰键组合=普通操作；相同键 0.8s 内合并计数
//    · 鼠标：移动轨迹（时间戳/坐标/速度），左键按下抬起=笔画起止
//    · 数位笔：Raw Input + HID 解析，覆盖 External Pen(0x01)、
//              Integrated Pen(0x02)、Touch Screen(0x04)，
//              与鼠标并行工作，采集时间戳/坐标/速度/压力
//    · 落笔瞬间若有任意键按下 → 整次按笔记为「键盘操作」
//
//  所有钩子与 Raw Input 均安装在同一条带消息循环的采集线程上，
//  因此回调与状态天然同线程，无需加锁，也不会拖慢前台程序。
// ============================================================
#include "Common.h"
#include <functional>
#include <string>
#include <atomic>

namespace pb {

struct ForegroundState {
    HWND        hwnd     = nullptr;
    std::string exe;              // 小写进程名
    std::string title;            // UTF-8 窗口标题
    bool        isTarget = false;
    int64_t     sinceUs  = 0;     // 进入该前台状态的时刻
};

struct CaptureCounters {
    uint64_t keys          = 0;   // 键盘事件（合并后）
    uint64_t keysMergedIn  = 0;   // 被合并掉的重复按键次数
    uint64_t mouseStrokes  = 0;
    uint64_t mouseMoves    = 0;   // 自由移动段
    uint64_t mouseClicks   = 0;   // 滚轮 / 右键 / 中键（此前完全未计数，导致前台滚轮不计入操作数）
    uint64_t mousePoints   = 0;
    uint64_t penStrokes    = 0;
    uint64_t penPoints     = 0;
    uint64_t penRawReports = 0;   // 收到的数位设备原始报文数（诊断）
    uint64_t penSamples    = 0;   // 成功解析的笔采样数（诊断）
    uint64_t undoOps       = 0;
    uint64_t redoOps       = 0;
    uint64_t penKbAttr     = 0;   // 落笔时有键按下 → 记为键盘操作 的次数
    // 最后一层回退：数位笔设备在、却拿不到任何落笔数据（如 CSP 开 Wintab 独占）时，
    // 把鼠标笔画记为画笔笔画的条数。单列出来是为了**可审计**——这类笔画没有压感
    // 与橡皮信息，混进 penStrokes 就无法分辨数据来源、也无法评估回退是否误判。
    uint64_t mouseAsPenStrokes = 0;
    uint64_t devices       = 0;   // 识别到的数位设备数
    uint64_t drops         = 0;   // 缓冲区溢出丢弃
    bool     penInRange    = false;
    // 是否有「正在进行中」的笔画（笔尖未抬 / 鼠标左键未松）。
    // 录屏据此判断活跃：一次连续运笔可能持续数秒，期间不产生任何「完成的操作」，
    // 若只看 lastOpUs 就会把长笔画误判为空闲并切断录制。
    bool     strokeActive  = false;
    int64_t  lastOpUs      = 0;   // 最近一次「目标软件内操作」时刻
    bool     targetActive  = false;
    bool     running       = false;
    std::string fgExe;
    std::string fgTitle;
    std::vector<std::string> deviceNames;  // 已识别的数位设备描述
};

class Capture {
public:
    static Capture& I();

    bool Start();
    void Stop();
    bool IsRunning() const;

    // 前台状态变化监听（在采集线程回调，实现里应尽快返回）
    void SetForegroundListener(std::function<void(const ForegroundState&)> cb);
    // 实时操作原子监听（用于录屏空闲判定 / 界面实时刷新）
    void SetOpListener(std::function<void(const OpAtom&)> cb);

    // 快照（界面轮询用）
    CaptureCounters Snapshot();

    bool        IsTargetActive();
    int64_t     LastOpUs();
    ForegroundState FgSnapshot();

    // 实时活跃状态（无锁，采集线程写 / 录屏线程读）。
    // ActivityUs：最近一次「输入采样」时刻 —— 注意是采样级而非笔画级。
    //   一次连续运笔可持续数秒，期间不产生任何「已完成的操作」；
    //   若只看笔画收尾时刻，长笔画中途就会被判为空闲。
    // StrokeActive：是否有笔画正在进行（笔尖未抬 / 左键未松）。
    int64_t     ActivityUs() const { return actUs_.load(std::memory_order_relaxed); }
    bool        StrokeActive() const { return strokeAct_.load(std::memory_order_relaxed) != 0; }

    // 触发一次立即落盘（退出前）
    void FlushNow();

    // Impl 是不透明实现（pimpl），定义在 .cpp 内。前置声明放公开区，
    // 以便同文件内的静态钩子回调（KbProc/MsProc/FgProc）访问 g_impl。
    struct Impl;

private:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    Impl*               impl_ = nullptr;
    HWND                hwnd_ = nullptr;

    static DWORD WINAPI ThreadProc(LPVOID);
    static LRESULT CALLBACK WndProcS(HWND, UINT, WPARAM, LPARAM);

    volatile long       started_ = 0;
    HANDLE              thread_  = nullptr;
    DWORD               threadId_= 0;
    volatile long       stopReq_ = 0;

    std::function<void(const ForegroundState&)> fgCb_;
    std::function<void(const OpAtom&)>          opCb_;

    CRITICAL_SECTION    snapCs_;
    CaptureCounters     counters_;
    ForegroundState     fg_;

    // 实时活跃状态：采集线程写、录屏线程读，用原子量避免加锁（录屏轮询频繁）。
    std::atomic<int64_t> actUs_{0};       // 最近一次输入采样时刻（epoch 微秒）
    std::atomic<long>    strokeAct_{0};   // 1 = 有笔画进行中
};

} // namespace pb
