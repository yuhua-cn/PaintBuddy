#pragma once
// ============================================================
// 数位笔注入识别：「这个鼠标事件其实是笔吗？」
//
// 为什么需要：真实用户报告（diagnostics_20261018_144042/_144046）
// 显示 penRawReports=0 且 mouseStrokes=73/123、penStrokes=0，
// 同时 undo=7、redo=30 证明用户确实在作画。即：这些用户的笔
// 被 Windows Ink 或厂商驱动接管后以「注入鼠标事件」出现，但
// 事件不带 FROMTOUCH 签名（0xFF515700），于是被当成普通鼠标，
// 核心统计指标（画笔笔画数）全错。
//
// 原实现（Capture.cpp IsInjectedByPen）只有两个判据：
//   1) FROMTOUCH 签名；
//   2) 「近期有真实笔采样且位置吻合」兜底 —— 但它依赖
//      lastPenSampleUs != 0，而这些用户原始报文为 0，该值恒为 0，
//      兜底**永久失效**。
// 于是只剩判据 1，签名不符就整条通道失效。
//
// 本模块补上第三个、也是唯一不依赖厂商签名的判据：
// GetCurrentInputMessageSource() 的 deviceType & IMDT_PEN。
// 该 API 由系统给出输入源类型（WinUser.h:15964-15997），
// 与厂商如何设置 dwExtraInfo 无关。
//
// 注意：IMO_INJECTED_FROM_INK 在本项目所用 SDK（10.0.26100）中
// **不存在**，INPUT_MESSAGE_ORIGIN_ID 只有 UNAVAILABLE/HARDWARE/
// INJECTED/SYSTEM 四个值，不要引用不存在的枚举。
//
// 判定逻辑全部写成纯函数（IsPenInjectedEvent / IsTouchEvent），
// 唯一碰 Win32 的是 CaptureInjectEvidence —— 因为这些用户的环境
// 无法在开发者机器上复现（开发者本机 Wacom 一切正常），只有
// 纯函数才能离线验证。这是项目一贯的做法，参见 ResolvePenType /
// EvaluateCalibration / PickPointerCandidate / DecideReportLog。
// ============================================================

#include <windows.h>
#include <cstdint>

namespace pb {

// —— 常量（值来自 WinUser.h，此处显式写出以便离线测试不依赖宏展开）——
static const DWORD   PB_INJ_SIG_MASK       = 0xFFFFFF00;  // dwExtraInfo 高位 = 签名
static const DWORD   PB_INJ_SIG_FROMTOUCH  = 0xFF515700;  // MOUSEEVENTF_FROMTOUCH
static const uint32_t PB_INJ_IMDT_PEN      = 0x00000008;  // IMDT_PEN
static const uint32_t PB_INJ_IMDT_TOUCH    = 0x00000004;  // IMDT_TOUCH
static const uint32_t PB_INJ_IMDT_MOUSE    = 0x00000002;  // IMDT_MOUSE
static const DWORD   PB_INJ_PT_TOUCH       = 1;           // POINTER_INPUT_TYPE.PT_TOUCH
static const DWORD   PB_INJ_LLMHF_INJECTED = 0x01;        // MSLLHOOKSTRUCT.flags
static const double  PB_INJ_POS_MATCH_R2   = 100.0;       // 位置吻合阈值（平方距离）

// 判定命中的原因，用于日志与测试断言
enum class InjectReason : uint8_t {
    kNotPen = 0,
    kFromTouchSignature,   // 命中 FROMTOUCH 签名（原路径）
    kImsDevicePen,         // 系统输入源报告为笔（新增，签名无关）
    kPositionMatch         // 位置吻合兜底（原路径）
};

// 一次鼠标事件的注入证据快照
struct InjectEvidence {
    DWORD    extraInfo = 0;   // MSLLHOOKSTRUCT.dwExtraInfo
    DWORD    flags     = 0;   // MSLLHOOKSTRUCT.flags（LLMHF_INJECTED 等）
    uint32_t deviceType = 0;  // INPUT_MESSAGE_SOURCE.deviceType
    uint32_t originId   = 0;  // INPUT_MESSAGE_SOURCE.originId
    uint8_t  srcOk      = 0;  // GetCurrentInputMessageSource 是否成功
};

// 位置吻合兜底所需的上下文（由调用方提供）
struct PenPositionMatch {
    bool    recentPenSample = false;  // 近期（80ms 内）有真实笔采样
    int32_t mx = 0, my = 0;           // 当前注入鼠标坐标
    int32_t lastPenX = 0, lastPenY = 0;
};

// 采集证据：唯一调用 Win32 API 的函数。
// 必须在低级鼠标钩子回调（MsProc）的上下文中调用 ——
// GetCurrentInputMessageSource 描述的是「当前正在处理的消息」的来源。
//
// queryIms=false 时跳过 GetCurrentInputMessageSource（srcOk 保持 0）。
// 用途：MsProc 是高频热路径（数位笔轮询可达 1000Hz），当廉价的
// FROMTOUCH 签名已命中时无需再查系统输入源，避免每条鼠标移动都多一次
// user32 调用。只有签名不命中（即需要新判据）时才传 true。
InjectEvidence CaptureInjectEvidence(DWORD extraInfo, DWORD msllFlags, bool queryIms = true);

// 判定：这个注入鼠标事件是否来自数位笔。
// realPenSilent 目前不参与判定（语义单一化，门控留给上层 Capture），
// 保留参数是为了将来需要时不必改签名。
bool IsPenInjectedEvent(const InjectEvidence& e, bool realPenSilent,
                        const PenPositionMatch* posMatch,
                        InjectReason* reason = nullptr);

// 手指触摸判定：仅在确证为触摸时返回 true（保守，避免误伤笔）。
// pointerType / pointerTypeKnown 由调用方通过 GetPointerInfo 提供。
bool IsTouchEvent(const InjectEvidence& e, UINT32 pointerId,
                  DWORD pointerType, bool pointerTypeKnown);

const char* InjectReasonName(InjectReason r);

// ------------------------------------------------------------
// 最后一层回退：「数位板确实在，但一条落笔数据都拿不到」
//
// 触发场景（用户复现 + 两份报告实证）：Clip Studio Paint 开启 Wintab 后，
// 数位驱动把**笔尖接触**数据交给 Wintab API 由 CSP 独占消费，Raw Input 侧
// 只剩「悬停」报文；同时驱动自行合成普通鼠标事件驱动光标 —— 这些事件既不带
// FROMTOUCH 签名，系统输入源也报 IMDT_MOUSE（驱动视角它就是鼠标），内核态
// 注入还使 MSLLHOOKSTRUCT.flags 不含 LLMHF_INJECTED，于是三个判据全部落空。
//
// 报告实证：
//   · diagnostics_20261018_155346：penSamples=759 全为 tip=0 悬停、
//     penStrokes=0，而 mouseStrokes=182、undo=11（用户确实在作画）。
//   · diagnostics_20261018_155341：penRawReports=0，注册回读
//     flags=0x120(PAGEONLY|INPUTSINK)、hwnd 与采集窗口一致且 valid=1
//     —— 注册没问题，系统就是不投递；且无 WARN 行（内核注入不带 INJECTED 标志）。
//
// 门控刻意收紧到三条同时成立，缺一即关闭：这是**有损**回退（拿不到压感与
// 橡皮，只保住笔画计数与轨迹），误判会把纯鼠标作画记成画笔。
// 写成纯函数以便离线验证 —— 该场景无法在开发者机器复现（本机 Wacom 正常）。
// ------------------------------------------------------------
struct MouseFallbackCtx {
    bool hasPenDevice    = false;  // 存在任何「有压感的笔设备」（注意不能用 DominantPenType：
                                   // 数位屏与数位板并存时它按设计返回 UNKNOWN，会误关回退）
    bool everPenTipDown  = false;  // 本会话是否收到过落笔采样（tip=1）
    bool everInjectedPen = false;  // 注入路径是否已识别出笔（三判据任一命中）
    // —— 以下两条只作日志留证，**不作硬门槛** ——
    // 为什么不能当门槛：报告 20261018_155341 里 penRawReports=0（笔完全静默）且
    // 驱动内核态注入不带 LLMHF_INJECTED，两条证据同时不成立；若据此否决，该用户
    // 仍然拿不到任何笔画，回退等于没生效。取舍是：宁可让「数位板插着却改用真鼠标
    // 作画」被记成画笔（少见、且单独计数可审计、可用配置关闭），也不能让真正需要
    // 回退的用户继续零笔画。
    bool penInRangeRecently = false;  // 笔近 2s 内产生过 in-range 报文（笔在手上/板上）
    bool eventSynthesized   = false;  // 本次按下带注入标志（非硬件鼠标产生）
};

// 三条否决全部通过才启用最后一层回退。
bool ShouldUseMouseFallback(const MouseFallbackCtx& ctx);

} // namespace pb
