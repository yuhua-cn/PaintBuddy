// ============================================================
//  深浅分类多信号探针（诊断工具，不进产品）
//
//  上一轮已测：撤回率(undo/全部操作)、状态熵、转移熵、平均连长。
//  结论：状态熵 CV=0.06 无区分力；avgRunLen 与 medianGap 相关
//  r=+0.992 完全冗余；撤回率是唯一独立信号(r=+0.055)。
//
//  本轮补测「时间轴离散度」—— 即用户说的「更随机更杂乱」，
//  上一轮只测了操作类型混杂度，没测节奏离散度。候选信号：
//    gapCV    块内间隔的变异系数 sd/mean（节奏忽快忽慢）
//    iqrRatio 块内间隔 p75/p25（同上的稳健版）
//    r1       撤回/画笔（用户要求的口径，替代上轮的 撤回/全部）
//    burstFrac 撤回处于「连击簇」(>=2 次相邻)中的比例
//    maxBurst 最长撤回连击长度
//    brushRate 画笔占比（上一轮发现的语义守卫）
//
//  决定性检验：加入这些信号后，有没有块的深浅标签真的改变？
//  一块都不变 → 这些信号在本数据上无实际作用，不该实现。
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

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    if (v.size() == 1) return v[0];
    double idx = p * (double)(v.size() - 1);
    size_t lo = (size_t)std::floor(idx), hi = (size_t)std::ceil(idx);
    if (lo == hi) return v[lo];
    return v[lo] + (v[hi] - v[lo]) * (idx - (double)lo);
}
static double Mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = 0; for (double x : v) m += x; return m / (double)v.size();
}
static double Sd(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double m = Mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / (double)(v.size() - 1));
}
static double Corr(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.size() < 3) return 0.0;
    double ma = Mean(a), mb = Mean(b), sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sxy += (a[i] - ma) * (b[i] - mb);
        sxx += (a[i] - ma) * (a[i] - ma);
        syy += (b[i] - mb) * (b[i] - mb);
    }
    return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
}

// ------------------------------------------------------------
struct BSig {
    double durSec = 0, medianGap = 0, gapCV = 0, iqrRatio = 0;
    // 干净口径：只统计「运笔节奏内」的间隔（< 5s），排除几十秒的长停顿。
    // 原 gapCV 含长停顿，会被离群值主导（实测 medGap=0.377 的块 gapCV=6.56，
    // 该数值在数学上只能由极端离群值产生），测的不是节奏杂乱度。
    double rhyCV = 0, rhyP90P50 = 0;
    int    rhyN = 0;
    double r1 = 0, brushRate = 0, burstFrac = 0;
    int nUndo = 0, nBrush = 0, nOps = 0, maxBurst = 0;
    // 输入设备来源：mouse 笔画在 RecordsToOps 里 kind=Normal（不是 Brush），
    // 所以「零画笔」不等于「没在画」——纯鼠标作画的用户会全块 brushRate=0。
    int nSrcKb = 0, nSrcMouse = 0, nSrcPen = 0;
    bool hasBrush = true;
    bool deepNow = false;   // 现有逻辑的判定
};

static BSig ComputeBlock(const std::vector<Op>& ops, int64_t bs, int64_t be) {
    BSig s;
    if (ops.empty()) return s;
    s.durSec = (double)(be - bs) / 1e6;
    s.nOps = (int)ops.size();

    std::vector<double> gaps;
    int64_t prevEnd = ops[0].end;
    for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == OpKind::Brush) ++s.nBrush;
        if (ops[i].kind == OpKind::Undo || ops[i].kind == OpKind::Redo) ++s.nUndo;
        // 设备来源：0=键盘 1=鼠标 2=数位笔
        if (ops[i].src == 0)      ++s.nSrcKb;
        else if (ops[i].src == 1) ++s.nSrcMouse;
        else                      ++s.nSrcPen;
        if (i > 0) {
            int64_t g = ops[i].t - prevEnd;
            if (g > 0) gaps.push_back((double)g / 1e6);
            prevEnd = std::max(prevEnd, ops[i].end);
        }
    }
    s.medianGap = Pct(gaps, 0.5);
    s.hasBrush = s.nBrush > 0;
    if (s.hasBrush) {
        s.r1 = (double)s.nUndo / (double)s.nBrush;
        s.brushRate = (double)s.nBrush / (double)ops.size();
    } else {
        s.brushRate = 0.0;
    }
    // 时间轴离散度（原口径：含长停顿，会被离群值主导）
    double mg = Mean(gaps);
    s.gapCV = (mg > 0) ? Sd(gaps) / mg : 0.0;
    double p25 = Pct(gaps, 0.25), p75 = Pct(gaps, 0.75);
    s.iqrRatio = (p25 > 1e-9) ? p75 / p25 : 0.0;

    // 干净口径：只取 <5s 的间隔（运笔节奏内的空档），排除长停顿
    std::vector<double> rhy;
    rhy.reserve(gaps.size());
    for (double g : gaps) if (g < 5.0) rhy.push_back(g);
    s.rhyN = (int)rhy.size();
    if (s.rhyN >= 5) {
        double rm = Mean(rhy);
        s.rhyCV = (rm > 0) ? Sd(rhy) / rm : 0.0;
        double r50 = Pct(rhy, 0.5), r90 = Pct(rhy, 0.90);
        s.rhyP90P50 = (r50 > 1e-9) ? r90 / r50 : 0.0;
    }

    // 撤回连击簇：相邻（间隔 < 2s）的撤回/重做算同一簇
    s.maxBurst = 0;
    int inBurst = 0;
    for (size_t i = 0; i < ops.size(); ++i) {
        bool isU = (ops[i].kind == OpKind::Undo || ops[i].kind == OpKind::Redo);
        if (isU) {
            bool adjacent = (i > 0) &&
                (ops[i].t - ops[i - 1].end < 2000000LL) &&
                (ops[i - 1].kind == OpKind::Undo || ops[i - 1].kind == OpKind::Redo);
            if (adjacent) { ++inBurst; }
            else { if (inBurst > 0) s.maxBurst = std::max(s.maxBurst, inBurst); inBurst = 1; }
        } else {
            if (inBurst > 0) s.maxBurst = std::max(s.maxBurst, inBurst);
            inBurst = 0;
        }
    }
    if (inBurst > 0) s.maxBurst = std::max(s.maxBurst, inBurst);
    // burstFrac：处于长度 >=2 的簇中的撤回占比
    if (s.nUndo > 0) {
        int inCluster = 0, run = 0;
        std::vector<int> runs;
        for (size_t i = 0; i < ops.size(); ++i) {
            bool isU = (ops[i].kind == OpKind::Undo || ops[i].kind == OpKind::Redo);
            if (isU) ++run;
            else { if (run > 0) runs.push_back(run); run = 0; }
        }
        if (run > 0) runs.push_back(run);
        for (int r : runs) if (r >= 2) inCluster += r;
        s.burstFrac = (double)inCluster / (double)s.nUndo;
    }
    return s;
}

static const int NS = 9;
static double Get(const BSig& s, int i) {
    switch (i) {
    case 0: return s.medianGap;
    case 1: return s.gapCV;
    case 2: return s.iqrRatio;
    case 3: return s.r1;
    case 4: return s.brushRate;
    case 5: return s.burstFrac;
    case 6: return (double)s.maxBurst;
    case 7: return s.rhyCV;
    default: return s.rhyP90P50;
    }
}
static const char* Name(int i) {
    static const char* n[NS] = {"medianGap ", "gapCV     ", "iqrRatio  ", "r1 撤/画笔",
                                "brushRate ", "burstFrac ", "maxBurst  ", "rhyCV(净) ",
                                "rhyP90/50 "};
    return n[i];
}

// ------------------------------------------------------------
static void RunFile(const char* label, const std::string& path) {
    printf("\n################ %s ################\n", label);
    DayEvents de;
    if (!LoadFileDirect(path, de)) { printf("[FAIL] 无法加载 %s\n", path.c_str()); return; }

    std::vector<Op> merged = MergeOps(RecordsToOps(de.records));
    Config cfg;
    Thresholds th = DeriveThresholds(merged, cfg);
    Analysis a = Analyze(merged, th);
    printf("n1=%.1fs n2=%.1fs mergeSec=%.2fs depthGap=%.3fs | 块=%d（深 %d / 浅 %d）\n",
           th.n1Sec, th.n2Sec, th.mergeSec, a.th.depthGapSec,
           a.blockCount, a.deepBlocks, a.shallowBlocks);

    std::vector<BSig> bs;
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const PaintBlock& b = a.blocks[bi];
        std::vector<Op> slice;
        for (const auto& o : a.ops) if (o.t >= b.start && o.t <= b.end) slice.push_back(o);
        BSig s = ComputeBlock(slice, b.start, b.end);
        s.deepNow = b.deep;
        bs.push_back(s);
    }

    printf("\n---- 块级信号 ----\n");
    printf("  %-3s %7s %6s %5s %5s %8s %8s %8s %8s %8s %8s %6s %5s %5s %5s %5s\n",
           "块", "时长s", "ops", "撤回", "画笔", "medGap",
           "gapCV污", "rhyCV净", "rhy90/50", "iqr比",
           "r1撤/笔", "最长簇", "deep", "键盘", "鼠标", "数位笔");
    for (size_t i = 0; i < bs.size(); ++i) {
        const BSig& s = bs[i];
        char r1s[20];
        if (s.hasBrush) snprintf(r1s, sizeof(r1s), "%8.3f", s.r1);
        else            snprintf(r1s, sizeof(r1s), "%8s", "无画笔");
        printf("  %-3zu %7.0f %6d %5d %5d %8.4f %8.3f %8.3f %8.2f %8.2f %s %6d %5s %5d %5d %5d\n",
               i, s.durSec, s.nOps, s.nUndo, s.nBrush, s.medianGap,
               s.gapCV, s.rhyCV, s.rhyP90P50, s.iqrRatio,
               r1s, s.maxBurst, s.deepNow ? "是" : "否",
               s.nSrcKb, s.nSrcMouse, s.nSrcPen);
    }

    // 纯鼠标作画检测：零画笔但鼠标操作占比高 → 规则A 会误杀
    printf("\n---- 规则A 误杀检查（纯鼠标作画的用户）----\n");
    for (size_t i = 0; i < bs.size(); ++i) {
        const BSig& s = bs[i];
        if (s.nBrush != 0 || s.nOps < 20) continue;
        double mouseFrac = (double)s.nSrcMouse / (double)s.nOps;
        double kbFrac    = (double)s.nSrcKb / (double)s.nOps;
        printf("  块%zu: 撤回%d 键盘%d(%.0f%%) 鼠标%d(%.0f%%) 数位笔%d → %s\n",
               i, s.nUndo, s.nSrcKb, 100.0 * kbFrac, s.nSrcMouse, 100.0 * mouseFrac,
               s.nSrcPen,
               mouseFrac > 0.3 ? "!! 鼠标占比高，规则A 会误杀真实作画"
                               : "纯键盘/修正活动，规则A 降级正确");
    }

    // 零画笔块的鼠标操作细分：移动 vs 真实笔画
    // 关键：RecordsToOps 里鼠标笔画('M')的 kind 是 Normal 而非 Brush，
    // 所以 brushRate=0 不代表没在画。必须按 note 区分才能判定规则A 是否安全。
    printf("\n---- 零画笔块的鼠标操作细分（按 note）----\n");
    bool anyZeroBrush = false;
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        if (bs[bi].nBrush != 0 || bs[bi].nOps < 20) continue;
        anyZeroBrush = true;
        int mv = 0, stroke = 0, click = 0, other = 0;
        for (const auto& o : a.ops) {
            if (o.t < a.blocks[bi].start || o.t > a.blocks[bi].end) continue;
            if (o.src != 1) continue;
            if (o.note.find("移动") != std::string::npos)      ++mv;
            else if (o.note.find("笔画") != std::string::npos) ++stroke;
            else if (o.note.find("按键") != std::string::npos) ++click;
            else                                               ++other;
        }
        printf("  块%zu: 鼠标移动 %d  鼠标笔画 %d  鼠标按键 %d  其他 %d\n", bi, mv, stroke, click, other);
        printf("     → %s\n",
               stroke > 0 ? "含真实鼠标笔画：规则A 会误杀，需改用设备无关的判据"
                          : "无鼠标笔画（纯移动/键盘）：规则A 降级是安全的");
    }
    if (!anyZeroBrush) printf("  无「零画笔且 ops>=20」的块\n");

    // 区分力：块间 CV
    printf("\n---- 区分力：块间 CV（越大越有区分力；<0.15 视为无区分力）----\n");
    for (int f = 0; f < NS; ++f) {
        std::vector<double> v;
        for (const auto& s : bs) v.push_back(Get(s, f));
        double m = Mean(v), sd = Sd(v);
        printf("  %s mean=%9.4f  sd=%9.4f  CV=%6.3f%s\n",
               Name(f), m, sd, m > 0 ? sd / m : 0.0,
               (m > 0 && sd / m < 0.15) ? "   ← 几乎无区分力" : "");
    }

    // 独立性：与 medianGap 的相关
    if (bs.size() >= 3) {
        printf("\n---- 独立性：与 medianGap 的 Pearson r（|r|>0.8 = 冗余）----\n");
        std::vector<double> x; for (const auto& s : bs) x.push_back(s.medianGap);
        for (int f = 1; f < NS; ++f) {
            std::vector<double> y; for (const auto& s : bs) y.push_back(Get(s, f));
            double r = Corr(x, y);
            printf("  %-11s r = %+0.3f%s\n", Name(f), r,
                   std::fabs(r) > 0.8 ? "   ← 冗余" : (std::fabs(r) < 0.4 ? "   ← 独立" : ""));
        }
    } else {
        printf("\n  块数 %zu < 3，相关性无统计意义（跳过）\n", bs.size());
    }

    // ---- 决定性检验：标签会变吗 ----
    // 产品侧实际值（paintCount 是设备无关的正确分母）
    printf("\n---- 产品侧实际值（Analyze 内部计算）----\n");
    printf("  method = %s\n", a.th.method.c_str());
    printf("  %-3s %6s %6s %10s %6s\n", "块", "paint", "undo", "undoPerPaint", "deep");
    std::vector<double> prodRates;
    for (size_t i = 0; i < a.blocks.size(); ++i) {
        const PaintBlock& b = a.blocks[i];
        printf("  %-3zu %6d %6d %10.4f %6s\n",
               i, b.paintCount, b.undoCount, b.undoPerPaint, b.deep ? "是" : "否");
        if (b.paintCount > 0) prodRates.push_back(b.undoPerPaint);
    }
    if (prodRates.size() >= 3) {
        double m = Pct(prodRates, 0.5);
        double mx = *std::max_element(prodRates.begin(), prodRates.end());
        printf("  有效块 %zu 个，中位 = %.4f，1.3x 阈值 = %.4f，最高块 = %.4f\n",
               prodRates.size(), m, m * 1.3, mx);
        printf("  → 撤回率降级规则会改变 %d 块%s\n",
               mx >= m * 1.3 ? 1 : 0,
               mx >= m * 1.3 ? "" : "（最高值未达阈值，规则不触发）");
    } else {
        printf("  有效块仅 %zu 个（<3）→ 中位数无基准意义\n", prodRates.size());
    }

    printf("\n---- 决定性检验：候选规则会改变哪些块的标签 ----\n");

    // 规则 A：语义守卫（无画笔 + 足量操作 → 不可能是深度绘画）
    printf("  规则A 语义守卫: brushRate < 0.02 且 ops >= 20 → 判浅度\n");
    int changedA = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        bool hit = (bs[i].brushRate < 0.02 && bs[i].nOps >= 20);
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) {
            ++changedA;
            printf("     块%zu: 深→浅  (撤回 %d, 画笔 %d, medGap %.4f)\n",
                   i, bs[i].nUndo, bs[i].nBrush, bs[i].medianGap);
        }
    }
    printf("     改变 %d 块\n", changedA);

    // 规则 B：r1 高于当天中位数 x1.3 → 降级
    double medR1 = Pct([&]{ std::vector<double> v; for (const auto& s : bs) if (s.hasBrush) v.push_back(s.r1); return v; }(), 0.5);
    printf("  规则B 撤回率降级: r1 >= 当天中位(%.3f) x 1.3 = %.3f → 判浅度\n", medR1, medR1 * 1.3);
    int changedB = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        if (!bs[i].hasBrush) continue;
        bool hit = bs[i].r1 >= medR1 * 1.3;
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) {
            ++changedB;
            printf("     块%zu: 深→浅  (r1 %.3f, medGap %.4f)\n", i, bs[i].r1, bs[i].medianGap);
        }
    }
    printf("     改变 %d 块\n", changedB);

    // 规则 C：时间轴离散度 —— gapCV 高 → 降级
    double medCV = Pct([&]{ std::vector<double> v; for (const auto& s : bs) v.push_back(s.gapCV); return v; }(), 0.5);
    printf("  规则C 节奏离散降级: gapCV >= 当天中位(%.3f) x 1.3 = %.3f → 判浅度\n", medCV, medCV * 1.3);
    int changedC = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        bool hit = bs[i].gapCV >= medCV * 1.3;
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) {
            ++changedC;
            printf("     块%zu: 深→浅  (gapCV %.3f, medGap %.4f)\n", i, bs[i].gapCV, bs[i].medianGap);
        }
    }
    printf("     改变 %d 块\n", changedC);

    // 规则 D：撤回连击簇 —— 最长簇 >= 4 → 降级
    printf("  规则D 撤回连击降级: 最长簇 >= 4 → 判浅度\n");
    int changedD = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        bool hit = bs[i].maxBurst >= 4;
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) {
            ++changedD;
            printf("     块%zu: 深→浅  (最长簇 %d, medGap %.4f)\n", i, bs[i].maxBurst, bs[i].medianGap);
        }
    }
    printf("     改变 %d 块\n", changedD);

    // 规则 E：设备无关撤回率 —— 错误分母 vs 产品侧正确分母的对照
    //
    // 【重要】这里刻意保留两种算法并列输出，用于记录一次真实的误判教训：
    //   错误分母：在「已合并」的 ops 上用 note.find("笔画") 数鼠标笔画。
    //             MergeOps 合并后 note 只保留组内首个操作的，被吸收的鼠标
    //             笔画数不出来 → 分母系统性少一半（实测 136 vs 真实 285），
    //             撤回率虚高一倍（0.897 vs 真实 0.428）。
    //   正确分母：产品侧 paintCount，在 RecordsToOps 阶段赋值、MergeOps 时累加。
    //
    // 正是这个错误分母让某块看起来像「撤回率 1.9 倍于中位」的离群值，
    // 从而误判为「密集但反复修正」。用正确分母重算后该块撤回率与中位持平。
    printf("  规则E 撤回率对照（错误分母 note匹配 vs 产品侧 paintCount）\n");
    printf("     %-4s %10s %10s %8s %s\n", "块", "错误r1'", "正确r1'", "paint", "结论");
    std::vector<double> r1Wrong, r1Right;
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        if (bs[bi].nOps < 20) continue;
        int paintWrong = bs[bi].nBrush;
        for (const auto& o : a.ops) {
            if (o.t < a.blocks[bi].start || o.t > a.blocks[bi].end) continue;
            if (o.src == 1 && o.note.find("笔画") != std::string::npos) ++paintWrong;
        }
        double rw = paintWrong > 0 ? (double)bs[bi].nUndo / (double)paintWrong : -1.0;
        double rr = a.blocks[bi].paintCount > 0 ? a.blocks[bi].undoPerPaint : -1.0;
        r1Wrong.push_back(rw); r1Right.push_back(rr);
        printf("     %-4zu %10s %10.4f %8d %s\n", bi,
               rw < 0 ? "无绘画" : "", rw < 0 ? 0.0 : rw,
               a.blocks[bi].paintCount,
               (rw >= 0 && rr >= 0 && std::fabs(rw - rr) > 0.05)
                 ? "!! 错误分母显著虚高" : "");
    }
    std::vector<double> rrValid, rwValid;
    for (double v : r1Right) if (v >= 0) rrValid.push_back(v);
    for (double v : r1Wrong) if (v >= 0) rwValid.push_back(v);
    double medR = Pct(rrValid, 0.5), medW = Pct(rwValid, 0.5);
    double maxR = rrValid.empty() ? 0.0 : *std::max_element(rrValid.begin(), rrValid.end());
    double maxW = rwValid.empty() ? 0.0 : *std::max_element(rwValid.begin(), rwValid.end());
    printf("     错误分母: 中位=%.3f  1.3x=%.3f  最高=%.3f  → %s\n",
           medW, medW * 1.3, maxW,
           maxW >= medW * 1.3 ? "会触发降级（伪信号）" : "不触发");
    printf("     正确分母: 中位=%.3f  1.3x=%.3f  最高=%.3f  → %s\n",
           medR, medR * 1.3, maxR,
           maxR >= medR * 1.3 ? "会触发降级" : "不触发（无离群块，阈值无从校准）");

    // 规则 F：干净节奏离散度 rhyCV（只含 <5s 的运笔节奏间隔）
    // 与规则C 对照：C 用的 gapCV 含几十秒长停顿，被离群值主导；
    // F 排除长停顿，测的才是「运笔节奏忽快忽慢」。
    printf("  规则F 节奏杂乱降级: rhyCV(净) >= 当天中位 x 1.3 → 判浅度\n");
    std::vector<double> rv;
    for (const auto& s : bs) if (s.rhyN >= 5) rv.push_back(s.rhyCV);
    double medRhy = Pct(rv, 0.5);
    printf("     当天中位 = %.3f，阈值 = %.3f（有效块 %zu 个）\n", medRhy, medRhy * 1.3, rv.size());
    int changedF = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        if (bs[i].rhyN < 5) continue;
        bool hit = bs[i].rhyCV >= medRhy * 1.3;
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) {
            ++changedF;
            printf("     块%zu: 深→浅  (rhyCV %.3f, medGap %.4f)\n", i, bs[i].rhyCV, bs[i].medianGap);
        }
    }
    printf("     改变 %d 块\n", changedF);

    // 组合 A+B
    printf("  组合 A+B（守卫 + 撤回率降级）:\n");
    int chAB = 0, deepAB = 0;
    for (size_t i = 0; i < bs.size(); ++i) {
        bool hit = (bs[i].brushRate < 0.02 && bs[i].nOps >= 20) ||
                   (bs[i].hasBrush && medR1 > 0 && bs[i].r1 >= medR1 * 1.3);
        bool newDeep = hit ? false : bs[i].deepNow;
        if (newDeep != bs[i].deepNow) ++chAB;
        if (newDeep) ++deepAB;
    }
    printf("     改变 %d 块 → 深 %d / 浅 %d（原 深 %d / 浅 %d）\n",
           chAB, deepAB, (int)bs.size() - deepAB, a.deepBlocks, a.shallowBlocks);
}

int main(int argc, char** argv) {
    struct Arg { const char* label; const char* path; };
    std::vector<Arg> args;
    for (int i = 1; i + 1 <= argc - 1; i += 2) args.push_back({argv[i], argv[i + 1]});
    if (args.empty()) {
        args.push_back({"数据1 5.8MB", "C:\\Users\\WINDOWS\\AppData\\Local\\PaintingBuddy\\logs\\20261018.jsonl"});
        args.push_back({"数据2 37MB",  "D:\\qqdownload\\logs\\20261017.jsonl"});
        args.push_back({"数据3 14MB",  "D:\\qqdownload\\logs\\20261018.jsonl"});
    }
    for (const auto& x : args) RunFile(x.label, x.path);
    printf("\ndone\n");
    return 0;
}
