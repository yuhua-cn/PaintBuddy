#pragma once
// ============================================================
//  录屏：一个绘画文件 = 一个视频文件，视频里只有「正在操作」的画面
//
//  需求要点
//  ────────
//  · 一个绘画文件对应一个视频文件：录制会话绑定「文档」，
//    不按停顿/空闲切片。只有换到另一个文档、关闭录制或退出程序才收尾。
//    切到别的软件再切回来，仍然续写同一个文件。
//  · 视频里不含停顿与无操作时间：实现方式是「跳帧写入」——
//    没有输入活动的帧直接丢弃不写，已写入的帧按序号连续排时间戳，
//    于是视频时间轴自然被压缩，播放时看不到任何等待。
//  · 连续运笔（数秒长笔画）绝不被切断：活跃判定用「采样级」时刻
//    与「笔画进行中」标志，而不是「已完成操作」的时刻。
//  · 文件名由绘画文档名得出（清洗非法字符 + 会话起始时间戳）。
//
//  技术选型：Windows.Graphics.Capture（WGC）+ Media Foundation H.264
//  · 捕获**整个显示器**而非单个窗口：绘画软件在缩放画布、展开面板、
//    切换 DPI 时窗口尺寸频繁变化，窗口捕获会被迫反复结束会话重开；
//    且窗口被遮挡时可能交付带黑区的帧。整屏捕获没有这些问题。
//  · 关闭黄色捕获边框，保证无感
// ============================================================
#include "Common.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace pb {

// 由采集层提供的「当前活动状态」
struct ActivityInfo {
    bool         targetActive = false;  // 前台是否为受监控绘画软件
    int64_t      lastOpUs     = 0;      // 最近一次「已完成操作」时刻（epoch 微秒）
    int64_t      actUs        = 0;      // 最近一次输入采样时刻（运笔途中持续刷新）
    bool         strokeActive = false;  // 是否有笔画正在进行（笔尖未抬 / 左键未松）
    void*        fgHwnd       = nullptr;// 前台窗口（HWND，用 void* 避免头文件依赖）
    std::wstring fgTitle;               // 前台窗口标题
};

struct RecorderStats {
    bool     supported   = false;  // 系统是否支持 WGC
    bool     running     = false;  // 录制线程是否在跑
    bool     capturing   = false;  // 捕获会话已建立（已绑定某个绘画文档）
    bool     writing     = false;  // 正在写入视频文件（已产生首帧）
    uint64_t framesCaptured = 0;
    uint64_t framesWritten  = 0;
    uint64_t framesSkipped  = 0;   // 因无输入活动被丢弃（即「不含停顿」的实现）
    uint64_t clipsCreated   = 0;   // 已创建的录制会话数（= 生成的视频文件数）
    int      width = 0, height = 0;
    double   clipSeconds = 0.0;    // 当前文件已写入的视频时长
    std::string sessionDoc;        // 当前会话对应的绘画文档名（UTF-8）
    std::string currentFile;       // 当前正在写的文件（UTF-8）
    std::vector<std::string> recentFiles;  // 最近生成的片段
    std::string lastError;
};

class Recorder {
public:
    static Recorder& I();

    // 系统能力探测（Win10 1803+ 且 WGC 可用）
    bool IsSupported();

    // 启动录制线程。src 用于轮询当前活动状态。
    bool Start(std::function<ActivityInfo()> src);
    // 停止并收尾（阻塞，会完成当前片段的 Finalize）
    void Stop();
    bool Running() const;

    RecorderStats Snapshot();

    // 立即收尾当前片段（例如手动点「保存这段」）
    void FinalizeCurrentClip();

    // 从窗口标题提取「文档名」，用于录屏文件名。
    //   "shijian.psd @ 11.1% (图层 199, RGB/8) *" -> "shijian"
    //   "作品_线稿.csp - Clip Studio Paint"        -> "作品_线稿"
    //   "涂鸦_01.sai2 - PaintTool SAI"             -> "涂鸦_01"
    // 提为公共静态函数以便单元测试直接验证。
    static std::wstring DocNameFromTitle(const std::wstring& title);

    // 判断从标题提取出的文档名是否「像真实绘画文档」，可用于建卡/切卡。
    // 不合格（空、含 .exe、括号不配对如「液化 (shijian」）时返回 false，
    // 录屏层据此**冻结**当前文档名而不新建垃圾卡片。提为公共静态以便单测。
    static bool IsPlausibleDocName(const std::wstring& doc);

    // 由采集层组装的「当前活动状态」提供者。
    // 提为公共静态函数：程序入口启动录屏、界面里切换录屏开关后重启录屏，
    // 都需要同一个实现。此前入口里是 static 函数，界面无法引用。
    static ActivityInfo MakeActivityInfo();

private:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;

    static DWORD WINAPI ThreadProc(LPVOID);
};

} // namespace pb
