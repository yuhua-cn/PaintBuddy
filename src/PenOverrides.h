#pragma once
// ============================================================
//  数位设备覆写（逃生舱口）
//
//  设计来源：借鉴 OpenTabletDriver 的 Attributes 机制 —— 默认走自动
//  （HID 描述符 + 指针 API），但保留一个按设备精确覆写的出口。
//
//  为什么需要它：本项目无法为每个品牌做真机验证（开发者只有一块 Wacom）。
//  当某台设备的描述符不规范（量程声明错误、集合划分异常）或坐标映射存在
//  无法自动消解的歧义时，如果只能改代码重新发版，画师就得一直用着坏数据。
//  有了覆写文件，用户或我们自己改一个 JSON 就能修。
//
//  刻意不做的事：不建 OTD 式的 300+ 设备配置库。我们站在 HIDClass 之上，
//  绝大多数设备靠描述符自动适配就够；覆写只处理少数异常设备。
//
//  可测试性：解析、校验、匹配全部是接受文本/参数的纯函数（不做文件 I/O），
//  因此无需真机、无需真实配置文件即可离线覆盖各种写错的输入。
// ============================================================
#include "Common.h"
#include <string>
#include <vector>

namespace pb {

struct PenOverride {
    // ---------------- 匹配键 ----------------
    // 全部可选，但至少要指定一个（见 ParseOverrides 的校验），
    // 否则一条规则会命中所有设备 —— 那是配置事故，不是配置。
    bool        hasVid = false;
    DWORD       vid = 0;
    bool        hasPid = false;
    DWORD       pid = 0;
    // HID 顶级集合的 usage（如 0x01 / 0x02 / 0x04）。
    // 这一维很关键：同一块板子会暴露多个集合（笔 / 触摸 / 按键），
    // 指定 usage 才能只覆写笔集合而不误伤触摸集合。
    bool        hasUsage = false;
    USHORT      usage = 0;
    // HID 设备路径子串（大小写不敏感）。用于区分同 VID/PID 的不同接口/型号，
    // 例如 "MI_00&Col04"。
    std::string pathContains;

    // ---------------- 覆写字段 ----------------
    // 未指定的字段保持自动探测结果；指定了才覆盖。
    bool        hasLogical = false;
    LONG        lxMin = 0, lxMax = 0, lyMin = 0, lyMax = 0;
    bool        hasPressureMax = false;
    LONG        pressureMax = 0;
    // 笔所在 link collection 索引。描述符集合划分异常、自动反推取错时用。
    bool        hasPenLink = false;
    USHORT      penLink = 0;
    // 设备类型：1=外接数位板 2=数位屏 4=触摸屏
    bool        hasDeviceType = false;
    uint8_t     deviceType = PEN_UNKNOWN;
    // 目标显示器。指定后坐标映射直接用它，跳过自动匹配与歧义判定 ——
    // 这是多屏歧义（两个 PEN 指针条目矩形不一致）的确定性解法。
    //
    // 优先用名称而非索引：EnumDisplayMonitors 的枚举顺序不保证跨重启稳定，
    // 只填索引会让用户配好的文件某天突然映射到错误的屏幕。
    bool        hasDisplayName = false;
    std::string displayName;           // 如 "\\\\.\\DISPLAY1"
    bool        hasDisplayIndex = false;
    int         displayIndex = 0;      // 兜底用；日志会列出可用名称供参考

    bool Matches(DWORD devVid, DWORD devPid, USHORT devUsage,
                 const std::string& devPathLower) const;
};

// 解析覆写文件内容。纯函数，不做 I/O。
//
// 单条规则非法时**跳过该条**并把原因记入 errors，其余规则照常生效 ——
// 绝不能因为一处写错就让整份配置失效，更不能让设备变得不可用。
// 返回值：JSON 本身可解析即为 true（errors 非空表示有规则被拒绝）。
bool ParseOverrides(const std::string& jsonText,
                    std::vector<PenOverride>& out,
                    std::vector<std::string>& errors);

// 从 DataDir()/pen_overrides.json 加载。
// 文件不存在是正常情况（绝大多数用户不需要覆写），静默返回空、不记错误。
std::vector<PenOverride> LoadOverrides(std::vector<std::string>& errors);

// 把覆写里的显示器指定（名称优先，索引兜底）解析为屏幕像素矩形。
bool ResolveDisplayRect(const PenOverride& o, RECT& outRect);

// 枚举显示器名称，用于日志提示用户该填什么。
std::vector<std::string> EnumDisplayNames();

} // namespace pb
