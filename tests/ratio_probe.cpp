// ============================================================
//  撤回/画笔比率的多尺度探针（诊断工具，不进产品）
//
//  回答两个问题：
//   1. 撤回信号在哪个时间尺度上才有区分力？
//      （块级？分钟级？10 秒级？）
//   2. 用什么分母才对？
//      r1 = 撤回 / 画笔
//      r2 = (普通 + 撤回) / 画笔
//      r3 = 撤回 / 全部操作   ← 上一轮实际用的口径，列出作对照
//
//  窗口采用不重叠滚动（tumbling），保证样本相互独立，
//  CV 与相关性统计才有意义。
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
    double ma = Mean(a), mb = Mean(b);
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sxy += (a[i] - ma) * (b[i] - mb);
        sxx += (a[i] - ma) * (a[i] - ma);
        syy += (b[i] - mb) * (b[i] - mb);
    }
    return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : 0.0;
}

// ------------------------------------------------------------
struct WinStat {
    int nUndo = 0, nRedo = 0, nBrush = 0, nNormal = 0, nTotal = 0;
    double medGap = 0;
    double r1 = 0, r2 = 0, r3 = 0;   // 撤回/画笔, (普通+撤回)/画笔, 撤回/全部
    bool hasBrush = false;           // r1/r2 是否可定义
};

static WinStat ComputeWindow(const std::vector<Op>& ops) {
    WinStat s;
    s.nTotal = (int)ops.size();
    for (const auto& o : ops) {
        switch (o.kind) {
        case OpKind::Undo:  ++s.nUndo;  break;
        case OpKind::Redo:  ++s.nRedo;  break;
        case OpKind::Brush: ++s.nBrush; break;
        default:            ++s.nNormal; break;
        }
    }
    std::vector<double> gaps;
    int64_t prevEnd = ops[0].end;
    for (size_t i = 1; i < ops.size(); ++i) {
        int64_t g = ops[i].t - prevEnd;
        if (g > 0) gaps.push_back((double)g / 1e6);
        prevEnd = std::max(prevEnd, ops[i].end);
    }
    s.medGap = Pct(gaps, 0.5);
    s.hasBrush = s.nBrush > 0;
    if (s.hasBrush) {
        s.r1 = (double)s.nUndo / (double)s.nBrush;
        s.r2 = (double)(s.nNormal + s.nUndo) / (double)s.nBrush;
    }
    if (s.nTotal > 0) s.r3 = (double)s.nUndo / (double)s.nTotal;
    return s;
}

// ------------------------------------------------------------
//  滚动窗口扫描
// ------------------------------------------------------------
static std::vector<WinStat> Tumble(const std::vector<Op>& all, double winSec, size_t minOps) {
    std::vector<WinStat> out;
    if (all.empty() || winSec <= 0) return out;
    int64_t winUs = (int64_t)(winSec * 1e6);
    int64_t t0 = all.front().t, t1 = all.back().end;
    size_t idx = 0;
    for (int64_t s = t0; s < t1; s += winUs) {
        int64_t e = s + winUs;
        while (idx < all.size() && all[idx].t < s) ++idx;
        std::vector<Op> slice;
        size_t j = idx;
        while (j < all.size() && all[j].t < e) { slice.push_back(all[j]); ++j; }
        if (slice.size() >= minOps) out.push_back(ComputeWindow(slice));
    }
    return out;
}

static void ReportScale(const std::vector<Op>& all, double winSec, const char* tag) {
    std::vector<WinStat> ws = Tumble(all, winSec, 3);
    if (ws.empty()) { printf("  %-22s 无有效窗口\n", tag); return; }

    std::vector<double> r1, r2, r3, mg, mgValid, r1v, r2v;
    int noBrush = 0, noBrushWithUndo = 0, opsCovered = 0;
    for (const auto& s : ws) {
        opsCovered += s.nTotal;
        r3.push_back(s.r3);
        if (!s.hasBrush) {
            ++noBrush;
            if (s.nUndo > 0) ++noBrushWithUndo;
        } else {
            r1.push_back(s.r1); r2.push_back(s.r2);
            mg.push_back(s.medGap);
            r1v.push_back(s.r1); r2v.push_back(s.r2); mgValid.push_back(s.medGap);
        }
    }
    printf("\n  ── 窗口 %s ──\n", tag);
    printf("     有效窗口 %zu（覆盖 %d 个操作）；无画笔窗口 %zu 个",
           ws.size(), opsCovered, (size_t)noBrush);
    if (noBrush > 0)
        printf("（其中 %d 个含撤回 → 纯修正/键盘活动，r1、r2 无定义）", noBrushWithUndo);
    printf("\n");

    if (!r1.empty()) {
        double m1 = Mean(r1), s1 = Sd(r1);
        printf("     r1 撤回/画笔        : n=%-4zu mean=%7.3f  sd=%7.3f  CV=%6.3f | p25=%7.3f p50=%7.3f p75=%7.3f p90=%7.3f max=%7.2f\n",
               r1.size(), m1, s1, m1 > 0 ? s1 / m1 : 0.0,
               Pct(r1, .25), Pct(r1, .5), Pct(r1, .75), Pct(r1, .9),
               *std::max_element(r1.begin(), r1.end()));
    }
    if (!r2.empty()) {
        double m2 = Mean(r2), s2 = Sd(r2);
        printf("     r2 (普通+撤回)/画笔 : n=%-4zu mean=%7.3f  sd=%7.3f  CV=%6.3f | p25=%7.3f p50=%7.3f p75=%7.3f p90=%7.3f max=%7.2f\n",
               r2.size(), m2, s2, m2 > 0 ? s2 / m2 : 0.0,
               Pct(r2, .25), Pct(r2, .5), Pct(r2, .75), Pct(r2, .9),
               *std::max_element(r2.begin(), r2.end()));
    }
    if (!r3.empty()) {
        double m3 = Mean(r3), s3 = Sd(r3);
        printf("     r3 撤回/全部(旧口径): n=%-4zu mean=%7.3f  sd=%7.3f  CV=%6.3f | p25=%7.3f p50=%7.3f p75=%7.3f p90=%7.3f max=%7.2f\n",
               r3.size(), m3, s3, m3 > 0 ? s3 / m3 : 0.0,
               Pct(r3, .25), Pct(r3, .5), Pct(r3, .75), Pct(r3, .9),
               *std::max_element(r3.begin(), r3.end()));
    }
    if (mgValid.size() >= 3) {
        printf("     与 medGap 相关性    : r1 %+0.3f   r2 %+0.3f   （|r|<0.4 = 独立信号）\n",
               Corr(r1v, mgValid), Corr(r2v, mgValid));
    }
}

// ------------------------------------------------------------
static void RunFile(const char* label, const std::string& path) {
    printf("\n######################## %s ########################\n", label);
    DayEvents de;
    if (!LoadFileDirect(path, de)) { printf("[FAIL] 无法加载 %s\n", path.c_str()); return; }

    std::vector<Op> all = MergeOps(RecordsToOps(de.records));
    Config cfg;
    Thresholds th = DeriveThresholds(all, cfg);
    Analysis a = Analyze(all, th);
    printf("记录 %zu → Op %zu | n1=%.3f n2=%.1f depthGap=%.3f | 块=%d（深 %d / 浅 %d）\n",
           de.records.size(), all.size(), th.n1Sec, th.n2Sec, a.th.depthGapSec,
           a.blockCount, a.deepBlocks, a.shallowBlocks);

    // 全天口径的操作构成
    int tU = 0, tR = 0, tB = 0, tN = 0;
    for (const auto& o : all) {
        if (o.kind == OpKind::Undo) ++tU;
        else if (o.kind == OpKind::Redo) ++tR;
        else if (o.kind == OpKind::Brush) ++tB;
        else ++tN;
    }
    printf("全天构成: 撤回 %d  重做 %d  画笔 %d  普通 %d  合计 %zu\n",
           tU, tR, tB, tN, all.size());
    if (tB > 0) {
        printf("全天比率: r1 撤回/画笔 = %.3f    r2 (普通+撤回)/画笔 = %.3f    r3 撤回/全部 = %.3f\n",
               (double)tU / (double)tB,
               (double)(tN + tU) / (double)tB,
               (double)tU / (double)all.size());
    }

    // ---- 块级（生产分类的实际单位）----
    printf("\n---- 块级比率（现有深浅分类的实际单位）----\n");
    printf("  %-3s %7s %6s %5s %5s %6s %7s %9s %9s %9s %8s %5s\n",
           "块", "时长s", "ops", "撤回", "画笔", "普通", "medGap",
           "r1撤/笔", "r2(普+撤)/笔", "r3撤/全", "重做", "deep");
    for (size_t bi = 0; bi < a.blocks.size(); ++bi) {
        const PaintBlock& b = a.blocks[bi];
        std::vector<Op> slice;
        for (const auto& o : all) if (o.t >= b.start && o.t <= b.end) slice.push_back(o);
        if (slice.empty()) continue;
        WinStat s = ComputeWindow(slice);
        char r1s[24], r2s[24];
        if (s.hasBrush) { snprintf(r1s, sizeof(r1s), "%9.3f", s.r1); snprintf(r2s, sizeof(r2s), "%9.3f", s.r2); }
        else              { snprintf(r1s, sizeof(r1s), "%9s", "无画笔"); snprintf(r2s, sizeof(r2s), "%9s", "无画笔"); }
        printf("  %-3zu %7.0f %6d %5d %5d %6d %7.4f %s %s %9.4f %8d %5s\n",
               bi, (double)(b.end - b.start) / 1e6, s.nTotal, s.nUndo, s.nBrush,
               s.nNormal, s.medGap, r1s, r2s, s.r3, s.nRedo, b.deep ? "是" : "否");
    }

    // ---- 多尺度窗口扫描 ----
    printf("\n---- 多尺度滚动窗口扫描（不重叠，每窗 >=3 个操作）----\n");
    struct Sc { double sec; const char* tag; };
    static const Sc scales[] = {
        {5.0,   "5 秒"},
        {10.0,  "10 秒"},
        {30.0,  "30 秒"},
        {60.0,  "60 秒（1 分钟）"},
        {120.0, "120 秒（2 分钟）"},
        {300.0, "300 秒（5 分钟）"},
        {600.0, "600 秒（10 分钟）"},
        {1800.0,"1800 秒（30 分钟）"},
    };
    for (const auto& sc : scales) ReportScale(all, sc.sec, sc.tag);
}

int main(int argc, char** argv) {
    struct Arg { const char* label; const char* path; };
    std::vector<Arg> args;
    for (int i = 1; i + 1 <= argc - 1; i += 2) args.push_back({argv[i], argv[i + 1]});
    if (args.empty()) {
        args.push_back({"我这份 5.8MB", "C:\\Users\\WINDOWS\\AppData\\Local\\PaintingBuddy\\logs\\20261018.jsonl"});
        args.push_back({"10-17 37MB",   "D:\\qqdownload\\logs\\20261017.jsonl"});
        args.push_back({"10-18 14MB",   "D:\\qqdownload\\logs\\20261018.jsonl"});
    }
    for (const auto& x : args) RunFile(x.label, x.path);
    printf("\ndone\n");
    return 0;
}
