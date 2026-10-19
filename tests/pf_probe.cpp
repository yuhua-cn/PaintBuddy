// ============================================================
//  pf 方案评估探针（诊断工具，不进产品）
//
//  问题：引入内部参数 pf，把间隔 < pf 的连续运笔（排线等）
//  合并为一次计数。pf 用常量 0.9s，还是计算得出？
//
//  本探针做三件事：
//   1. 在真实数据上，把若干 pf 候选值代入完整管线，
//      输出 n1 / n2 / depthGap / 暂停 / 打断，看哪个能修好。
//   2. 用 4 种不同绘画节奏的合成数据检验泛化性——
//      重点看「计算型 pf」会不会把本来正常的用户也改了。
//   3. 检验 pf 是否引入循环依赖（pf 由间隔算出，又用来筛间隔）。
//
//  只读分析，不写任何数据文件；合成数据用固定种子，可复现。
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
#include <random>

using namespace pb;

// ------------------------------------------------------------
//  统计小工具
// ------------------------------------------------------------
static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    if (v.size() == 1) return v[0];
    double idx = p * (double)(v.size() - 1);
    size_t lo = (size_t)std::floor(idx);
    size_t hi = (size_t)std::ceil(idx);
    if (lo == hi) return v[lo];
    return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}
static double Med(std::vector<double> v) { return Pct(v, 0.5); }
static double MAD(std::vector<double> v) {
    if (v.empty()) return 0.0;
    double m = Med(v);
    std::vector<double> d; d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - m));
    return Med(d);
}

// ------------------------------------------------------------
//  间隔收集（与 DeriveThresholds 完全同口径：running max of end）
// ------------------------------------------------------------
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

// ------------------------------------------------------------
//  带样本量约束的谷检测（修正版 LargestLogJump）
//  要求候选点两侧各占搜索区间 >= minSideFrac，且倍率达门槛
// ------------------------------------------------------------
static bool ValleyGuarded(const std::vector<double>& sorted, double lo, double hi,
                          double minSideFrac, double minRatio, double& out) {
    out = 0.0;
    if (sorted.size() < 20) return false;
    size_t i0 = (size_t)(std::lower_bound(sorted.begin(), sorted.end(), lo) - sorted.begin());
    size_t i1 = (size_t)(std::upper_bound(sorted.begin(), sorted.end(), hi) - sorted.begin());
    if (i1 < i0 + 20) return false;
    size_t span = i1 - i0;
    size_t minSide = (size_t)(minSideFrac * (double)span);
    double best = 0.0; size_t bIdx = 0; bool found = false;
    for (size_t i = i0 + minSide; i + 1 + minSide < i1; ++i) {
        if (sorted[i] <= 0.0) continue;
        double r = sorted[i + 1] / sorted[i];
        if (r > best) { best = r; bIdx = i; found = true; }
    }
    if (!found || best < minRatio) return false;
    out = std::sqrt(sorted[bIdx] * sorted[bIdx + 1]);
    return true;
}

// ------------------------------------------------------------
//  复刻生产版 LargestLogJump（无样本量守卫），用于对照
// ------------------------------------------------------------
static bool LargestLogJumpProd(const std::vector<double>& sorted, double lo, double hi,
                               double& outVal, double& outRatio) {
    outVal = 0.0; outRatio = 0.0;
    size_t i0 = (size_t)(std::lower_bound(sorted.begin(), sorted.end(), lo) - sorted.begin());
    size_t i1 = (size_t)(std::upper_bound(sorted.begin(), sorted.end(), hi) - sorted.begin());
    if (i1 < i0 + 2) return false;
    for (size_t i = i0; i + 1 < i1; ++i) {
        if (sorted[i] <= 0.0) continue;
        double r = sorted[i + 1] / sorted[i];
        if (r > outRatio) { outRatio = r; outVal = std::sqrt(sorted[i] * sorted[i + 1]); }
    }
    return outRatio > 0.0;
}

// ------------------------------------------------------------
//  候选修复·过滤法：完全复刻 DeriveThresholds，
//  只在收集 gaps 时跳过 < filterSec 的样本
// ------------------------------------------------------------
static Thresholds DeriveFiltered(const std::vector<Op>& ops, double filterSec) {
    Thresholds th;
    std::vector<double> gaps;
    if (ops.size() >= 2) {
        int64_t prevEnd = ops[0].end;
        for (size_t i = 1; i < ops.size(); ++i) {
            int64_t g = ops[i].t - prevEnd;
            if (g > 0) {
                double sec = (double)g / 1e6;
                if (sec >= filterSec) gaps.push_back(sec);
            }
            prevEnd = std::max(prevEnd, ops[i].end);
        }
    }
    if (gaps.empty()) { th.method = "过滤后无样本，沿用默认"; return th; }
    std::sort(gaps.begin(), gaps.end());
    th.gapSamples = (int)gaps.size();
    double v = 0, ratio = 0;
    if (LargestLogJumpProd(gaps, 30.0, 86400.0, v, ratio) && ratio >= 2.0)
        th.n2Sec = std::min(3600.0, std::max(60.0, v));
    else
        th.n2Sec = std::min(3600.0, std::max(60.0, Pct(gaps, 0.95)));
    if (LargestLogJumpProd(gaps, 0.02, th.n2Sec * 0.8, v, ratio) && ratio >= 1.8)
        th.n1Sec = std::min(60.0, std::max(0.3, v));
    else
        th.n1Sec = std::min(60.0, std::max(0.3, Pct(gaps, 0.75)));
    if (th.n2Sec < th.n1Sec * 3.0) { th.n2Sec = th.n1Sec * 3.0; if (th.n2Sec < 60.0) th.n2Sec = 60.0; }
    return th;
}

// ------------------------------------------------------------
//  pf 候选估算器
// ------------------------------------------------------------
struct PfCandidate {
    const char* name;
    const char* kind;   // "常量" / "计算"
    double pf;
};

static std::vector<PfCandidate> BuildCandidates(const std::vector<double>& strokeDur,
                                                std::vector<double> gaps,
                                                double mergeWindowSec) {
    std::vector<double> sg = gaps;
    std::sort(sg.begin(), sg.end());

    std::vector<PfCandidate> c;
    c.push_back({"0.9s 固定",              "常量", 0.9});
    c.push_back({"= 合并窗口 (1.0s)",      "常量", mergeWindowSec});
    c.push_back({"p50(间隔)",              "计算", Med(gaps)});
    c.push_back({"p75(间隔)",              "计算", Pct(gaps, 0.75)});
    c.push_back({"2.0 x p50(笔画时长)",    "计算", 2.0 * Med(strokeDur)});
    c.push_back({"3.0 x p50(笔画时长)",    "计算", 3.0 * Med(strokeDur)});
    c.push_back({"p50(间隔) + 3*MAD",      "计算", Med(gaps) + 3.0 * MAD(gaps)});

    double v = 0.0;
    if (ValleyGuarded(sg, 0.1, 10.0, 0.10, 1.8, v))
        c.push_back({"谷检测[0.1,10]带守卫", "计算", v});
    else
        c.push_back({"谷检测[0.1,10]带守卫", "计算", -1.0});   // -1 = 未检出
    return c;
}

// ------------------------------------------------------------
//  按 pf 把连续运笔聚合成「作画段落」
//  variant 0: 只聚合画笔 kind（保守，键盘时序原样保留）
//  variant 1: 聚合除撤回/重做外的全部操作
// ------------------------------------------------------------
static std::vector<Op> MergePassages(const std::vector<Op>& ops, double pfSec, int variant) {
    if (pfSec <= 0.0 || ops.size() < 2) return ops;
    int64_t pfUs = (int64_t)(pfSec * 1e6);

    auto isUndoRedo = [](const Op& o) {
        return o.kind == OpKind::Undo || o.kind == OpKind::Redo;
    };
    auto eligible = [&](const Op& o) {
        if (isUndoRedo(o)) return false;
        if (variant == 0) return o.kind == OpKind::Brush;
        return true;
    };

    std::vector<Op> out;
    out.reserve(ops.size());
    size_t i = 0;
    while (i < ops.size()) {
        if (!eligible(ops[i])) { out.push_back(ops[i]); ++i; continue; }
        Op acc = ops[i];
        acc.merged = std::max(1, ops[i].merged);
        size_t brushN = (ops[i].kind == OpKind::Brush) ? 1 : 0;
        size_t j = i + 1;
        while (j < ops.size() && eligible(ops[j]) && (ops[j].t - acc.end) < pfUs) {
            acc.end = std::max(acc.end, ops[j].end);
            acc.intensity += ops[j].intensity;
            acc.merged += std::max(1, ops[j].merged);
            if (ops[j].kind == OpKind::Brush) ++brushN;
            ++j;
        }
        // 段落以画笔为主 → 记为画笔（保持「一笔一计数」的语义上界）
        if (brushN * 2 >= (j - i)) acc.kind = OpKind::Brush;
        out.push_back(std::move(acc));
        i = j;
    }
    return out;
}

// ------------------------------------------------------------
//  跑完整管线
// ------------------------------------------------------------
struct Res {
    int    nOps = 0, gapSamples = 0;
    double n1 = 0, n2 = 0, depth = 0;
    int    blocks = 0, normalGaps = 0, pauses = 0, interrupts = 0;
    int    deep = 0, shallow = 0;
};
static Res Run(const std::vector<Op>& ops) {
    Res r;
    r.nOps = (int)ops.size();
    Config cfg;                                  // 全默认 → 自动推导
    Thresholds th = DeriveThresholds(ops, cfg);
    Analysis a = Analyze(ops, th);
    r.n1 = th.n1Sec; r.n2 = th.n2Sec; r.depth = a.th.depthGapSec;
    r.gapSamples = th.gapSamples;
    r.blocks = a.blockCount; r.normalGaps = a.normalGapCount;
    r.pauses = a.pauseCount; r.interrupts = a.interruptCount;
    r.deep = a.deepBlocks; r.shallow = a.shallowBlocks;
    return r;
}

// ------------------------------------------------------------
//  合成数据：不同绘画节奏
// ------------------------------------------------------------
struct Profile {
    const char* name;
    double durMed;      // 笔画时长中位数
    double gapMed;      // 笔间间隔中位数
    double pauseFrac;   // 长停顿占比
    double pauseMed;    // 长停顿中位数
    int    n;           // 笔画数
};

static std::vector<Op> Synth(const Profile& p, int64_t seed) {
    std::mt19937_64 rng((uint64_t)seed);
    auto ln = [&](double median, double sigma) {
        std::lognormal_distribution<double> d(std::log(median), sigma);
        return std::max(0.001, d(rng));
    };
    std::vector<Op> ops;
    ops.reserve(p.n);
    int64_t t = 1792286283373180LL;
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int i = 0; i < p.n; ++i) {
        double dur = ln(p.durMed, 0.55);
        Op o;
        o.t = t;
        o.end = t + (int64_t)(dur * 1e6);
        o.kind = OpKind::Brush;
        o.src = 2;
        o.intensity = (float)(dur * 900.0);
        ops.push_back(o);
        t = o.end;
        double g = (u01(rng) < p.pauseFrac) ? ln(p.pauseMed, 0.8) : ln(p.gapMed, 0.6);
        t += (int64_t)(g * 1e6);
    }
    return ops;
}

static std::vector<double> Durations(const std::vector<Op>& ops) {
    std::vector<double> d;
    d.reserve(ops.size());
    for (const auto& o : ops) d.push_back((double)(o.end - o.t) / 1e6);
    return d;
}

// ------------------------------------------------------------
//  直接按文件路径加载日志（不依赖 LogDir，避免创建目录副作用）
// ------------------------------------------------------------
static bool LoadFileDirect(const std::string& path, DayEvents& out) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;
    out.records.clear();
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        if (end > i) {
            std::string line = text.substr(i, end - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                RawRecord r;
                if (ParseEventLine(line, r) && r.tUs > 0) out.records.push_back(std::move(r));
            }
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    std::sort(out.records.begin(), out.records.end(),
              [](const RawRecord& a, const RawRecord& b) { return a.tUs < b.tUs; });
    return true;
}

// ============================================================
int main(int argc, char** argv) {
    const std::string day = (argc > 1) ? argv[1] : "20261018";
    const std::string filePath = (argc > 2) ? argv[2] : std::string();
    const double MW = (double)EVENT_MERGE_WINDOW_US / 1e6;

    DayEvents de;
    bool ok;
    if (!filePath.empty()) {
        printf("日志文件: %s\n", filePath.c_str());
        ok = LoadFileDirect(filePath, de);
    } else {
        EnsureDirs();
        printf("日志目录: %s\n", LogDir().c_str());
        ok = LoadDay(day, de, false);
    }
    if (!ok) { printf("[FAIL] 无法加载 %s\n", day.c_str()); return 1; }

    std::vector<Op> rawOps = RecordsToOps(de.records);
    std::vector<Op> merged = MergeOps(rawOps);

    // 笔画时长：取原始笔记录
    std::vector<double> strokeDur;
    for (const auto& r : de.records)
        if (r.tUs > 0 && r.type == 'p' && !r.hover)
            strokeDur.push_back((double)(r.endUs - r.tUs) / 1e6);

    std::vector<double> gapsMerged = CollectGaps(merged);

    printf("==== 数据：%s ====\n", day.c_str());
    printf("  原始记录 %zu → Op %zu → 合并后 Op %zu（合并窗口 %.1fs）\n",
           de.records.size(), rawOps.size(), merged.size(), MW);
    printf("  笔画 %zu 条，时长 p50=%.3fs p75=%.3fs\n",
           strokeDur.size(), Med(strokeDur), Pct(strokeDur, .75));
    printf("  间隔池 n=%zu，p50=%.3fs p75=%.3fs p95=%.3fs\n\n",
           gapsMerged.size(), Med(gapsMerged), Pct(gapsMerged, .75), Pct(gapsMerged, .95));

    // ---------- 基线 ----------
    printf("==== 1. 基线（当前生产行为，无 pf）====\n");
    Res base = Run(merged);
    printf("  Op=%d  gapSamples=%d\n", base.nOps, base.gapSamples);
    printf("  n1=%.3fs  n2=%.1fs  depthGap=%.3fs\n", base.n1, base.n2, base.depth);
    printf("  块=%d  普通间隔=%d  暂停=%d  打断=%d\n",
           base.blocks, base.normalGaps, base.pauses, base.interrupts);
    printf("  深度块=%d  浅度块=%d（depthGap=%.3fs）\n",
           base.deep, base.shallow, base.depth);
    for (const auto& b : Analyze(merged, DeriveThresholds(merged, Config())).blocks)
        printf("     块 时长=%.0fs opCount=%d medianGap=%.4fs deep=%s\n",
               (double)(b.end - b.start) / 1e6, b.opCount, b.medianGapSec,
               b.deep ? "是" : "否");
    printf("\n");

    // ---------- pf 候选值 ----------
    std::vector<PfCandidate> cands = BuildCandidates(strokeDur, gapsMerged, MW);
    printf("==== 2. pf 候选值（由本份数据算出）====\n");
    for (const auto& c : cands) {
        if (c.pf < 0) printf("  %-26s [%s]  未检出有效谷\n", c.name, c.kind);
        else          printf("  %-26s [%s]  pf = %.3fs\n", c.name, c.kind, c.pf);
    }

    // ---------- 各 pf 代入完整管线 ----------
    printf("\n==== 3. 各 pf 代入管线的结果（variant 0：只聚合画笔）====\n");
    printf("  %-26s %8s %7s %8s %9s %6s %6s %6s %6s\n",
           "pf 方案", "Op数", "n1", "n2", "depthGap", "块", "普通", "暂停", "打断");
    printf("  %-26s %8d %7.3f %8.1f %9.3f %6d %6d %6d %6d\n",
           "（基线，无 pf）", base.nOps, base.n1, base.n2, base.depth,
           base.blocks, base.normalGaps, base.pauses, base.interrupts);
    for (const auto& c : cands) {
        if (c.pf <= 0) continue;
        std::vector<Op> psg = MergePassages(merged, c.pf, 0);
        Res r = Run(psg);
        printf("  %-26s %8d %7.3f %8.1f %9.3f %6d %6d %6d %6d\n",
               c.name, r.nOps, r.n1, r.n2, r.depth,
               r.blocks, r.normalGaps, r.pauses, r.interrupts);
    }

    printf("\n==== 4. 同上（variant 1：聚合除撤回/重做外全部操作）====\n");
    printf("  %-26s %8s %7s %8s %9s %6s %6s %6s %6s\n",
           "pf 方案", "Op数", "n1", "n2", "depthGap", "块", "普通", "暂停", "打断");
    for (const auto& c : cands) {
        if (c.pf <= 0) continue;
        std::vector<Op> psg = MergePassages(merged, c.pf, 1);
        Res r = Run(psg);
        printf("  %-26s %8d %7.3f %8.1f %9.3f %6d %6d %6d %6d\n",
               c.name, r.nOps, r.n1, r.n2, r.depth,
               r.blocks, r.normalGaps, r.pauses, r.interrupts);
    }

    // 参照：默认阈值
    {
        Thresholds def; def.n1Auto = def.n2Auto = def.depthAuto = false;
        Analysis b = Analyze(merged, def);
        printf("\n  参照·固定默认阈值 n1=3.0 n2=300 depth=2.0:\n");
        printf("     块=%d  普通间隔=%d  暂停=%d  打断=%d\n",
               b.blockCount, b.normalGapCount, b.pauseCount, b.interruptCount);
    }

    // ---------- 过滤法 vs pf 合并法 ----------
    printf("\n==== 4b. 两种修复思路对照 ====\n");
    printf("  过滤法 = 推导阈值时剔除 <1s 的间隔（不改变界面计数）\n");
    printf("  pf法   = 把 <pf 的连续运笔聚合成段落（改变计数）\n\n");
    {
        Thresholds f = DeriveFiltered(merged, 1.0);
        Analysis fa = Analyze(merged, f);
        printf("  %-22s gapSamples=%-6d n1=%.3f  n2=%.1f  depthGap=%.3f\n",
               "过滤法 (剔除<1s)", f.gapSamples, f.n1Sec, f.n2Sec, fa.th.depthGapSec);
        printf("  %-22s 块=%d  普通=%d  暂停=%d  打断=%d\n", "",
               fa.blockCount, fa.normalGapCount, fa.pauseCount, fa.interruptCount);

        std::vector<Op> psg = MergePassages(merged, MW, 1);
        Thresholds pf = DeriveFiltered(psg, 0.0);   // 不过滤，看 pf 本身的效果
        Analysis pfa = Analyze(psg, pf);
        printf("  %-22s gapSamples=%-6d n1=%.3f  n2=%.1f  depthGap=%.3f\n",
               "pf法 (pf=1.0s)", pf.gapSamples, pf.n1Sec, pf.n2Sec, pfa.th.depthGapSec);
        printf("  %-22s 块=%d  普通=%d  暂停=%d  打断=%d\n", "",
               pfa.blockCount, pfa.normalGapCount, pfa.pauseCount, pfa.interruptCount);

        // 两者叠加
        Thresholds both = DeriveFiltered(psg, 1.0);
        Analysis ba = Analyze(psg, both);
        printf("  %-22s gapSamples=%-6d n1=%.3f  n2=%.1f  depthGap=%.3f\n",
               "两者叠加", both.gapSamples, both.n1Sec, both.n2Sec, ba.th.depthGapSec);
        printf("  %-22s 块=%d  普通=%d  暂停=%d  打断=%d\n", "",
               ba.blockCount, ba.normalGapCount, ba.pauseCount, ba.interruptCount);

        // 间隔池对比
        std::vector<double> kept;
        for (double g : gapsMerged) if (g >= 1.0) kept.push_back(g);
        printf("\n  间隔池: 全池 n=%zu (p50=%.3f p75=%.3f p95=%.3f)\n",
               gapsMerged.size(), Med(gapsMerged), Pct(gapsMerged, .75), Pct(gapsMerged, .95));
        printf("          筛后 n=%zu (p50=%.3f p75=%.3f p95=%.3f)  ← 过滤法用的就是这个\n",
               kept.size(), Med(kept), Pct(kept, .75), Pct(kept, .95));
        printf("          <1s 占比 = %.1f%%\n",
               gapsMerged.empty() ? 0.0
                 : 100.0 * (double)(gapsMerged.size() - kept.size()) / (double)gapsMerged.size());
    }

    // ---------- 泛化性：不同节奏 ----------
    printf("\n==== 5. 泛化性检验（合成数据，4 种绘画节奏）====\n");
    printf("  关注：本来正常的节奏，会不会被 pf 改坏\n\n");
    const Profile profiles[] = {
        {"A 极快排线（类本次真实数据）", 0.26, 0.55, 0.03,  90.0, 1500},
        {"B 常规线稿",                  0.60, 1.50, 0.03, 120.0, 1200},
        {"C 慢速厚涂",                  2.00, 5.00, 0.04, 180.0,  800},
        {"D 机械抖动/点击",             0.05, 0.08, 0.02,  60.0, 2000},
    };
    for (size_t pi = 0; pi < sizeof(profiles) / sizeof(profiles[0]); ++pi) {
        const Profile& p = profiles[pi];
        std::vector<Op> syn = Synth(p, 20261018 + (int64_t)pi);
        std::vector<Op> synMerged = MergeOps(syn);
        std::vector<double> sd = Durations(syn);
        std::vector<double> sg = CollectGaps(synMerged);

        printf("  ---- %s ----\n", p.name);
        printf("     笔画 %zu 条，时长 p50=%.3fs；间隔 n=%zu p50=%.3fs p75=%.3fs p95=%.3fs\n",
               syn.size(), Med(sd), sg.size(), Med(sg), Pct(sg, .75), Pct(sg, .95));

        Res rb = Run(synMerged);
        printf("     基线(无 pf)      : Op=%d  n1=%.3f  n2=%.1f  暂停=%d  打断=%d\n",
               rb.nOps, rb.n1, rb.n2, rb.pauses, rb.interrupts);

        std::vector<PfCandidate> cc = BuildCandidates(sd, sg, MW);
        for (const auto& c : cc) {
            if (c.pf <= 0) { printf("     %-22s: 谷未检出\n", c.name); continue; }
            std::vector<Op> psg = MergePassages(synMerged, c.pf, 1);
            Res r = Run(psg);
            printf("     %-22s: pf=%.3f → Op=%d n1=%.3f n2=%.1f 暂停=%d 打断=%d\n",
                   c.name, c.pf, r.nOps, r.n1, r.n2, r.pauses, r.interrupts);
        }
        // 固定 1.0s 作对照
        {
            std::vector<Op> psg = MergePassages(synMerged, MW, 1);
            Res r = Run(psg);
            printf("     %-22s: pf=%.3f → Op=%d n1=%.3f n2=%.1f 暂停=%d 打断=%d\n",
                   "[对照] 常量 1.0s", MW, r.nOps, r.n1, r.n2, r.pauses, r.interrupts);
        }
        // 段落被吃掉的比例
        {
            std::vector<Op> psg = MergePassages(synMerged, MW, 1);
            printf("     常量1.0s 使 Op 从 %d 降到 %d（压缩 %.1f%%）\n\n",
                   rb.nOps, (int)psg.size(),
                   rb.nOps ? 100.0 * (1.0 - (double)psg.size() / (double)rb.nOps) : 0.0);
        }
    }

    // ---------- 循环依赖检验 ----------
    printf("==== 6. 循环依赖 / 稳定性检验 ====\n");
    {
        std::vector<double> g = gapsMerged;
        double p75 = Pct(g, 0.75);
        std::vector<double> kept;
        for (double x : g) if (x >= p75) kept.push_back(x);
        printf("  若 pf = p75(间隔) = %.4fs，则筛后池 = 原池最高的 25%%\n", p75);
        printf("     筛后 p75 = %.4fs ≡ 原池 p93.75 = %.4fs\n",
               Pct(kept, 0.75), Pct(g, 0.9375));
        printf("  → 「pf=p75 再取 p75」等价于固定取原池 p93.75，是伪自适应\n");
        printf("     （数值随用户节奏等比缩放，但不反映真实停顿结构）\n");

        // 迭代是否收敛
        double pf = p75;
        printf("  迭代稳定性（每轮用上轮筛后池的 p75 作 pf）:\n");
        std::vector<double> cur = g;
        for (int it = 1; it <= 5; ++it) {
            double np = Pct(cur, 0.75);
            std::vector<double> nx;
            for (double x : cur) if (x >= np) nx.push_back(x);
            printf("     第%d轮 pf=%.4f  池 n=%zu → %zu\n", it, np, cur.size(), nx.size());
            if (nx.empty()) break;
            cur = nx; pf = np;
        }
        printf("  → 池会一路收缩（每轮只剩 25%%），说明 pf 必须由外部锚定，不能自迭代\n");
    }

    printf("\ndone\n");
    return 0;
}
