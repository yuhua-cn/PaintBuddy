#include "PenInject.h"

namespace pb {

// ------------------------------------------------------------
// 采集注入证据（唯一允许调用 Win32 API 的地方）
// ------------------------------------------------------------
InjectEvidence CaptureInjectEvidence(DWORD extraInfo, DWORD msllFlags, bool queryIms) {
    InjectEvidence e;
    e.extraInfo = extraInfo;
    e.flags = msllFlags;

    if (queryIms) {
        INPUT_MESSAGE_SOURCE src{};
        e.srcOk = GetCurrentInputMessageSource(&src) ? 1 : 0;
        if (e.srcOk) {
            e.deviceType = (uint32_t)src.deviceType;
            e.originId = (uint32_t)src.originId;
        }
    }
    return e;
}

// ------------------------------------------------------------
// 判定（纯函数，可离线测试）
// ------------------------------------------------------------
bool IsPenInjectedEvent(const InjectEvidence& e, bool realPenSilent,
                        const PenPositionMatch* posMatch, InjectReason* reason) {
    InjectReason r = InjectReason::kNotPen;

    // 判据 1：FROMTOUCH 签名（Windows Ink 促进的标准签名）。
    // 这是原实现的唯一判据，已在本机 Wacom 上验证有效，必须保持优先。
    if ((e.extraInfo & PB_INJ_SIG_MASK) == (PB_INJ_SIG_FROMTOUCH & PB_INJ_SIG_MASK)) {
        r = InjectReason::kFromTouchSignature;
    }
    // 判据 2：系统上报的输入源设备类型为笔。
    // 不依赖任何厂商签名，覆盖「驱动/Ink 注入但不设 FROMTOUCH」的情况
    // —— 这正是真实用户报告里 penRawReports=0 且笔被记成鼠标笔画的成因。
    else if (e.srcOk && (e.deviceType & PB_INJ_IMDT_PEN) != 0) {
        r = InjectReason::kImsDevicePen;
    }
    // 判据 3：位置吻合兜底（近期有真实笔采样，且注入坐标与之接近）。
    // 保留原实现语义：仅在真实笔通道有采样时才可能成立。
    else if (posMatch && posMatch->recentPenSample) {
        double dx = (double)posMatch->mx - (double)posMatch->lastPenX;
        double dy = (double)posMatch->my - (double)posMatch->lastPenY;
        if (dx * dx + dy * dy <= PB_INJ_POS_MATCH_R2) {
            r = InjectReason::kPositionMatch;
        }
    }

    // 真实笔通道已在工作时，注入事件不是笔的来源。
    // 注意：判据 1/2 命中说明「这个事件由笔产生」，与「原始通道是否静默」是
    // 两件事——上层（Capture）会另外用 RealPenSilent() 决定是否走 Ink 回退，
    // 这里不掺入该门控，保持判定语义单一、可独立测试。
    (void)realPenSilent;

    if (reason) *reason = r;
    return r != InjectReason::kNotPen;
}

// 手指触摸判定：仅在「确证 deviceType 含 TOUCH 且不含 PEN」时返回 true。
//
// 为什么这么保守：MOUSEEVENTF_FROMTOUCH 这个签名同时覆盖手指触摸与笔
// （名字里的 TOUCH 就是它的字面含义），所以不能靠签名区分两者。
// 提取不到 pointerId、查询失败、或类型不明确时一律返回 false ——
// 保持原有「当作笔」的行为，宁可漏丢一个触摸，也不误伤真实笔数据。
bool IsTouchEvent(const InjectEvidence& e, UINT32 pointerId, DWORD pointerType, bool pointerTypeKnown) {
    // 优先用系统上报的输入源类型（若可用且明确）
    if (e.srcOk) {
        bool isPen = (e.deviceType & PB_INJ_IMDT_PEN) != 0;
        bool isTouch = (e.deviceType & PB_INJ_IMDT_TOUCH) != 0;
        if (isTouch && !isPen) return true;
        if (isPen) return false;
        // 类型不明确（UNAVAILABLE / MOUSE 等）→ 继续用 pointer 类型判断
    }
    // 退回到 pointer 类型（GetPointerInfo 的结果，由调用方提供）
    if (pointerTypeKnown && pointerId != 0) {
        return pointerType == PB_INJ_PT_TOUCH;
    }
    return false;
}

const char* InjectReasonName(InjectReason r) {
    switch (r) {
    case InjectReason::kFromTouchSignature: return "FROMTOUCH 签名";
    case InjectReason::kImsDevicePen:       return "系统输入源=笔(IMDT_PEN)";
    case InjectReason::kPositionMatch:      return "位置吻合兜底";
    case InjectReason::kNotPen:             return "非笔";
    }
    return "?";
}

// 最后一层回退门控（纯函数）。
//
// 三条必须同时成立，任一不成立即关闭：
//   1) hasPenDevice —— 确实识别到了有压感的数位笔设备。没有数位板时，
//      用户用的就是普通鼠标，把鼠标笔画记成笔是纯粹的错报。
//   2) !everPenTipDown —— 本会话从未收到过落笔采样。这是关键：一旦收到过
//      真实落笔数据，说明原始通道工作正常，绝不能抢它的记录。
//      注意判据是「落笔」而非「任何采样」——Wintab 独占时悬停报文照常进来
//      （报告 155346 的 759 条全是 tip=0），若按「有无采样」判活，回退会被
//      悬停数据永久关闭，这正是缺陷⑦。
//   3) !everInjectedPen —— 注入路径也没识别出笔。若三判据（FROMTOUCH 签名 /
//      IMDT_PEN / 位置吻合）任一命中，说明压感可取、语义准确，应走那条路，
//      不必退化到有损回退。
//
// penInRangeRecently / eventSynthesized **不参与判定**，只由调用方写进日志留证。
// 原因是硬门槛会否决掉真正需要回退的用户（见头文件说明）。误判风险由两层
// 兜住：单独计数 mouseAsPenStrokes 使其可审计，以及配置项可一键关闭。
bool ShouldUseMouseFallback(const MouseFallbackCtx& ctx) {
    if (!ctx.hasPenDevice)    return false;
    if (ctx.everPenTipDown)   return false;
    if (ctx.everInjectedPen)  return false;
    return true;
}

} // namespace pb
