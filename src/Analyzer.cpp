#include "Analyzer.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace pb {

// ============================================================
//  状态命名
// ============================================================
const char* StateName(State s) {
    switch (s) {
    case State::Normal:    return "normal";
    case State::Correct:   return "correct";
    case State::Brush:     return "brush";
    case State::NormalGap: return "gap";
    case State::Pause:     return "pause";
    default:               return "?";
    }
}
const char* StateNameCn(State s) {
    switch (s) {
    case State::Normal:    return "普通操作";
    case State::Correct:   return "撤回/重做";
    case State::Brush:     return "画笔";
    case State::NormalGap: return "普通间隔";
    case State::Pause:     return "暂停绘画";
    default:               return "未知";
    }
}

// ============================================================
//  小工具
// ============================================================
static double PercentileSorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    double idx = p * (double)(sorted.size() - 1);
    size_t lo = (size_t)floor(idx);
    size_t hi = (size_t)ceil(idx);
    if (lo == hi) return sorted[lo];
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (idx - (double)lo);
}

static double Median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return PercentileSorted(v, 0.5);
}

// 在 [lo,hi] 区间内找「对数空间最大跳变」，返回跳变点（几何均值）与倍率。
// 操作间隔的分布通常是多峰的（连续作画 / 停顿思考 / 离开），
// 峰与峰之间的谷就是天然的分类时间点。
//
// 带样本量守卫：要求区间内样本足够多、且候选点两侧都有足够样本。
// 原实现只要求区间内有 2 个点即可出结果，实测会让阈值由极少数离群样本
// 单独决定——一份真实日志里 n2 的跳变点右侧只有 1 个样本，删掉那 1 个
// 样本 n2 就从 273s 掉到 60s 地板。守卫不满足时返回 false，让调用方
// 退回更稳健的分位数分支。
static bool GuardedValley(const std::vector<double>& sorted, double lo, double hi,
                          double& outVal, double& outRatio) {
    outVal = 0.0; outRatio = 0.0;
    size_t i0 = (size_t)(std::lower_bound(sorted.begin(), sorted.end(), lo) - sorted.begin());
    size_t i1 = (size_t)(std::upper_bound(sorted.begin(), sorted.end(), hi) - sorted.begin());
    if (i1 < i0 + 20) return false;                       // 区间内样本太少
    size_t span = i1 - i0;
    size_t minSide = std::max((size_t)3, (size_t)(0.10 * (double)span));
    double best = 0.0; size_t bIdx = 0; bool found = false;
    for (size_t i = i0 + minSide; i + 1 + minSide < i1; ++i) {
        if (sorted[i] <= 0.0) continue;
        double r = sorted[i + 1] / sorted[i];
        if (r > best) { best = r; bIdx = i; found = true; }
    }
    if (!found || best <= 0.0) return false;
    outRatio = best;
    outVal = std::sqrt(sorted[bIdx] * sorted[bIdx + 1]);
    return true;
}

// —— 阈值语义边界 ——
// 这些不是「防除零」的数值夹取，而是分类阈值的语义下限：低于它们的值
// 会让指标失去意义。三份真实日志验证过：不设这些边界时暂停阈值会落到
// 0.42~0.65s（落笔间的凝视全被记成暂停，次数虚高 10~21 倍），
// n2 落到 60s（喝水、翻参考都被记成离开，作画块被切碎）。
static const size_t kMinGapSamples = 8;     // 有效停顿样本少于此值 → 回退默认阈值

// n1 =「暂停绘画」下限：人类有意识地停下手（看画、比对参考、思考构图）。
// 低于 30 秒的空档属正常作画节奏的一部分，不该单独计为一次暂停。
// 上限 90s 是为了保证 n2（300s）仍满足「n2 >= n1*3」的区分度要求。
static const double kN1Floor = 30.0;
static const double kN1Cap   = 90.0;
static const double kN1Pct   = 0.97;   // 兜底：停顿候选池的 97 百分位

// mergeSec =「活动合并」窗口：贴合运笔节奏，量级是秒而不是几十秒。
// 与 n1 解耦后，n1 可以自由抬高而不影响活动数与甘特图。
static const double kMergeFloor = 1.0;
static const double kMergeCap   = 15.0;

static const double kN2Floor = 300.0;       // 「打断」下限不低于 5 分钟（人类离开画布的常见时长）
static const double kN2Cap   = 3600.0;

static State OpToState(const Op& o) {
    switch (o.kind) {
    case OpKind::Undo:
    case OpKind::Redo:   return State::Correct;
    case OpKind::Brush:  return State::Brush;
    default:             return State::Normal;
    }
}

// ============================================================
//  原始记录 → 操作原子
// ============================================================
std::vector<Op> RecordsToOps(const std::vector<RawRecord>& recs) {
    std::vector<Op> ops;
    ops.reserve(recs.size());
    for (const auto& r : recs) {
        if (r.tUs <= 0) continue;
        switch (r.type) {
        case 'k': {
            Op o;
            o.t = r.tUs;
            o.end = r.endUs > r.tUs ? r.endUs : r.tUs;
            o.kind = r.kind;
            o.src = 0;
            o.intensity = (float)r.count;
            o.note = DescribeKey(r.vk, r.mods);
            if (r.count > 1) { o.note += " x" + std::to_string(r.count); }
            ops.push_back(o);
            break;
        }
        case 'm': {
            Op o;
            o.t = r.tUs;
            o.end = r.endUs > r.tUs ? r.endUs : r.tUs;
            o.kind = OpKind::Normal;

            // button == 3 是滚轮（采集层已按 keyMergeMs 合并，count = 合并条数）。
            // 按用户口径，滚轮**视为键盘操作**：src=0，强度用合并次数，
            // 与键盘的 "xN" 记法保持一致。
            if (r.button == 3) {
                o.src = 0;
                o.intensity = (float)std::max(1, r.count);
                o.note = "滚轮";
                if (r.count > 1) o.note += " x" + std::to_string(r.count);
                ops.push_back(o);
                break;
            }

            // 鼠标自由移动段：与笔画同一规则，段起始时有键按住 → 归为键盘操作
            o.src = r.kbAttr ? 0 : 1;
            o.intensity = r.pathLen;
            o.note = r.kbAttr ? "鼠标移动(归为键盘)" : "鼠标移动";
            ops.push_back(o);
            break;
        }
        case 'M': {   // 鼠标笔画（左键按下→抬起）
            Op o;
            o.t = r.tUs;
            o.end = r.endUs > r.tUs ? r.endUs : r.tUs;
            o.kind = OpKind::Normal;
            // 落笔瞬间有键按住 → 整次记为键盘操作（与数位笔同一规则）。
            // 此前这里硬编码 src=1 且忽略 r.kbAttr，导致「键盘按下时的鼠标
            // 操作记为键盘操作」这条规则对鼠标笔画完全不生效。
            o.src = r.kbAttr ? 0 : 1;
            o.intensity = r.pathLen;
            o.note = (r.button == 0) ? (r.kbAttr ? "鼠标笔画(归为键盘)" : "鼠标笔画")
                                     : "鼠标按键";
            // 左键拖拽 = 真实作画笔画（右键/中键多为菜单与平移，不计）。
            // 这里必须计数：鼠标笔画的 kind 是 Normal、src 是 1，
            // 按 kind==Brush 统计会把纯鼠标作画的用户当成「零画笔」。
            o.paintCount = (r.button == 0) ? 1 : 0;
            ops.push_back(o);
            break;
        }
        case 'p': {   // 数位笔
            if (r.hover) continue;   // 悬空移动不算操作
            Op o;
            o.t = r.tUs;
            o.end = r.endUs > r.tUs ? r.endUs : r.tUs;
            // 落笔瞬间有键按下 → 整次按笔记为键盘操作
            o.kind = r.kbAttr ? OpKind::Normal : OpKind::Brush;
            o.src  = r.kbAttr ? 0 : 2;
            o.intensity = r.pathLen;
            o.note = r.kbAttr ? "笔(归为键盘)" : (r.eraser ? "橡皮" : "画笔笔画");
            // 落笔（非悬空）即一次绘画动作。有键按住时 kind 会变成 Normal，
            // 但它确实画了东西，所以 paintCount 不受 kbAttr 影响。
            o.paintCount = 1;
            ops.push_back(o);
            break;
        }
        default: break;   // 'f' 前台切换不作为操作
        }
    }
    std::sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        if (a.t != b.t) return a.t < b.t;
        return a.end < b.end;
    });
    return ops;
}

// ============================================================
//  连续事件合并
//
//  规则：除「画笔」与「撤回/重做」外，相邻间隔 < windowUs 的操作合并为
//  一个事件；合并后事件的时间戳 = 组内最早那个操作的时间戳。
//
//  画笔与修正类操作「独立成组」：既不吸收相邻的普通操作，也不会被
//  相邻的普通操作吸收，因此它们的计数与时刻始终精确。
//
//  为什么在分析层做而不在采集层做：落盘的是原始事件流（只追加、永不
//  改写历史），合并只是「统计口径」，改窗口或改规则时历史数据无需重录。
// ============================================================
std::vector<Op> MergeOps(std::vector<Op> ops, int64_t windowUs) {
    if (ops.size() < 2 || windowUs <= 0) return ops;

    // 先按时间排序（RecordsToOps 已排过，但公开接口不能假设调用方排过）
    std::sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        if (a.t != b.t) return a.t < b.t;
        return a.end < b.end;
    });

    // 独立成组的操作：画笔、撤回、重做
    auto isStandalone = [](const Op& o) {
        return o.kind == OpKind::Brush ||
               o.kind == OpKind::Undo  ||
               o.kind == OpKind::Redo;
    };

    std::vector<Op> out;
    out.reserve(ops.size());

    size_t i = 0;
    while (i < ops.size()) {
        const Op& cur = ops[i];
        if (isStandalone(cur)) {
            out.push_back(cur);          // 独立事件，原样保留
            out.back().merged = std::max(1, cur.merged);
            ++i;
            continue;
        }

        // 普通操作：向后吸收连续的可合并操作
        Op acc = cur;
        acc.merged = std::max(1, cur.merged);
        size_t j = i + 1;
        while (j < ops.size()) {
            const Op& nxt = ops[j];
            if (isStandalone(nxt)) break;            // 遇到画笔/修正 → 停止吸收
            if (nxt.t - acc.end >= windowUs) break;  // 间隔已达窗口 → 另起一组
            acc.end = std::max(acc.end, nxt.end);
            acc.intensity += nxt.intensity;
            acc.merged += std::max(1, nxt.merged);
            acc.paintCount += nxt.paintCount;   // 组内绘画动作数守恒
            // 分类与来源取组内首个（acc.kind / acc.src 已是首个的值）
            ++j;
        }
        if (acc.merged > 1) acc.note += " ×" + std::to_string(acc.merged);
        out.push_back(std::move(acc));
        i = j;
    }
    return out;
}

// ============================================================
//  阈值推导：统计用户习惯得出「最佳分类时间点」
// ============================================================
Thresholds DeriveThresholds(const std::vector<Op>& ops, const Config& cfg) {
    Thresholds th;

    // 用户显式设定则直接采用（关闭自动）
    if (cfg.n1Sec > 0)       { th.n1Sec = cfg.n1Sec;       th.n1Auto = false; }
    if (cfg.n2Sec > 0)       { th.n2Sec = cfg.n2Sec;       th.n2Auto = false; }
    if (cfg.depthGapSec > 0) { th.depthGapSec = cfg.depthGapSec; th.depthAuto = false; }

    if (ops.size() < 2) {
        th.method = "数据不足，使用默认阈值";
        return th;
    }

    // —— 收集相邻操作间隔（秒）——
    std::vector<double> allGaps;
    allGaps.reserve(ops.size());
    int64_t prevEnd = ops[0].end;
    for (size_t i = 1; i < ops.size(); ++i) {
        int64_t g = ops[i].t - prevEnd;
        if (g > 0) allGaps.push_back((double)g / 1000000.0);
        prevEnd = std::max(prevEnd, ops[i].end);
    }
    if (allGaps.empty()) {
        th.method = "无有效间隔，使用默认阈值";
        return th;
    }
    std::sort(allGaps.begin(), allGaps.end());

    // —— 停顿候选池：剔除合并窗口以内的间隔 ——
    // MergeOps 已经把「间隔 < EVENT_MERGE_WINDOW_US」的相邻普通操作合并成
    // 一个事件，即程序自己认定这段属于同一个连续动作。但画笔与撤回/重做是
    // 「独立成组」，它们之间的微小间隔不会被合并掉，于是全部漏进阈值统计。
    //
    // 后果（三份真实日志实测）：笔触节奏间隔占样本池 86%~92%，把 p75 压到
    // 0.42~0.65s，n1 因此塌缩，暂停次数虚高 10~21 倍。
    //
    // 但只用于「分位数兜底」，不用于谷检测：真实双峰的谷（例如密集段 0.5s
    // 与稀疏段 8s 之间的 16 倍跳变）恰恰落在 1s 边界两侧，滤掉就看不见了。
    const double filterSec = (double)EVENT_MERGE_WINDOW_US / 1e6;
    std::vector<double> gaps;
    gaps.reserve(allGaps.size());
    for (double g : allGaps) if (g >= filterSec) gaps.push_back(g);

    if (gaps.size() < kMinGapSamples) {
        // 整场几乎没有超过 1 秒的空档 → 样本不足以统计「停顿」结构。
        // 此时任何自动值都是噪声，直接用默认阈值最安全。
        th.gapSamples = (int)gaps.size();
        char b[192];
        snprintf(b, sizeof(b), "停顿样本不足（%zu 个 >=%.1fs），使用默认阈值",
                 gaps.size(), filterSec);
        th.method = b;
        return th;
    }
    std::sort(gaps.begin(), gaps.end());
    th.gapSamples = (int)gaps.size();

    char buf[256];
    buf[0] = '\0';

    // —— n2（打断下限）——
    // 谷检测下界用 n2 的语义地板：低于 5 分钟的空档不该被叫作「打断」。
    // 原实现从 30s 起搜且地板只有 60s，实测会产出 60s，使画师退后看画、
    // 喝水、翻参考都被记成离开，作画块被切碎（一份真实日志：打断 12 次，
    // 正确应为 6 次）。
    if (th.n2Auto) {
        double v = 0, ratio = 0;
        if (GuardedValley(allGaps, kN2Floor, 86400.0, v, ratio) && ratio >= 2.0) {
            th.n2Sec = std::min(kN2Cap, std::max(kN2Floor, v));
            snprintf(buf, sizeof(buf), "n2=%.0fs（%.1f 倍跳变）", th.n2Sec, ratio);
        } else {
            th.n2Sec = std::min(kN2Cap, std::max(kN2Floor, PercentileSorted(gaps, 0.95)));
            snprintf(buf, sizeof(buf), "n2=%.0fs（95 百分位，无有效跳变）", th.n2Sec);
        }
    }

    // —— mergeSec（活动合并窗口）——
    // 沿用原 n1 的推导逻辑：谷检测跑在全池上（含亚秒样本），这样「密集运笔
    // 节奏 ↔ 停顿」之间的真实跳变才看得见；找不到有效谷时退回停顿候选池的
    // 75 百分位。量级是秒级，贴合运笔节奏。
    //
    // 三份真实日志实测值 2.0~2.5s，对应活动数 69 / 148 / 73。
    if (th.mergeAuto) {
        double v = 0, ratio = 0;
        if (GuardedValley(allGaps, 0.02, kMergeCap * 2.0, v, ratio) && ratio >= 1.8) {
            th.mergeSec = std::min(kMergeCap, std::max(kMergeFloor, v));
        } else {
            th.mergeSec = std::min(kMergeCap, std::max(kMergeFloor, PercentileSorted(gaps, 0.75)));
        }
    }

    // —— n1（暂停绘画下限）——
    // 与 mergeSec 解耦后，这里的量级是「人类有意识地停下手」，几十秒。
    // 取停顿候选池的 97 百分位再夹到语义区间 [30, 90]。
    //
    // 实测（三份日志，暂停次数）：
    //   n1=2.5s（旧口径）→ 66 / 114 / 45    绝大多数只是落笔间的凝视
    //   n1=30s           →  7 /  12 /  3
    // 上限 90s 保证 n2 的语义地板 300s 仍满足「n2 >= n1*3」的区分度。
    if (th.n1Auto) {
        double v = PercentileSorted(gaps, kN1Pct);
        th.n1Sec = std::min(kN1Cap, std::max(kN1Floor, v));
        char b2[160];
        snprintf(b2, sizeof(b2), "暂停下限 n1=%.0fs（%.0f 百分位）", th.n1Sec, kN1Pct * 100.0);
        th.method = std::string(b2) + "，" + buf;
    }

    // 保证 n1 与 n2 有足够区分度（地板已含语义下限）
    if (th.n2Sec < th.n1Sec * 3.0) {
        th.n2Sec = th.n1Sec * 3.0;
        if (th.n2Sec < kN2Floor) th.n2Sec = kN2Floor;
    }
    if (th.method.empty()) th.method = buf;
    return th;
}

// ============================================================
//  主分析
// ============================================================
Analysis Analyze(std::vector<Op> ops, const Thresholds& thIn) {
    Analysis a;
    a.th = thIn;

    std::sort(ops.begin(), ops.end(), [](const Op& x, const Op& y) {
        if (x.t != y.t) return x.t < y.t;
        return x.end < y.end;
    });
    a.ops = std::move(ops);

    if (a.ops.empty()) {
        a.empty = true;
        return a;
    }
    a.empty = false;
    a.spanStart = a.ops.front().t;
    a.spanEnd   = a.ops.back().end;

    const double n1Us = thIn.n1Sec * 1e6;      // 暂停绘画下限
    const double n2Us = thIn.n2Sec * 1e6;      // 打断绘画下限
    const double mergeUs = thIn.mergeSec * 1e6; // 活动合并窗口（与 n1 解耦）

    // —— 逐操作扫描：分类间隔、切分作画块、构建序列 ——
    PaintBlock cur;
    cur.start = a.ops[0].t;
    cur.end   = a.ops[0].end;
    cur.actBegin = a.activities.size();
    cur.gapBegin = a.gaps.size();

    std::vector<double> curGaps;   // 当前块内的间隔，用于中位数
    bool blockOpen = true;

    auto closeBlock = [&]() {
        if (!blockOpen) return;
        cur.actEnd = a.activities.size();
        cur.gapEnd = a.gaps.size();
        cur.activityCount = (int)(cur.actEnd - cur.actBegin);
        cur.medianGapSec = curGaps.empty() ? 0.0 : Median(curGaps);
        double dur = (double)(cur.end - cur.start) / 1e6;
        cur.opsPerSec = dur > 0 ? (double)cur.opCount / dur : 0.0;
        // 深度判定：块内典型间隔越小 → 操作越密集 → 深度绘画
        cur.deep = (cur.medianGapSec > 0.0) && (cur.medianGapSec <= a.th.depthGapSec);
        a.blocks.push_back(cur);
        blockOpen = false;
    };

    auto openBlock = [&](const Op& o) {
        cur = PaintBlock();
        cur.start = o.t;
        cur.end   = o.end;
        cur.actBegin = a.activities.size();
        cur.gapBegin = a.gaps.size();
        curGaps.clear();
        blockOpen = true;
    };

    for (size_t i = 0; i < a.ops.size(); ++i) {
        const Op& o = a.ops[i];
        State st = OpToState(o);

        // 计数
        switch (o.kind) {
        case OpKind::Undo:  a.undoTotal++;   break;
        case OpKind::Redo:  a.redoTotal++;   break;
        case OpKind::Brush: a.brushTotal++;  break;
        default:            a.normalTotal++; break;
        }

        if (i > 0) {
            const Op& p = a.ops[i - 1];
            int64_t gs = p.end;
            int64_t ge = o.t;
            if (ge > gs) {
                Gap g; g.start = gs; g.end = ge;
                double gl = (double)(ge - gs);
                if (gl >= n2Us) {
                    // —— 打断绘画：结束当前块，另起新块 ——
                    g.type = GapType::Interrupt;
                    a.interruptCount++;
                    a.interruptUs += (ge - gs);
                    a.gaps.push_back(g);
                    closeBlock();
                    openBlock(o);
                    // 打断不计入马尔可夫序列（跨会话无转移关系）
                    a.seqBlockEnd.push_back(a.sequence.size());
                    // 注意：不能 continue，当前操作仍要并入新块
                } else {
                    if (gl >= n1Us) {
                        g.type = GapType::Pause;
                        a.pauseCount++;
                        a.pauseUs += (ge - gs);
                        cur.pauseUs += (ge - gs);
                    } else {
                        g.type = GapType::Normal;
                        a.normalGapCount++;
                    }
                    a.gaps.push_back(g);
                    curGaps.push_back(gl / 1e6);
                    a.sequence.push_back(g.type == GapType::Pause ? State::Pause : State::NormalGap);
                }
            }
        }

        if (!blockOpen) openBlock(o);

        // —— 活动合并 ——
        // 只与「属于当前块」的最后一个活动合并，避免跨打断把两次会话粘在一起。
        // 窗口用 mergeSec 而非 n1：n1 是「暂停」判定线（几十秒量级），
        // 若用它合并会把相隔半分钟的操作粘成同一个活动。
        bool merged = false;
        if (a.activities.size() > cur.actBegin) {
            Activity& last = a.activities.back();
            if (last.start >= cur.start &&
                o.t <= last.end + (int64_t)mergeUs) {
                last.end = std::max(last.end, o.end);
                last.opCount++;
                last.sumIntensity += o.intensity;
                if (o.kind == OpKind::Undo || o.kind == OpKind::Redo) last.undoCount++;
                else if (o.kind == OpKind::Brush) last.brushCount++;
                else last.normalCount++;
                int mx = std::max(last.normalCount, std::max(last.undoCount, last.brushCount));
                last.dominant = (mx == last.brushCount) ? State::Brush
                              : (mx == last.undoCount)  ? State::Correct
                                                        : State::Normal;
                merged = true;
            }
        }
        if (!merged) {
            Activity act;
            act.start = o.t; act.end = std::max(o.t, o.end);
            act.dominant = st;
            act.opCount = 1;
            act.sumIntensity = o.intensity;
            if (o.kind == OpKind::Undo || o.kind == OpKind::Redo) act.undoCount = 1;
            else if (o.kind == OpKind::Brush) act.brushCount = 1;
            else act.normalCount = 1;
            a.activities.push_back(act);
        }

        cur.end = std::max(cur.end, o.end);
        cur.opCount++;
        cur.paintCount += o.paintCount;   // 设备无关的绘画动作数
        switch (st) {
        case State::Correct: cur.undoCount++; break;
        case State::Brush:   cur.brushCount++; break;
        default:             cur.normalCount++; break;
        }

        a.sequence.push_back(st);
    }
    closeBlock();
    a.seqBlockEnd.push_back(a.sequence.size());

    a.blockCount = (int)a.blocks.size();

    // —— 汇总 ——
    for (const auto& b : a.blocks) {
        a.activePaintUs += (b.end - b.start);
        if (b.deep) a.deepBlocks++; else a.shallowBlocks++;
    }

    // —— 深度分界自动推导（用块内中位间隔的中位数）——
    if (a.th.depthAuto) {
        std::vector<double> meds;
        meds.reserve(a.blocks.size());
        for (const auto& b : a.blocks)
            if (b.medianGapSec > 0.0 && b.opCount >= 3) meds.push_back(b.medianGapSec);
        if (meds.size() >= 2) {
            a.th.depthGapSec = std::min(30.0, std::max(0.3, Median(meds)));
            a.th.blockSamples = (int)meds.size();
            char b2[160];
            snprintf(b2, sizeof(b2), "，深度分界=%.2fs（%zu 个块的中位间隔之中位数）",
                     a.th.depthGapSec, meds.size());
            a.th.method += b2;
            // 用新分界重算 deep 标记
            a.deepBlocks = a.shallowBlocks = 0;
            for (auto& b : a.blocks) {
                b.deep = (b.medianGapSec > 0.0) && (b.medianGapSec <= a.th.depthGapSec);
                if (b.deep) a.deepBlocks++; else a.shallowBlocks++;
            }
        } else {
            a.th.method += "，深度分界样本不足，沿用默认值";
        }
    }

    // —— 设备无关的撤回率（undoPerPaint）——
    //
    // 只算不判：深浅分类目前仍只看 medianGapSec。曾经尝试用「撤回率高于
    // 当天中位 1.3 倍则降级为浅度」，但实测推翻了它的依据 ——
    // 那个 1.3 倍阈值唯一的支撑是一个「122 次撤回 / 0 次数位笔」的块，
    // 当时以为它没在画画。用设备无关的分母重算后发现它有 285 次真实鼠标
    // 笔画，撤回率 0.428 与当天中位 0.4067 持平，根本不是离群值。
    // 用正确分母重算后，该规则在三份真实日志上一块都不会改变，
    // 1.3 倍就成了无法校准的凭空数字，故不引入。
    //
    // 分母用 paintCount（设备无关）而不是 brushCount：鼠标笔画的 kind 是
    // Normal，按 brushCount 统计会把纯鼠标作画的用户算成「零画笔」。
    // 实测该漏计会让分母少一半（285 → 136），比率虚高一倍。
    //
    // 这里仍然计算并保存，供界面展示与后续用更多数据校准阈值使用。
    for (auto& b : a.blocks) {
        b.undoPerPaint = (b.paintCount > 0)
                       ? (double)b.undoCount / (double)b.paintCount
                       : 0.0;
    }

    // ========================================================
    //  马尔可夫统计（仅在块内计转移，跨打断不计）
    // ========================================================
    size_t seqStart = 0;
    for (size_t be : a.seqBlockEnd) {
        if (be <= seqStart) { seqStart = be; continue; }
        size_t len = be - seqStart;
        // 状态频次
        for (size_t i = 0; i < len; ++i) {
            State s = a.sequence[seqStart + i];
            a.stateTotal[(size_t)s]++;
        }
        // 一阶
        for (size_t i = 0; i + 1 < len; ++i) {
            State s = a.sequence[seqStart + i];
            State t = a.sequence[seqStart + i + 1];
            a.transCount[(size_t)s][(size_t)t]++;
        }
        // 长程关联：滞后 1..MAX_LAG
        for (int k = 1; k <= MAX_LAG; ++k) {
            for (size_t i = 0; i + (size_t)k < len; ++i) {
                State s = a.sequence[seqStart + i];
                State t = a.sequence[seqStart + i + (size_t)k];
                a.lagCount[k - 1][(size_t)s][(size_t)t]++;
            }
        }
        // 二阶条件：P(c | a,b)
        for (size_t i = 0; i + 2 < len; ++i) {
            State x = a.sequence[seqStart + i];
            State y = a.sequence[seqStart + i + 1];
            State z = a.sequence[seqStart + i + 2];
            a.lag2Count[(size_t)x][(size_t)y][(size_t)z]++;
        }
        seqStart = be;
    }

    // 归一化
    for (size_t i = 0; i < (size_t)State::COUNT; ++i) {
        long long row = 0;
        for (size_t j = 0; j < (size_t)State::COUNT; ++j) row += a.transCount[i][j];
        for (size_t j = 0; j < (size_t)State::COUNT; ++j)
            a.trans[i][j] = row > 0 ? (double)a.transCount[i][j] / (double)row : 0.0;

        for (int k = 0; k < MAX_LAG; ++k) {
            long long rk = 0;
            for (size_t j = 0; j < (size_t)State::COUNT; ++j) rk += a.lagCount[k][i][j];
            for (size_t j = 0; j < (size_t)State::COUNT; ++j)
                a.lagProb[k][i][j] = rk > 0 ? (double)a.lagCount[k][i][j] / (double)rk : 0.0;
        }
    }
    for (size_t x = 0; x < (size_t)State::COUNT; ++x)
        for (size_t y = 0; y < (size_t)State::COUNT; ++y) {
            long long row = 0;
            for (size_t z = 0; z < (size_t)State::COUNT; ++z) row += a.lag2Count[x][y][z];
            for (size_t z = 0; z < (size_t)State::COUNT; ++z)
                a.lag2Prob[x][y][z] = row > 0 ? (double)a.lag2Count[x][y][z] / (double)row : 0.0;
        }

    // —— 找出显著的长程关联（提升度 lift = P(j|i,lag k) / P(j)）——
    long long total = 0;
    for (size_t i = 0; i < (size_t)State::COUNT; ++i) total += a.stateTotal[i];
    if (total > 0) {
        std::vector<Analysis::NotableLag> cand;
        for (int k = 2; k <= MAX_LAG; ++k) {          // k=1 是普通一阶转移，不算「长程」
            for (size_t i = 0; i < (size_t)State::COUNT; ++i) {
                long long row = 0;
                for (size_t j = 0; j < (size_t)State::COUNT; ++j) row += a.lagCount[k - 1][i][j];
                if (row < 20) continue;               // 样本太少不具统计意义
                for (size_t j = 0; j < (size_t)State::COUNT; ++j) {
                    if (a.stateTotal[j] < 10) continue;
                    double pj = (double)a.stateTotal[j] / (double)total;   // 边缘概率
                    if (pj <= 0) continue;
                    double pij = a.lagProb[k - 1][i][j];
                    double lift = pij / pj;
                    if (lift >= 1.5) {
                        Analysis::NotableLag nl;
                        nl.valid = true; nl.lag = k;
                        nl.from = (State)i; nl.to = (State)j;
                        nl.prob = pij; nl.baseline = pj; nl.lift = lift; nl.n = row;
                        cand.push_back(nl);
                    }
                }
            }
        }
        std::sort(cand.begin(), cand.end(), [](const Analysis::NotableLag& x, const Analysis::NotableLag& y) {
            return x.lift > y.lift;
        });
        if (cand.size() > 6) cand.resize(6);
        a.notables = cand;
    }

    return a;
}

// ============================================================
//  甘特时间轴（打断折叠为最多 1 小时）
// ============================================================
int64_t GanttTimeline::RealToDisp(int64_t realUs) const {
    if (segments.empty()) return 0;
    for (const auto& s : segments) {
        if (realUs >= s.realStart && realUs <= s.realStart + (s.dispEnd - s.dispStart))
            return s.dispStart + (realUs - s.realStart);
    }
    // 落在打断区（被压缩）：钳制到最近段边界
    for (const auto& s : segments) {
        int64_t realEnd = s.realStart + (s.dispEnd - s.dispStart);
        if (realUs < s.realStart) return s.dispStart;
        if (realUs <= realEnd) return s.dispEnd;
    }
    return segments.back().dispEnd;
}

int64_t GanttTimeline::DispToReal(int64_t dispUs) const {
    if (segments.empty()) return 0;
    for (const auto& s : segments) {
        if (dispUs >= s.dispStart && dispUs <= s.dispEnd)
            return s.realStart + (dispUs - s.dispStart);
    }
    if (dispUs < segments.front().dispStart) return segments.front().realStart;
    const auto& last = segments.back();
    return last.realStart + (last.dispEnd - last.dispStart);
}

GanttTimeline BuildGantt(const Analysis& a, bool collapseInterrupts) {
    GanttTimeline tl;
    if (a.blocks.empty()) { tl.dispStart = tl.dispEnd = tl.realStart = tl.realEnd = 0; return tl; }

    tl.realStart = a.spanStart;
    tl.realEnd   = a.spanEnd;
    int64_t disp = 0;

    // 甘特段合并窗口：与活动合并同源（mergeSec），不用 n1。
    // n1 已改为「暂停」判定线（几十秒量级），用它会把相隔很远的同态活动
    // 粘成一条色带。
    const double n1Us = a.th.mergeSec * 1e6;

    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const PaintBlock& b = a.blocks[bi];

        // —— 块间打断 ——
        if (bi > 0) {
            int64_t rs = a.blocks[bi - 1].end;
            int64_t re = b.start;
            if (re > rs) {
                int64_t realLen = re - rs;
                int64_t dispLen = collapseInterrupts
                    ? std::min<int64_t>(realLen, INTERRUPT_DISPLAY_CAP_US) : realLen;
                DispSegment seg; seg.dispStart = disp; seg.dispEnd = disp + dispLen; seg.realStart = rs;
                tl.segments.push_back(seg);

                GanttCell c;
                c.dispStart = disp; c.dispEnd = disp + dispLen;
                c.realStart = rs;   c.realEnd = re;
                c.kind = GK_INTERRUPT;
                c.truncated = collapseInterrupts && realLen > INTERRUPT_DISPLAY_CAP_US;
                tl.cells.push_back(c);

                GanttBreak gb; gb.dispPos = disp; gb.realStart = rs; gb.realEnd = re;
                tl.breaks.push_back(gb);

                disp += dispLen;
            }
        }

        // —— 块本身 ——
        int64_t bs = b.start, be = b.end;
        int64_t blockLen = std::max<int64_t>(1, be - bs);
        {
            DispSegment seg; seg.dispStart = disp; seg.dispEnd = disp + blockLen; seg.realStart = bs;
            tl.segments.push_back(seg);
        }

        // 底色带（深度 / 浅度）
        {
            GanttCell band;
            band.dispStart = disp; band.dispEnd = disp + blockLen;
            band.realStart = bs;   band.realEnd = be;
            band.kind = b.deep ? GK_BAND_DEEP : GK_BAND_SHALLOW;
            band.intensity = (float)b.medianGapSec;
            tl.cells.push_back(band);
        }

        // 块内间隔（普通 / 暂停）
        for (size_t gi = b.gapBegin; gi < b.gapEnd && gi < a.gaps.size(); ++gi) {
            const Gap& g = a.gaps[gi];
            if (g.type == GapType::Interrupt) continue;
            if (g.start < bs || g.end > be) continue;
            GanttCell c;
            c.realStart = g.start; c.realEnd = g.end;
            c.dispStart = disp + (g.start - bs);
            c.dispEnd   = disp + (g.end - bs);
            c.kind = (g.type == GapType::Pause) ? GK_GAP_PAUSE : GK_GAP_NORMAL;
            tl.cells.push_back(c);
        }

        // 块内操作小段（同状态且间隔 < n1/2 的相邻活动合并，控制单元数量）
        int64_t runStart = -1, runEnd = -1;
        State   runState = State::Normal;
        float   runInt = 0.f;
        auto flushRun = [&]() {
            if (runStart < 0) return;
            GanttCell c;
            c.realStart = runStart; c.realEnd = runEnd;
            c.dispStart = disp + (runStart - bs);
            c.dispEnd   = disp + (runEnd - bs);
            c.kind = GK_OP;
            c.state = runState;
            c.intensity = runInt;
            tl.cells.push_back(c);
            runStart = -1;
        };
        for (size_t oi = b.actBegin; oi < b.actEnd && oi < a.activities.size(); ++oi) {
            const Activity& act = a.activities[oi];
            if (act.start < bs || act.end > be) continue;
            State st = act.dominant;
            if (runStart >= 0 && st == runState &&
                (act.start - runEnd) < (int64_t)(n1Us * 0.5)) {
                runEnd = std::max(runEnd, act.end);
                runInt += act.sumIntensity;
                continue;
            }
            flushRun();
            runStart = act.start;
            runEnd = std::max(act.start, act.end);
            runState = st;
            runInt = act.sumIntensity;
        }
        flushRun();

        disp += blockLen;
    }

    tl.dispStart = 0;
    tl.dispEnd = disp;
    return tl;
}

// ============================================================
//  便捷入口
// ============================================================
bool AnalyzeDay(const std::string& day, Analysis& out, const Config& cfg) {
    DayEvents de;
    if (!LoadDay(day, de, false) || de.records.empty()) { out = Analysis(); return false; }
    // 连续事件合并只做一次：阈值推导、块切分、马尔可夫序列全部基于合并后的粒度
    std::vector<Op> ops = MergeOps(RecordsToOps(de.records));
    if (ops.empty()) { out = Analysis(); return false; }
    Thresholds th = DeriveThresholds(ops, cfg);
    out = Analyze(std::move(ops), th);
    return !out.empty;
}

bool AnalyzeRange(int64_t fromUs, int64_t toUs, Analysis& out, const Config& cfg) {
    std::vector<Op> all;
    for (const auto& day : ListDays()) {
        DayEvents de;
        if (!LoadDay(day, de, false)) continue;
        // 用当天首尾时间快速跳过与范围完全不相交的文件
        if (!de.records.empty()) {
            if (de.records.back().tUs < fromUs || de.records.front().tUs > toUs) continue;
        }
        std::vector<RawRecord> sub;
        sub.reserve(de.records.size());
        for (const auto& r : de.records)
            if (r.tUs >= fromUs && r.tUs <= toUs) sub.push_back(r);
        if (sub.empty()) continue;
        std::vector<Op> ops = RecordsToOps(sub);
        all.insert(all.end(), ops.begin(), ops.end());
    }
    if (all.empty()) { out = Analysis(); return false; }
    std::sort(all.begin(), all.end(), [](const Op& x, const Op& y) { return x.t < y.t; });
    // 跨天汇总后统一合并一次，避免漏掉跨天边界的相邻事件
    all = MergeOps(std::move(all));
    Thresholds th = DeriveThresholds(all, cfg);
    out = Analyze(std::move(all), th);
    return !out.empty;
}

std::vector<Op> LoadAllOps(size_t maxDays) {
    std::vector<Op> all;
    std::vector<std::string> days = ListDays();
    if (maxDays > 0 && days.size() > maxDays)
        days.erase(days.begin(), days.begin() + (days.size() - maxDays));
    for (const auto& day : days) {
        DayEvents de;
        if (!LoadDay(day, de, false)) continue;
        std::vector<Op> ops = RecordsToOps(de.records);
        all.insert(all.end(), ops.begin(), ops.end());
    }
    std::sort(all.begin(), all.end(), [](const Op& x, const Op& y) { return x.t < y.t; });
    // 跨天汇总后统一合并一次（本函数是「全部历史」口径的唯一入口，
    // 与 AnalyzeDay / AnalyzeRange 互不调用，因此不会重复合并）
    return MergeOps(std::move(all));
}

} // namespace pb
