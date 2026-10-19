// ============================================================
//  图表预览渲染器（离屏）：加载真实数据 → 分析 → 渲染成 PNG
//
//  目的：在无显示器/沙箱环境下验证「仿 iOS」图表的渲染正确性与观感。
//  产出 preview_gantt.png / preview_habit.png / preview_dark.png
// ============================================================
#include "Common.h"
#include "Config.h"
#include "Store.h"
#include "Analyzer.h"
#include "Ui.h"
#include "Charts.h"
#include <cstdio>
#include <cstdlib>

using namespace pb;

// 在离屏画布上渲染一个「总览」样式的画面（模拟主窗口的总览页核心区）
static bool RenderGanttPng(const Analysis& a, const Theme& th, const std::string& path,
                           int W) {
    // 甘特图：先量内容高度再建画布，保证整图完整
    GanttView g;
    g.SetTheme(th);
    g.SetData(a);
    g.FitAll();
    float ganttH = g.ContentHeight() + 16.f;
    int H = (int)(ganttH + 150.f + 40.f);
    if (H < 420) H = 420;

    Painter p;
    if (!p.CreateOffscreen((UINT)W, (UINT)H, 96.f)) { printf("  离屏创建失败\n"); return false; }
    p.SetTheme(th);
    p.Begin(&th.bg);

    const float pad = 24.f;
    float y = pad;

    // 标题
    p.Text(L"总览图", RF(pad, y, (float)W - pad, y + 26.f), 18.f, TWeight::Semibold,
           th.label, TAlign::Left, TVAlign::Center);
    y += 30.f;
    wchar_t sub[256];
    swprintf_s(sub, L"深度块 %d · 浅度块 %d · 打断 %d 次 · 撤回 %d · 画笔 %d · 普通 %d",
               a.deepBlocks, a.shallowBlocks, a.interruptCount,
               a.undoTotal, a.brushTotal, a.normalTotal);
    p.Text(sub, RF(pad, y, (float)W - pad, y + 18.f), 12.f, TWeight::Regular, th.label2,
           TAlign::Left, TVAlign::Center);
    y += 22.f;

    // 阈值说明
    wchar_t th2[256];
    swprintf_s(th2, L"普通间隔 < %.2fs   暂停 < %.1fs   ≥ %.1fs 打断   深度绘画：块内中位间隔 ≤ %.2fs",
               a.th.n1Sec, a.th.n1Sec, a.th.n2Sec, a.th.depthGapSec);
    p.Text(th2, RF(pad, y, (float)W - pad, y + 18.f), 11.f, TWeight::Regular, th.label3,
           TAlign::Left, TVAlign::Center);
    y += 26.f;

    // 图例
    GanttView::DrawLegend(p, RF(pad, y, (float)W - pad, y + 20.f), th);
    y += 28.f;

    float ganttTop = y;
    D2D1_RECT_F gr = RF(pad, ganttTop, (float)W - pad, (float)H - pad - 26.f);
    p.Card(gr, 10.f);
    D2D1_RECT_F inner = Inset(gr, 10.f, 8.f);
    g.Draw(p, inner, 0.f);

    // 底部坐标提示
    p.Text(L"列 = 时间（sqrt 压缩的可变单位，打断显示封顶 1 小时）；行 = 深度 / 浅度 / 暂停；打断为贯穿三行的整块",
           RF(pad, (float)H - pad - 18.f, (float)W - pad, (float)H - pad), 11.f,
           TWeight::Regular, th.label3, TAlign::Left, TVAlign::Center);

    p.End();
    bool ok = p.SavePng(path);
    printf("  %s -> %s  (%dx%d)\n", ok ? "OK" : "FAIL", path.c_str(), W, H);
    return ok;
}

// 渲染「习惯」页的四个子面板到一张长图
static bool RenderHabitPng(const Analysis& a, const Theme& th, const std::string& path, int W) {
    HabitView h;
    h.SetTheme(th);
    h.SetData(a);

    Painter measure;
    if (!measure.CreateOffscreen((UINT)W, 100u, 96.f)) return false;
    measure.SetTheme(th);
    float hh = 0;
    HabitView::Panel panels[] = { HabitView::Panel::Transition, HabitView::Panel::LongRange,
                                  HabitView::Panel::SecondOrder, HabitView::Panel::Summary };
    for (HabitView::Panel pp : panels) {
        h.SetPanel(pp);
        hh += h.ContentHeight((float)W - 48.f) + 30.f;
    }
    measure.Destroy();

    int H = (int)hh + 80;
    Painter p;
    if (!p.CreateOffscreen((UINT)W, (UINT)H, 96.f)) { printf("  离屏创建失败\n"); return false; }
    p.SetTheme(th);
    p.Begin(&th.bg);

    p.Text(L"习惯 · 行为序列关联", RF(24.f, 20.f, (float)W - 24.f, 48.f), 18.f, TWeight::Semibold,
           th.label, TAlign::Left, TVAlign::Center);
    p.Text(L"序列：普通操作 / 撤回·重做 / 画笔 / 普通间隔 / 暂停绘画（打断不进入序列）",
           RF(24.f, 48.f, (float)W - 24.f, 66.f), 12.f, TWeight::Regular, th.label2,
           TAlign::Left, TVAlign::Center);

    float y = 76.f;
    for (int i = 0; i < 4; ++i) {
        h.SetPanel(panels[i]);
        if (i == 1) h.SetLag(2);
        if (i == 2) h.SetPrefix(State::Normal, State::NormalGap);
        D2D1_RECT_F r = RF(24.f, y, (float)W - 24.f, y + h.ContentHeight((float)W - 48.f));
        h.Draw(p, r, 0.f);
        y += RH(r) + 30.f;
    }
    p.End();
    bool ok = p.SavePng(path);
    printf("  %s -> %s  (%dx%d)\n", ok ? "OK" : "FAIL", path.c_str(), W, H);
    return ok;
}

// 放大状态下的甘特图：验证「概括色块 → 逐笔操作明细」的交叉淡入淡出。
// 淡入淡出按真实时间推进，故循环重绘到动画稳定（Animating()==false）后导出。
static bool RenderGanttZoomedPng(const Analysis& a, const Theme& th, const std::string& path,
                                 int W, float scale, const wchar_t* tag) {
    const int H = 320;
    GanttView g;
    g.SetTheme(th);
    g.SetData(a);

    Painter p;
    if (!p.CreateOffscreen((UINT)W, (UINT)H, 96.f)) { printf("  离屏创建失败\n"); return false; }
    p.SetTheme(th);

    const float pad = 20.f;
    D2D1_RECT_F inner = RF(pad, pad + 26.f, (float)W - pad, (float)H - pad);

    // 以绘图区中心为锚点放大到指定倍率（等价于在该处滚动滚轮）
    float cx = (inner.left + inner.right) * 0.5f;
    g.ZoomAt(cx, scale, inner);
    // 把放大后的视图对准最长作画段的中点，避免落在打断压缩块上
    if (!a.blocks.empty()) {
        int64_t best = -1, bestLen = -1;
        for (const auto& b : a.blocks) {
            if (b.end - b.start > bestLen) { bestLen = b.end - b.start; best = (b.start + b.end) / 2; }
        }
        if (best > 0) g.CenterOnReal(best, inner);
    }

    // 循环重绘推进淡入淡出，直到动画稳定（上限 200 帧，防止异常时死循环）
    int frames = 0;
    do {
        p.Begin(&th.bg);
        p.Text(tag, RF(pad, pad - 16.f, (float)W - pad, pad + 10.f), 13.f,
               TWeight::Semibold, th.label, TAlign::Left, TVAlign::Center);
        g.Draw(p, inner, 0.f);
        p.End();
        ++frames;
        Sleep(16);   // 约 60fps，与界面淡入淡出定时器节奏一致
    } while (g.Animating() && frames < 200);

    bool ok2 = p.SavePng(path);
    printf("  %s -> %s  (%dx%d, %d 帧, 明细混合系数 %.2f)\n",
           ok2 ? "OK" : "FAIL", path.c_str(), W, H, frames, g.DetailMix());
    return ok2;
}

int main() {
    Clock::Init();
    // 只读隔离目录里的合成数据，不碰用户真实数据目录
    OverrideDataDir(SampleDataDir());
    CfgReload();
    Config cfg = Cfg();

    std::string outDir = "preview";   // 预览图输出到项目内，不再写数据目录
    printf("合成数据目录: %s\n", DataDir().c_str());
    printf("输出目录: %s\n\n", outDir.c_str());

    // 加载全部合成数据
    std::vector<Op> ops = LoadAllOps(0);
    printf("加载操作原子: %zu\n", ops.size());
    if (ops.size() < 8) {
        printf("数据不足，请先运行 build_gen.bat 生成合成数据\n");
        return 1;
    }

    Thresholds th = DeriveThresholds(ops, cfg);
    Analysis a = Analyze(std::move(ops), th);
    printf("分析：块=%d 深=%d 浅=%d 打断=%d 暂停=%d 序列=%zu 显著长程关联=%zu\n\n",
           a.blockCount, a.deepBlocks, a.shallowBlocks, a.interruptCount,
           a.pauseCount, a.sequence.size(), a.notables.size());

    Theme light = Theme::Light();
    Theme dark = Theme::Dark();

    printf("渲染甘特总览（浅色）：\n");
    RenderGanttPng(a, light, outDir + "\\preview_gantt.png", 1240);

    printf("渲染甘特总览（深色）：\n");
    RenderGanttPng(a, dark, outDir + "\\preview_gantt_dark.png", 1240);

    printf("渲染习惯关联（浅色）：\n");
    RenderHabitPng(a, light, outDir + "\\preview_habit.png", 760);

    // 放大层级目检：淡入淡出应在「每笔操作约 6px」附近发生
    printf("渲染甘特放大层级（验证概括↔明细淡入淡出）：\n");
    RenderGanttZoomedPng(a, light, outDir + "\\preview_gantt_zoom2.png",  1240, 2.f,  L"放大 2×（应仍为概括色块）");
    RenderGanttZoomedPng(a, light, outDir + "\\preview_gantt_zoom16.png", 1240, 16.f, L"放大 16×（过渡中或已切换）");
    RenderGanttZoomedPng(a, light, outDir + "\\preview_gantt_zoom64.png", 1240, 64.f, L"放大 64×（应为逐笔操作明细）");

    printf("\n完成。\n");
    return 0;
}
