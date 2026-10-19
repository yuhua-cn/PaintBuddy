// ============================================================
//  间隔统计探针（诊断工具，不进产品）
//
//  只统计两类间隔：
//    A. 画笔间隔   —— 相邻数位笔笔画之间（抬笔 → 下一次落笔）
//    B. 总操作间隔 —— 进入分析层的全部操作之间（生产口径：MergeOps 之后）
//  另附原始未合并口径作对照。
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

static void Report(const char* title, std::vector<double> gaps) {
    printf("\n──── %s ────\n", title);
    if (gaps.empty()) { printf("  无样本\n"); return; }
    std::vector<double> s = gaps;
    std::sort(s.begin(), s.end());
    double sum = 0; for (double x : s) sum += x;
    printf("  样本数 n = %zu\n", s.size());
    printf("  min=%.4f  p10=%.4f  p25=%.4f  p50=%.4f  p75=%.4f  p90=%.4f  p95=%.4f  p99=%.4f  max=%.2f  mean=%.4f  (秒)\n",
           s.front(), Pct(s, .10), Pct(s, .25), Pct(s, .50), Pct(s, .75),
           Pct(s, .90), Pct(s, .95), Pct(s, .99), s.back(), sum / (double)s.size());
    const double edges[] = {0.1, 0.3, 0.6, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0};
    size_t n = s.size(), idx = 0;
    printf("  分桶:");
    for (size_t e = 0; e < sizeof(edges) / sizeof(edges[0]); ++e) {
        size_t c = 0;
        while (idx < n && s[idx] < edges[e]) { ++c; ++idx; }
        printf(" <%g:%.1f%%", edges[e], 100.0 * (double)c / (double)n);
    }
    printf(" >=%g:%.1f%%\n", edges[sizeof(edges) / sizeof(edges[0]) - 1],
           100.0 * (double)(n - idx) / (double)n);
    printf("  合计 = %.1f 秒\n", sum);
}

int main(int argc, char** argv) {
    const std::string day = (argc > 1) ? argv[1] : "20261018";
    EnsureDirs();
    printf("日志目录: %s\n", LogDir().c_str());

    // 生产代码用 parsePoints=false（Analyzer.cpp 三处调用都是 false）。
    // 先确认两种模式的记录数是否一致。
    DayEvents dFalse, dTrue;
    bool okF = LoadDay(day, dFalse, false);
    bool okT = LoadDay(day, dTrue, true);
    printf("LoadDay(parsePoints=false) ok=%d 记录=%zu\n", okF ? 1 : 0, dFalse.records.size());
    printf("LoadDay(parsePoints=true ) ok=%d 记录=%zu\n", okT ? 1 : 0, dTrue.records.size());
    if (dFalse.records.size() != dTrue.records.size())
        printf("!! 两种模式记录数不一致 —— 以生产口径 false 为准\n");

    DayEvents& de = dFalse;
    if (de.records.empty()) { printf("[FAIL] 无数据\n"); return 1; }

    // ============ A. 画笔间隔（原始笔记录，未经合并）============
    std::vector<const RawRecord*> pens;
    for (const auto& r : de.records) {
        if (r.tUs > 0 && r.type == 'p' && !r.hover) pens.push_back(&r);
    }
    std::sort(pens.begin(), pens.end(),
              [](const RawRecord* a, const RawRecord* b) { return a->tUs < b->tUs; });
    printf("\n数位笔笔画（非悬空）= %zu 条\n", pens.size());

    std::vector<double> penGaps, penDurs;
    for (size_t i = 0; i < pens.size(); ++i) {
        penDurs.push_back((double)(pens[i]->endUs - pens[i]->tUs) / 1e6);
        if (i > 0) {
            int64_t g = pens[i]->tUs - pens[i - 1]->endUs;
            if (g > 0) penGaps.push_back((double)g / 1e6);
        }
    }
    Report("A. 画笔间隔（相邻笔画：抬笔 → 下一次落笔）", penGaps);
    Report("A附. 笔画自身时长（落笔 → 抬笔）", penDurs);

    // ============ B. 总操作间隔 ============
    std::vector<Op> opsRaw = RecordsToOps(de.records);
    std::vector<Op> opsMerged = MergeOps(opsRaw);
    printf("\n原始记录 %zu → Op %zu → 合并后 Op %zu（合并窗口 %.0f ms）\n",
           de.records.size(), opsRaw.size(), opsMerged.size(),
           (double)EVENT_MERGE_WINDOW_US / 1000.0);

    // 与 DeriveThresholds 完全相同的口径：running max of end
    auto collect = [](const std::vector<Op>& ops) {
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
    };
    Report("B. 总操作间隔（生产口径：MergeOps 之后，阈值推导实际用的就是这个）",
           collect(opsMerged));
    Report("B附. 总操作间隔（未合并的原始 Op，仅对照）", collect(opsRaw));

    // ============ C. 两者关系 ============
    std::vector<double> all = collect(opsMerged);
    printf("\n──── C. 对比 ────\n");
    if (!all.empty() && !penGaps.empty()) {
        printf("  画笔间隔  p50=%.4fs  p75=%.4fs  （n=%zu）\n",
               Pct(penGaps, .5), Pct(penGaps, .75), penGaps.size());
        printf("  总操作间隔 p50=%.4fs  p75=%.4fs  （n=%zu）\n",
               Pct(all, .5), Pct(all, .75), all.size());
        size_t under1 = 0;
        for (double g : all) if (g < 1.0) ++under1;
        printf("  总操作间隔中 <1s 的占 %.1f%%（%zu/%zu）\n",
               100.0 * (double)under1 / (double)all.size(), under1, all.size());
    }

    printf("\ndone\n");
    return 0;
}
