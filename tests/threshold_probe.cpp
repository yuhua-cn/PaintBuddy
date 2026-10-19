// ============================================================
//  阈值退化探针（诊断工具，不进产品）
//
//  目的：用项目自身的解析/合并/推导代码，跑用户提供的真实日志，
//  回答「n1、n2 为什么会塌缩一个数量级」。
//
//  只读分析，不写任何数据文件。
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Config.h"
#include "Analyzer.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace pb;

// ---- 本地复刻 LargestLogJump（原函数是 static，这里要多返回诊断信息）----
struct JumpInfo {
    bool   ok = false;
    double val = 0.0;      // 跳变点（几何均值）
    double ratio = 0.0;    // 倍率
    size_t lo = 0, hi = 0; // 跳变发生在 sorted 的哪两个相邻元素之间
    size_t nLeft = 0, nRight = 0;  // 候选点两侧在搜索区间内的样本数
};

static JumpInfo FindJump(const std::vector<double>& sorted, double lo, double hi) {
    JumpInfo j;
    size_t i0 = (size_t)(std::lower_bound(sorted.begin(), sorted.end(), lo) - sorted.begin());
    size_t i1 = (size_t)(std::upper_bound(sorted.begin(), sorted.end(), hi) - sorted.begin());
    if (i1 < i0 + 2) return j;
    double best = 0.0, bestVal = 0.0;
    size_t bIdx = 0; bool found = false;
    for (size_t i = i0; i + 1 < i1; ++i) {
        if (sorted[i] <= 0.0) continue;
        double r = sorted[i + 1] / sorted[i];
        if (r > best) { best = r; bestVal = std::sqrt(sorted[i] * sorted[i + 1]); bIdx = i; found = true; }
    }
    if (!found) return j;
    j.ok = true; j.val = bestVal; j.ratio = best; j.lo = bIdx; j.hi = bIdx + 1;
    j.nLeft  = bIdx - i0 + 1;      // 含 sorted[bIdx]
    j.nRight = i1 - (bIdx + 1);    // sorted[bIdx+1] 起
    return j;
}

static double Pct(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted[0];
    double idx = p * (double)(sorted.size() - 1);
    size_t lo = (size_t)std::floor(idx);
    size_t hi = (size_t)std::ceil(idx);
    if (lo == hi) return sorted[lo];
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (idx - (double)lo);
}

// 复刻 DeriveThresholds 的间隔收集口径（running max of end）
static std::vector<double> CollectGaps(const std::vector<Op>& ops) {
    std::vector<double> gaps;
    if (ops.size() < 2) return gaps;
    gaps.reserve(ops.size());
    int64_t prevEnd = ops[0].end;
    for (size_t i = 1; i < ops.size(); ++i) {
        int64_t g = ops[i].t - prevEnd;
        if (g > 0) gaps.push_back((double)g / 1000000.0);
        prevEnd = std::max(prevEnd, ops[i].end);
    }
    return gaps;
}

static void DumpPool(const char* label, std::vector<double> gaps) {
    if (gaps.empty()) { printf("  %s: 空\n", label); return; }
    std::sort(gaps.begin(), gaps.end());
    size_t n = gaps.size();
    printf("  %s: n=%zu\n", label, n);
    printf("      最小=%.4f  最大=%.1f\n", gaps.front(), gaps.back());
    const double ps[] = {0.10, 0.25, 0.50, 0.75, 0.90, 0.95, 0.99};
    for (double p : ps) printf("      p%-4.0f = %10.4f s\n", p * 100, Pct(gaps, p));
    // 分桶：看样本到底堆在哪个量级
    const double edges[] = {0.1, 0.3, 0.6, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0};
    printf("      分桶（占比）:");
    size_t idx = 0;
    for (size_t e = 0; e < sizeof(edges) / sizeof(edges[0]); ++e) {
        size_t c = 0;
        while (idx < n && gaps[idx] < edges[e]) { ++c; ++idx; }
        printf(" <%g:%.1f%%", edges[e], 100.0 * (double)c / (double)n);
    }
    size_t rest = n - idx;
    printf(" >=%g:%.1f%%\n", edges[sizeof(edges) / sizeof(edges[0]) - 1], 100.0 * (double)rest / (double)n);
}

int main(int argc, char** argv) {
    const std::string day = (argc > 1) ? argv[1] : "20261018";

    EnsureDirs();
    printf("日志目录: %s\n", LogDir().c_str());

    DayEvents de;
    if (!LoadDay(day, de, false)) {
        printf("[FAIL] 无法加载 %s\n", day.c_str());
        return 1;
    }
    printf("加载 %s: %zu 条原始记录\n\n", day.c_str(), de.records.size());

    Config cfg;   // 全默认 → n1/n2/depth 都是 0 → 走自动推导
    printf("==== 1. 生产管线原样复现 ====\n");
    std::vector<Op> opsRaw = RecordsToOps(de.records);
    std::vector<Op> ops = MergeOps(opsRaw);
    printf("  原始记录 %zu → Op %zu → 合并后 Op %zu\n",
           de.records.size(), opsRaw.size(), ops.size());
    printf("  合并窗口 = %.0f ms\n", (double)EVENT_MERGE_WINDOW_US / 1000.0);

    Thresholds th = DeriveThresholds(ops, cfg);
    printf("  推导结果: n1=%.3fs  n2=%.1fs  depthGap=%.3fs\n",
           th.n1Sec, th.n2Sec, th.depthGapSec);
    printf("  method: %s\n", th.method.c_str());
    printf("  gapSamples=%d\n\n", th.gapSamples);

    Analysis a = Analyze(ops, th);
    printf("  分析结果: 块=%d  深度块=%d  浅度块=%d\n",
           a.blockCount, a.deepBlocks, a.shallowBlocks);
    printf("            普通间隔=%d  暂停=%d  打断=%d\n",
           a.normalGapCount, a.pauseCount, a.interruptCount);
    printf("  → 阈值塌缩时，暂停与打断应当异常偏多\n\n");

    printf("==== 2. 间隔样本池构成 ====\n");
    std::vector<double> gapsAll = CollectGaps(ops);
    DumpPool("合并后全部间隔", gapsAll);

    // 按 kind/src 拆分：哪些操作贡献了微小间隔
    size_t brushCnt = 0, kbCnt = 0, mouseCnt = 0, penCnt = 0;
    for (const auto& o : ops) {
        if (o.kind == OpKind::Brush) ++brushCnt;
        if (o.src == 0) ++kbCnt; else if (o.src == 1) ++mouseCnt; else ++penCnt;
    }
    printf("\n  合并后 Op 构成: 总数=%zu  画笔kind=%zu | src 键盘=%zu 鼠标=%zu 数位笔=%zu\n",
           ops.size(), brushCnt, kbCnt, mouseCnt, penCnt);
    printf("  → 画笔/撤回/重做是 MergeOps 里的「独立成组」，其间隔不会被合并掉\n\n");

    // 只保留「两侧都不是画笔 kind」的间隔
    std::vector<double> gapsNoBrush;
    std::vector<double> gapsBrushOnly;
    if (ops.size() >= 2) {
        int64_t prevEnd = ops[0].end;
        for (size_t i = 1; i < ops.size(); ++i) {
            int64_t g = ops[i].t - prevEnd;
            if (g > 0) {
                double sec = (double)g / 1000000.0;
                bool standalone = (ops[i].kind == OpKind::Brush ||
                                   ops[i].kind == OpKind::Undo ||
                                   ops[i].kind == OpKind::Redo);
                if (standalone) gapsBrushOnly.push_back(sec);
                else            gapsNoBrush.push_back(sec);
            }
            prevEnd = std::max(prevEnd, ops[i].end);
        }
    }
    DumpPool("非独立事件间隔", gapsNoBrush);
    DumpPool("独立事件(画笔/修正)间隔", gapsBrushOnly);
    if (!gapsAll.empty()) {
        printf("\n  独立事件间隔占全池 %.1f%%\n",
               100.0 * (double)gapsBrushOnly.size() / (double)gapsAll.size());
    }

    printf("\n==== 3. 跳变检测到底选中了什么 ====\n");
    std::vector<double> gs = gapsAll;
    std::sort(gs.begin(), gs.end());
    JumpInfo j2 = FindJump(gs, 30.0, 86400.0);
    if (j2.ok) {
        printf("  n2 搜索区间 [30, 86400]: 跳变点=%.2fs  倍率=%.2f  (门槛 ratio>=2.0 → %s)\n",
               j2.val, j2.ratio, j2.ratio >= 2.0 ? "命中" : "未命中");
        printf("     位于 sorted[%zu]=%.4f → sorted[%zu]=%.4f\n",
               j2.lo, gs[j2.lo], j2.hi, gs[j2.hi]);
        printf("     区间内左侧样本=%zu  右侧样本=%zu\n", j2.nLeft, j2.nRight);
        double clamped = std::min(3600.0, std::max(60.0, j2.val));
        printf("     夹取后 n2=%.1fs%s\n", clamped,
               clamped != j2.val ? "  ← 撞到 60s 地板" : "");
    } else {
        printf("  n2 搜索区间 [30, 86400]: 区间内不足 2 个样本 → 走 95 百分位\n");
    }
    printf("  p95 = %.2fs（若无跳变则 n2 = max(60, p95) = %.1fs）\n\n",
           Pct(gs, 0.95), std::min(3600.0, std::max(60.0, Pct(gs, 0.95))));

    double n2Used = th.n2Sec;
    JumpInfo j1 = FindJump(gs, 0.02, n2Used * 0.8);
    if (j1.ok) {
        printf("  n1 搜索区间 [0.02, %.1f]: 跳变点=%.4fs  倍率=%.2f  (门槛 ratio>=1.8 → %s)\n",
               n2Used * 0.8, j1.val, j1.ratio, j1.ratio >= 1.8 ? "命中" : "未命中");
        printf("     位于 sorted[%zu]=%.4f → sorted[%zu]=%.4f\n",
               j1.lo, gs[j1.lo], j1.hi, gs[j1.hi]);
        printf("     区间内左侧样本=%zu  右侧样本=%zu\n", j1.nLeft, j1.nRight);
        printf("     左样本占全池 %.2f%%（无任何最小样本量约束）\n",
               100.0 * (double)j1.nLeft / (double)gs.size());
        double clamped = std::min(60.0, std::max(0.3, j1.val));
        printf("     夹取后 n1=%.4fs%s\n", clamped,
               clamped != j1.val ? "  ← 撞到 0.3s 地板" : "");
    } else {
        printf("  n1 搜索区间: 无有效跳变\n");
    }
    printf("  p75 = %.4fs（若无跳变则 n1 = max(0.3, p75) = %.4fs）\n\n",
           Pct(gs, 0.75), std::min(60.0, std::max(0.3, Pct(gs, 0.75))));

    printf("==== 4. 尺度不变保护为何没拦住 ====\n");
    printf("  现有保护: n2 >= n1*3  →  %.1f >= %.3f*3 = %.3f  → %s\n",
           th.n2Sec, th.n1Sec, th.n1Sec * 3.0,
           th.n2Sec >= th.n1Sec * 3.0 ? "通过（未触发）" : "触发");
    printf("  默认值应为 n1=3.0 / n2=300；实测 n1=%.3f / n2=%.1f\n",
           th.n1Sec, th.n2Sec);
    printf("  n1 缩水 %.1f 倍，n2 缩水 %.1f 倍 → 等比塌缩，相对保护完全无感\n\n",
           3.0 / std::max(th.n1Sec, 1e-9), 300.0 / std::max(th.n2Sec, 1e-9));

    printf("==== 5. 若剔除合并窗口内的间隔（候选修复口径）====\n");
    std::vector<double> gapsFiltered;
    for (double g : gapsAll) if (g >= 1.0) gapsFiltered.push_back(g);   // EVENT_MERGE_WINDOW_US = 1s
    DumpPool("间隔 >= 1.0s", gapsFiltered);
    if (!gapsFiltered.empty()) {
        std::vector<double> gf = gapsFiltered;
        std::sort(gf.begin(), gf.end());
        JumpInfo f2 = FindJump(gf, 30.0, 86400.0);
        double n2f = (f2.ok && f2.ratio >= 2.0)
                   ? std::min(3600.0, std::max(60.0, f2.val))
                   : std::min(3600.0, std::max(60.0, Pct(gf, 0.95)));
        JumpInfo f1 = FindJump(gf, 0.02, n2f * 0.8);
        double n1f = (f1.ok && f1.ratio >= 1.8)
                   ? std::min(60.0, std::max(0.3, f1.val))
                   : std::min(60.0, std::max(0.3, Pct(gf, 0.75)));
        printf("  → 仅剔除 <1s 间隔后: n1=%.3fs  n2=%.1fs\n", n1f, n2f);
        printf("    跳变命中: n2 %s(%.2f倍)  n1 %s(%.2f倍)\n",
               (f2.ok && f2.ratio >= 2.0) ? "是" : "否", f2.ratio,
               (f1.ok && f1.ratio >= 1.8) ? "是" : "否", f1.ratio);
    }
    printf("\n==== 6. 若加绝对语义下限（候选修复口径）====\n");
    printf("  n1 = max(%.3f, 1.0) = %.3fs\n", th.n1Sec, std::max(th.n1Sec, 1.0));
    printf("  n2 = max(%.1f, 300) = %.1fs\n", th.n2Sec, std::max(th.n2Sec, 300.0));
    printf("  该口径下暂停/打断的判定门槛回到人类尺度\n");

    printf("\n==== 7. 暂停爆表的量化 ====\n");
    size_t pauseN = 0, pauseUnder2 = 0, pauseUnder5 = 0, pauseUnder30 = 0;
    for (const auto& g : a.gaps) {
        if (g.type != GapType::Pause) continue;
        ++pauseN;
        double sec = (double)(g.end - g.start) / 1e6;
        if (sec < 2.0)  ++pauseUnder2;
        if (sec < 5.0)  ++pauseUnder5;
        if (sec < 30.0) ++pauseUnder30;
    }
    printf("  实际判为暂停的间隔 = %zu 条\n", pauseN);
    if (pauseN) {
        printf("    其中 <2s  = %zu 条（%.1f%%）← 落笔之间的正常凝视\n",
               pauseUnder2, 100.0 * (double)pauseUnder2 / (double)pauseN);
        printf("    其中 <5s  = %zu 条（%.1f%%）\n",
               pauseUnder5, 100.0 * (double)pauseUnder5 / (double)pauseN);
        printf("    其中 <30s = %zu 条（%.1f%%）\n",
               pauseUnder30, 100.0 * (double)pauseUnder30 / (double)pauseN);
    }
    // 若 n1 回到 3.0s，这些暂停有多少会退回「普通间隔」
    size_t wouldBeNormal = 0;
    for (const auto& g : a.gaps) {
        if (g.type != GapType::Pause) continue;
        double sec = (double)(g.end - g.start) / 1e6;
        if (sec < 3.0 && sec < th.n2Sec) ++wouldBeNormal;
    }
    printf("  → 若 n1 = 3.0s（默认值），其中 %zu 条会退回「普通间隔」\n", wouldBeNormal);
    printf("    暂停数将从 %zu 降到 %zu（减少 %.1f%%）\n",
           pauseN, pauseN - wouldBeNormal,
           pauseN ? 100.0 * (double)wouldBeNormal / (double)pauseN : 0.0);

    printf("\n==== 8. n2 对单个离群样本的依赖度 ====\n");
    printf("  当前 n2 走的是「跳变」分支（ratio=2.49，门槛 2.0）\n");
    if (gs.size() >= 3) {
        std::vector<double> gsDrop = gs;
        gsDrop.pop_back();   // 去掉最大的那个间隔（431.5s）
        JumpInfo jd = FindJump(gsDrop, 30.0, 86400.0);
        printf("  反事实：若删掉最大的 1 个间隔样本（%.1fs），\n", gs.back());
        if (jd.ok && jd.ratio >= 2.0) {
            printf("     跳变仍命中: %.1fs (%.2f倍)\n", jd.val, jd.ratio);
        } else {
            printf("     跳变%s（ratio=%.2f）→ 退回 95 百分位分支\n",
                   jd.ok ? "未达门槛" : "无有效样本", jd.ratio);
            double p95d = Pct(gsDrop, 0.95);
            printf("     p95=%.2fs → n2 = max(60, %.2f) = %.1fs  ← 撞地板\n",
                   p95d, p95d, std::min(3600.0, std::max(60.0, p95d)));
        }
    }
    printf("  全池 p95 仅 %.2fs（被笔触微间隔压低）→ 百分位分支必然产出 60s\n", Pct(gs, 0.95));
    printf("  结论：n2 是否塌到 60s，取决于当天有没有出现一次 >=2 倍的最大跳变\n");

    printf("\n==== 9. 深度分界的真实来源（验证「跟着 n1 走」）====\n");
    printf("  DeriveThresholds 阶段 depthGap=%.3fs（默认，未推导）\n", th.depthGapSec);
    printf("  Analyze 阶段重算后 depthGap=%.3fs  blockSamples=%d\n",
           a.th.depthGapSec, a.th.blockSamples);
    printf("  各块中位间隔:\n");
    size_t bi = 0;
    for (const auto& b : a.blocks) {
        printf("     块%zu: 时长=%.1fs  opCount=%d  medianGap=%.4fs  deep=%s\n",
               bi++, (double)(b.end - b.start) / 1e6, b.opCount, b.medianGapSec,
               b.deep ? "是" : "否");
    }
    printf("  → 块的中位间隔集合 curGaps 同时收 Normal 与 Pause（n1 只决定二者归类，\n");
    printf("    不决定谁进集合）；真正决定集合的是 n2（切块）。故 depthGap 主要跟 n2 走。\n");
    printf("  → 但块数过少（%d 块）时 meds.size()<2，推导跳过、沿用默认 2.0s。\n", a.blockCount);

    printf("\n==== 10. 对照：同一份数据用默认阈值重跑 ====\n");
    {
        size_t ge60 = 0, ge120 = 0, ge300 = 0;
        for (double g : gs) { if (g >= 60.0) ++ge60; if (g >= 120.0) ++ge120; if (g >= 300.0) ++ge300; }
        printf("  间隔 >=60s 的样本数 = %zu（若 n2=60 → 打断约 %zu 次）\n", ge60, ge60);
        printf("  间隔 >=120s = %zu，>=300s = %zu\n", ge120, ge300);

        Thresholds def;                 // n1=3.0 / n2=300 / depthGap=2.0
        def.n1Auto = def.n2Auto = def.depthAuto = false;
        Analysis b = Analyze(ops, def);   // 注意：Analyze 不重算 depth（Auto=false）
        printf("\n  默认阈值 n1=%.1f n2=%.1f depthGap=%.1f:\n",
               def.n1Sec, def.n2Sec, def.depthGapSec);
        printf("     块=%d  深度=%d  浅度=%d\n", b.blockCount, b.deepBlocks, b.shallowBlocks);
        printf("     普通间隔=%d  暂停=%d  打断=%d\n",
               b.normalGapCount, b.pauseCount, b.interruptCount);
        printf("     depthGap 重算后=%.3fs\n", b.th.depthGapSec);
        printf("\n  对比（自动推导 vs 默认）:\n");
        printf("     n1      %.3fs → %.1fs\n", th.n1Sec, def.n1Sec);
        printf("     n2      %.1fs → %.1fs\n", th.n2Sec, def.n2Sec);
        printf("     暂停    %d → %d（%+.1f%%）\n", a.pauseCount, b.pauseCount,
               a.pauseCount ? 100.0 * ((double)b.pauseCount - (double)a.pauseCount) / (double)a.pauseCount : 0.0);
        printf("     打断    %d → %d\n", a.interruptCount, b.interruptCount);
        printf("     块数    %d → %d\n", a.blockCount, b.blockCount);
    }

    printf("\ndone\n");
    return 0;
}
