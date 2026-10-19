// ============================================================
//  UI 离屏预览：无头渲染主窗口各页为 PNG，用于验证布局
//  链接除 main.cpp 外的全部单元；不建窗口、不装钩子、不启动采集
// ============================================================
#include "Common.h"
#include "Config.h"
#include "Store.h"
#include "Analyzer.h"
#include "MainWindow.h"
#include <cstdio>
#include <cstdlib>

using namespace pb;

int main() {
    Clock::Init();
    // 只读隔离目录里的合成数据，不碰用户真实数据目录
    OverrideDataDir(SampleDataDir());
    CfgReload();
    Config cfg = Cfg();

    std::string outDir = "preview";   // 预览图输出到项目内，不再写数据目录

    // 载入合成数据做分析，供总览/习惯页有内容可画
    std::vector<Op> ops = LoadAllOps(0);
    // 必须在 std::move 之前取 size：移动后的 vector 恒为 0，
    // 此前把 ops.size() 放在 Analyze(std::move(ops)) 之后打印，
    // 于是无论读到多少数据都显示 ops=0，极易被误判为「没读到数据」。
    size_t opCount = ops.size();
    Analysis a;
    if (!ops.empty()) {
        Thresholds th = DeriveThresholds(ops, cfg);
        a = Analyze(std::move(ops), th);
    }
    printf("合成数据目录: %s\n", DataDir().c_str());
    printf("输出目录: %s\n", outDir.c_str());
    printf("ops=%zu empty=%d\n", opCount, (int)a.empty);

    App& app = App::I();

    // 深色（默认）
    app.InitForHeadlessPreview(true, &a);
    // 设计逻辑尺寸（DIP）。按各缩放比换算成物理像素传入 RenderPagePng：
    // 物理 = DIP * dpi/96。这样每档输出的逻辑布局区域一致，用于验证
    // 不同 DPI 下界面不裁切、不错位（坐标系修复后的无头回归）。
    const int DW = 1240, DH = 860;   // 设计 DIP 尺寸
    struct Scale { const char* tag; float dpi; } scales[] = {
        { "100", 96.f }, { "125", 120.f }, { "150", 144.f },
    };
    for (const Scale& s : scales) {
        int pw = (int)(DW * s.dpi / 96.f);
        int ph = (int)(DH * s.dpi / 96.f);
        char suffix[32];
        snprintf(suffix, sizeof(suffix), "_%s", s.tag);
        std::string sfx = suffix;
        printf("settings %s%%: %d\n", s.tag, (int)app.RenderPagePng(3, outDir + "\\ui_settings_dark" + sfx + ".png", pw, ph, s.dpi));
        printf("overview %s%%: %d\n", s.tag, (int)app.RenderPagePng(0, outDir + "\\ui_overview_dark" + sfx + ".png", pw, ph, s.dpi));
        if (s.dpi == 96.f) {
            printf("record dark   : %d\n", (int)app.RenderPagePng(2, outDir + "\\ui_record_dark.png", pw, ph, s.dpi));
            printf("habit dark    : %d\n", (int)app.RenderPagePng(1, outDir + "\\ui_habit_dark.png", pw, ph, s.dpi));
        }
    }

    // 浅色
    app.InitForHeadlessPreview(false, &a);
    printf("settings light: %d\n", (int)app.RenderPagePng(3, outDir + "\\ui_settings_light.png", DW, DH, 96.f));

    // 设置页加高截图：设置页本身可滚动，860 DIP 会截断下半部分。
    // 「数据与目录」卡新增第三行（导出诊断信息按钮），用加高画布确认
    // 卡片高度按行数正确计算、按钮对齐不错位。
    printf("settings tall : %d\n", (int)app.RenderPagePng(3, outDir + "\\ui_settings_tall.png", DW, 1500, 96.f));

    printf("done\n");
    return 0;
}
