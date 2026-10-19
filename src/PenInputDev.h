#pragma once
// ============================================================
//  PenInput::Dev —— 单个数位设备的解析状态
//
//  为什么单独成一个头文件：
//  它原本是 PenInput 的 private 嵌套类型，导致离线测试既无法构造它、
//  也无法调用以它为参数的解析函数。测试要在「没有对应品牌硬件」的
//  情况下验证解析与映射逻辑（开发者只有 Wacom，无法测 Huion /
//  XP-Pen / Gaomon 等），就必须能手工填出各种能力组合的 Dev。
//
//  这里只搬位置，字段与语义保持不变。
// ============================================================
#include "Common.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <string>
#include <vector>

namespace pb {

// 报文诊断日志的状态机（供 PenInput::DecideReportLog 判定「这条要不要记」）。
//
// 为什么不是简单计数前 N 条：真实报告（diagnostics_20261017_233222）里 XP-Pen
// 的前 5 条报文全是悬空（tip=0、坐标不变），额度用尽后**落笔数据再也不记**，
// 导致无法看出该设备落笔时压感/坐标/HID 通道是否正常——这是排查盲点。
// 改为「前 3 条基线 + 关键状态跳变各记一次」，跳变比连续采样更有诊断价值。
struct ReportLogState {
    int  baseline = 0;         // 已记录的基线条数（前 3 条无条件记）
    bool sawTipDown = false;   // 已记录首次落笔（tip 0→1）
    bool sawPressure = false;  // 已记录首次出现压感（pressure 0→>0）
    bool sawEraser = false;    // 已记录首次橡皮
    bool sawTipUp = false;     // 已记录首次抬笔（tip 1→0）
    bool lastTip = false;      // 上一条采样的 tip（检测跳变用）
    float lastPressure = 0.f;  // 上一条采样的压感
};

struct Dev {
    HANDLE               handle = nullptr;
    // 设备序号：进程内自增，首次见到该 handle 时分配。
    // 用于采集层按设备分桶（多块板/数位屏同时使用时笔画不交织）。
    // 不用 HANDLE 本身：HANDLE 值会被系统复用，跨会话不稳定。
    // 0 保留给「未知」，与旧日志缺失 did 字段的默认值一致。
    uint32_t             devSeq = 0;
    std::wstring         path;
    std::string          name;
    std::vector<BYTE>    preparsedBlob;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS            caps{};
    std::vector<HIDP_LINK_COLLECTION_NODE> links;

    USAGE topUsage = 0, topUsagePage = 0;
    DWORD vid = 0, pid = 0;
    uint8_t type = PEN_UNKNOWN;
    bool  needPointerType = false;

    bool  hasX = false, hasY = false, hasPressure = false;
    bool  xRel = false, yRel = false;
    LONG  lxMin = 0, lxMax = 1, lyMin = 0, lyMax = 1;
    LONG  pressMin = 0, pressMax = 1;
    bool  hasTipSwitch = false, hasInRange = false, hasEraser = false;
    bool  hasTouch = false, hasBarrel = false, usesReportIDs = false;

    // 笔所在 link collection 索引（HidP_GetUsageValue/GetUsages 的第 3 参数）。
    // 0 = 不指定（取报文中第一个同 Usage 的字段）。
    //
    // 为什么需要它：复合设备（Wacom 的笔/触摸/ExpressKeys 在同一个
    // preparsed data 里）的笔集合与触摸集合**都有 X/Y**。官方注释明确：
    //   "If a report packet contains two different fields with the same
    //    Usage and UsagePage, they can be distinguished with the optional
    //    LinkCollection field value."
    // 且 hidpi.h 警告传 0 时 "these functions will only work with the first
    // field of the structure" —— 即可能取到**触摸集合**的 X/Y 而非笔的。
    //
    // 定位方式：笔集合必然含 TipSwitch(0x42) 或 TipPressure(0x30)，
    // 从这两个 cap 的 LinkCollection 字段反推，比遍历 links 树更可靠。
    // 找不到（如纯触摸设备）时保持 0，行为与改动前一致。
    USHORT penLink = 0;

    // 报文中出现过的 Report ID（用于 HID 解析形态探测）
    std::vector<USAGE> reportIDs;
    std::vector<BYTE> scratch;

    // 报文解析形态缓存：-1=未探测 0=原始直读 1=需前缀 parseCachedId
    int   parseCachedForm = -1;
    USAGE parseCachedId = 0;
    USHORT parseCachedLink = 0;    // 采纳时实际使用的 link collection（0=未指定）
    int   parseFails = 0;
    bool  failLogged = false;
    bool  rawLogged = false;
    ReportLogState repLog;          // 报文诊断日志状态（前3条基线 + 关键跳变）
    bool  seenRawLogged = false;   // 是否已记录「首次收到此设备报文」
    bool  noXYLogged = false;      // 是否已记录「无 X/Y 能力」丢弃
    bool  touchPadLogged = false;  // 是否已记录「触控板非绘图设备，已排除」
    bool  zeroCnt_ = false;        // 是否已记录「dwCount/dwSizeHid 为 0」丢弃

    // 确认不是笔类设备（无 X/Y 能力）后置位，此后该设备的报文直接早退。
    //
    // 为什么需要：改用 RIDEV_PAGEONLY 注册整个 0x0D 页后，触控板、触摸屏、
    // 手写面板等杂设备的报文也会涌进来，触摸屏多点触控时每秒可达数百条。
    // 早退让这类设备的每条报文开销只剩一次 map 查找。
    bool  rejected_ = false;

    // 穷举 Report ID 时被范围校验拒绝的次数（用于诊断「一直在猜但都猜错」）
    int   bruteRejected = 0;
    bool  bruteWarned = false;     // 首次拒绝已告警
    bool  bruteLogged = false;     // 首次穷举成功已告警（提示需人工核对）

    // 每设备的报文与解析计数。
    // 用途：「收到报文但一条都没解析成功」是设备已识别、解析路径却坏了的
    // 明确异常信号，必须让用户看见（见 Capture::Snapshot 的提示），
    // 而不是静默地记成零笔画。此前只有全局计数，无法定位是哪台设备。
    uint64_t devReports = 0;   // 本设备收到的报文条数
    uint64_t devParsed  = 0;   // 本设备成功解析出的采样数

    // 映射：screen = logical * s + o
    // mappingSource: 0=无 1=指针API精确匹配 2=无候选(虚拟桌面兜底)
    //                3=鼠标反解标定 4=候选歧义(拒绝猜测，虚拟桌面兜底) 5=覆写指定
    double sx = 1, ox = 0, sy = 1, oy = 0;
    bool   mappingValid = false;
    int    mappingSource = 0;
    RECT   mapDisplay{};      // 注意：存的是「屏幕像素矩形」（device），非物理尺寸
    LONG   mapDevLeft = 0, mapDevTop = 0;

    // 速度
    bool    havePrev = false;
    int64_t prevT = 0;
    int32_t prevX = 0, prevY = 0;
    bool    prevTip = false;

    // 相对模式累加基准
    bool   haveRelBase = false;
    double relX = 0, relY = 0;

    // 最近逻辑坐标（供采集层标定）
    LONG    lastLogicalX = 0, lastLogicalY = 0;
    int64_t lastLogicalT = 0;

    // 标定样本
    std::vector<int32_t> calLX, calSX, calLY, calSY;
    bool calibRejectLogged = false;   // 退化解被拒已记过一次日志（限流）

    // —— 覆写（逃生舱口）结果 ——
    // 由 ApplyOverride 在 LoadDeviceInfo 之后、BuildMapping 之前填入。
    // 见 PenOverrides.h：描述符不规范或映射歧义无法自动消解时的人工修正。
    bool  ovApplied = false;      // 是否有覆写规则命中本设备
    bool  ovLogical = false;      // 量程被覆写（则 LoadDeviceInfo 读到的值已被替换）
    bool  ovPressure = false;     // 压感上限被覆写
    bool  ovPenLink = false;      // 笔集合索引被覆写
    bool  ovType = false;         // 设备类型被覆写
    bool  hasOvDisplay = false;   // 指定了目标显示器
    RECT  ovDisplay{};            // 该显示器的屏幕像素矩形
};

} // namespace pb
