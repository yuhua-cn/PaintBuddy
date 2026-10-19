// ============================================================
//  n1 扫描探针（诊断工具，不进产品）
//
//  目标：用户要求数据1 的暂停次数为个位数。
//  本探针扫描 n1，输出每个取值下三份数据的：
//    - 暂停 / 普通间隔 / 打断 计数
//    - 活动数（n1 同时是活动合并窗口，抬高会合并更多操作）
//    - 甘特单元数
//    - 该 n1 在「停顿候选池」中所处的百分位
//  并列出最大的若干个间隔，看清要跨过哪些值才能到个位数。
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

// n1 在池中的百分位排名（即「有多少比例的间隔 < n1」）
static double RankPct(const std::vector<double>& sorted, double v) {
    if (sorted.empty()) return 0.0;
    size_t below = (size_t)(std::lower_bound(sorted.begin(), sorted.end(), v) - sorted.begin());
    return 100.0 * (double)below / (double)sorted.size();
}

static void RunFile(const char* label, const std::string& path) {
    printf("\n################ %s ################\n", label);
    DayEvents de;
    if (!LoadFileDirect(path, de)) { printf("[FAIL] 无法加载 %s\n", path.c_str()); return; }

    std::vector<Op> merged = MergeOps(RecordsToOps(de.records));
    Config cfg;
    Thresholds prod = DeriveThresholds(merged, cfg);
    Analysis base = Analyze(merged, prod);

    printf("当前生产阈值 n1=%.3fs  n2=%.1fs  depthGap=%.3fs\n",
           prod.n1Sec, prod.n2Sec, base.th.depthGapSec);
    printf("当前结果：块=%d  普通间隔=%d  暂停=%d  打断=%d  活动=%zu\n",
           base.blockCount, base.normalGapCount, base.pauseCount, base.interruptCount,
           base.activities.size());

    // 间隔池
    std::vector<double> allGaps, filt;
    int64_t prevEnd = merged[0].end;
    for (size_t i = 1; i < merged.size(); ++i) {
        int64_t g = merged[i].t - prevEnd;
        if (g > 0) {
            double sec = (double)g / 1e6;
            allGaps.push_back(sec);
            if (sec >= 1.0) filt.push_back(sec);
        }
        prevEnd = std::max(prevEnd, merged[i].end);
    }
    std::sort(allGaps.begin(), allGaps.end());
    std::sort(filt.begin(), filt.end());

    printf("\n---- 停顿候选池（>=1s，n=%zu）----\n", filt.size());
    printf("  p50=%.3f  p75=%.3f  p90=%.3f  p95=%.3f  p97=%.3f  p99=%.3f  max=%.1f\n",
           Pct(filt, .50), Pct(filt, .75), Pct(filt, .90), Pct(filt, .95),
           Pct(filt, .97), Pct(filt, .99), filt.back());

    printf("\n---- 最大的 15 个间隔（要跨过它们才能减少暂停）----\n");
    for (int k = 0; k < 15 && k < (int)allGaps.size(); ++k) {
        size_t idx = allGaps.size() - 1 - (size_t)k;
        printf("  第%-3d大 = %9.3f s\n", k + 1, allGaps[idx]);
    }
    // 若要让暂停 <=9，n1 必须超过第 10 大的间隔
    if (allGaps.size() >= 10) {
        double need = allGaps[allGaps.size() - 10];
        printf("  → 要使暂停 <=9，n1 必须 > %.3f s（第 10 大间隔）\n", need);
        printf("    该值在停顿候选池中处于 p%.1f\n", RankPct(filt, need));
    }

    printf("\n---- n1 扫描 ----\n");
    printf("  %-8s %6s %8s %8s %6s %8s %8s %8s %8s\n",
           "n1(s)", "暂停", "普通间隔", "打断", "块", "深/浅", "活动数", "甘特单元", "池内分位");
    static const double sweep[] = {2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0,
                                   12.0, 15.0, 20.0, 25.0, 30.0, 45.0, 60.0};
    for (double n1 : sweep) {
        Thresholds t = prod;
        t.n1Sec = n1;
        t.n1Auto = t.n2Auto = t.depthAuto = false;   // 固定，隔离 n1 的影响
        Analysis a = Analyze(merged, t);
        GanttTimeline g = BuildGantt(a);
        printf("  %-8.2f %6d %8d %8d %6d %4d/%-3d %8zu %8zu %9.1f%%%s\n",
               n1, a.pauseCount, a.normalGapCount, a.interruptCount, a.blockCount,
               a.deepBlocks, a.shallowBlocks, a.activities.size(), g.cells.size(),
               RankPct(filt, n1),
               a.pauseCount <= 9 ? "   ← 个位数" : "");
    }
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
