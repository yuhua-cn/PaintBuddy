#pragma once
// ============================================================
//  配置：目标软件列表、合并窗口、录屏参数、分类阈值
//  以 JSON 存于 DataDir/config.json，用户可在界面中修改
// ============================================================
#include "Common.h"

namespace pb {

struct Config {
    // —— 采集 ——
    bool              enabled        = true;    // 总开关（无感记录）
    std::vector<std::string> targets;           // 受监控的绘画软件进程名（小写）
    int               keyMergeMs     = 800;     // 连续相同键合并窗口
    bool              captureMouseWhenIdle = false; // 目标非前台时是否也记（默认否）
    // 最后一层回退：识别到数位笔设备、却拿不到任何落笔数据时（如 CSP 开启
    // Wintab 独占笔尖通道），把鼠标笔画记为画笔笔画。默认开启 —— 零笔画对
    // 用户是彻底失效，而误判只是来源不精确（单独计数 mouseAsPenStrokes 可审计）。
    // 代价：这类笔画没有压感与橡皮信息。可在 config.json 置 false 关闭。
    bool              mouseAsPenFallback = true;

    // —— 录屏 ——
    // 一个绘画文件对应一个视频文件；停顿/无操作的时间不写入视频。
    // 「无活动多久后不再写帧」是内部固定窗口（见 Recorder.cpp kActiveWindowUs），
    // 不再作为用户可配参数：会话只在换文档/关录制/退出时结束，不因空闲切片。
    bool              recordVideo    = false;   // 默认关闭：录屏耗资源，需用户主动开启
    // 捕获模式：0 = 整屏（默认，画面含整个桌面，被遮挡也不会黑块）；
    //           1 = 窗口（只录绘画主窗口，画面干净，但被别的窗口压住时会录到黑块，
    //               且液化等模态全屏窗口期间主窗口被完全遮挡 → 录不到画面）。
    int               captureMode    = 0;
    int               videoBitrateKbps = 5000;
    int               videoFps       = 30;
    int               minClipMs      = 400;     // 短于此长度的文件丢弃
    bool              recordAudio    = false;

    // —— 路径（空字符串 = 用系统默认，见 Store.cpp 的 ApplyPathOverrides）——
    // videoCacheDir: 录屏缓存目录，默认 %LOCALAPPDATA%\PaintingBuddy\videos
    // exportDir    : 导出成品目录，默认系统「视频」库
    // 两者都可被用户改成任意磁盘目录（例如放到空间更大的盘）。
    std::string       videoCacheDir;
    std::string       exportDir;

    // —— 分类阈值（0 = 自动统计得出）——
    double            n1Sec          = 0.0;     // 暂停绘画下限（0 = 自动统计）
    double            n2Sec          = 0.0;     // 打断绘画下限（0 = 自动统计）
    double            depthGapSec    = 0.0;     // 深度绘画的块内中位间隔上限

    // —— 界面 ——
    bool              darkMode       = true;    // 默认深色
    int               lastTab        = 0;
    // 开机自动启动（默认开启）：写 HKCU\...\Run，命令行带 --tray，
    // 登录后直接进托盘静默运行，不弹窗口。每次启动会按此值同步注册表，
    // 故 exe 换位置后从新位置启动一次即可自动纠正。见 Store.cpp AutoStart*。
    bool              autoStart      = true;
    int               ver            = 0;       // 配置版本（迁移用）

    void LoadDefaults();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    bool IsTarget(const std::string& exeLower) const;
};

Config& Cfg();          // 全局单例
void    CfgReload();
void    CfgSave();

// 已知绘画软件默认清单（含 CSP / 优动漫 / SAI2 / PS 的常见进程名）
const char* const* DefaultTargets(size_t& count);

} // namespace pb
