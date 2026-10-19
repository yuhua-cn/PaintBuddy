// ============================================================
//  合成数据生成器：产出逼真的「绘画会话」JSONL 写入真实数据目录，
//  用于端到端验证 加载→反序列化→分析→甘特/习惯渲染 整条链路。
//  （GUI 无法在沙箱里靠真实输入驱动，故用合成数据打通全流程）
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Config.h"
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace pb;

static std::mt19937 rng(20261016u);
static double U(double a, double b) {
    std::uniform_real_distribution<double> d(a, b);
    return d(rng);
}
static int UI(int a, int b) {
    std::uniform_int_distribution<int> d(a, b);
    return d(rng);
}

// 生成一笔数位笔笔画（含连续采样点，带压力曲线与速度）
static void EmitPenStroke(int64_t& t, uint8_t device, bool withKeyboard) {
    RawRecord r;
    r.type = 'p';
    r.tUs = t;
    r.device = device;
    r.kbAttr = withKeyboard;    // 落笔时有键按下 → 归为键盘操作
    r.kind = withKeyboard ? OpKind::Normal : OpKind::Brush;

    int nPts = UI(12, 120);                 // 采样点数
    double dur = U(0.15, 1.6);              // 笔画时长（秒）
    int64_t dtUs = (int64_t)(dur * 1e6 / nPts);
    double x = U(200, 2400), y = U(200, 1400);
    double ang = U(0, 6.283);
    double spd = U(60, 900);                // 像素/秒
    float maxP = 0, sumP = 0, maxS = 0, sumS = 0, len = 0;

    for (int i = 0; i < nPts; ++i) {
        double frac = (double)i / nPts;
        // 压力：起笔轻→中段重→收笔轻（钟形）
        float pr = (float)(0.15 + 0.8 * sin(3.14159 * frac) * U(0.7, 1.0));
        if (pr < 0) pr = 0; if (pr > 1) pr = 1;
        // 位置：带曲率的轨迹
        ang += U(-0.3, 0.3);
        double step = spd * (dtUs / 1e6);
        x += cos(ang) * step;
        y += sin(ang) * step;
        float s = (float)(spd * U(0.6, 1.4));

        PenPoint p;
        p.dtUs = (i == 0) ? 0 : (int32_t)dtUs;
        p.x = (int32_t)x; p.y = (int32_t)y;
        p.speed = s; p.pressure = pr;
        p.flags = PENF_TIP | PENF_INRANGE;
        r.ppts.push_back(p);

        maxP = std::max(maxP, pr); sumP += pr;
        maxS = std::max(maxS, s);  sumS += s;
        if (i > 0) len += (float)step;
    }
    r.maxPressure = maxP; r.avgPressure = nPts ? sumP / nPts : 0;
    r.maxSpeed = maxS;    r.avgSpeed = nPts ? sumS / nPts : 0;
    r.pathLen = len;
    r.endUs = t + (int64_t)(dur * 1e6);
    r.exe = "clipstudiopaint.exe";
    r.title = "作品_线稿.csp - Clip Studio Paint";
    r.isTarget = true;
    AppendEvent(r);

    t = r.endUs;
}

// 生成一次键盘操作（快捷键）
static void EmitKey(int64_t& t, bool& lastWasUndo) {
    RawRecord r;
    r.type = 'k';
    r.tUs = t;
    int pick = UI(0, 100);
    int vk; uint8_t mods = PB_MOD_NONE; OpKind kind = OpKind::Normal; int cnt = 1;
    if (pick < 14) {                        // 撤回 Ctrl+Z
        vk = 'Z'; mods = PB_MOD_CTRL; kind = OpKind::Undo;
        cnt = (UI(0, 100) < 30) ? UI(2, 5) : 1;   // 有时连按
        lastWasUndo = true;
    } else if (pick < 19) {                 // 重做 Ctrl+Y / Ctrl+Shift+Z
        if (UI(0, 1)) { vk = 'Y'; mods = PB_MOD_CTRL; }
        else          { vk = 'Z'; mods = PB_MOD_CTRL | PB_MOD_SHIFT; }
        kind = OpKind::Redo;
        lastWasUndo = false;
    } else if (pick < 34) { vk = 'B'; lastWasUndo = false; }        // 画笔
    else if (pick < 46) { vk = 'E'; lastWasUndo = false; }          // 橡皮
    else if (pick < 56) { vk = 'V'; lastWasUndo = false; }          // 移动
    else if (pick < 64) { vk = '['; lastWasUndo = false; }          // 缩小笔刷
    else if (pick < 72) { vk = ']'; lastWasUndo = false; }          // 放大笔刷
    else if (pick < 80) { vk = 'X'; lastWasUndo = false; }          // 切换前景背景
    else if (pick < 88) { vk = 'G'; lastWasUndo = false; }          // 油漆桶
    else if (pick < 94) { vk = VK_SPACE; lastWasUndo = false; }     // 抓手
    else { vk = UI('A', 'Z'); lastWasUndo = false; }

    r.vk = vk; r.mods = mods; r.count = cnt; r.kind = kind;
    int64_t hold = cnt > 1 ? (int64_t)(cnt - 1) * 90000 : 0;  // 连按在 0.8s 内合并
    r.endUs = t + hold + UI(20000, 80000);
    r.exe = "clipstudiopaint.exe";
    r.title = "作品_线稿.csp - Clip Studio Paint";
    r.isTarget = true;
    AppendEvent(r);
    t = r.endUs;
}

// 生成一次鼠标操作（框选/拖动图层）
static void EmitMouse(int64_t& t) {
    RawRecord r;
    r.type = 'M';
    r.tUs = t;
    r.kind = OpKind::Normal;
    r.button = 0;
    int nPts = UI(6, 40);
    double x = U(200, 2400), y = U(200, 1400);
    double dur = U(0.1, 0.8);
    int64_t dtUs = (int64_t)(dur * 1e6 / nPts);
    float len = 0, maxS = 0;
    for (int i = 0; i < nPts; ++i) {
        x += U(-40, 40); y += U(-40, 40);
        float s = (float)U(50, 600);
        PathPoint p;
        p.dtUs = (i == 0) ? 0 : (int32_t)dtUs;
        p.x = (int32_t)x; p.y = (int32_t)y; p.speed = s;
        r.mpts.push_back(p);
        maxS = std::max(maxS, s);
        if (i > 0) len += (float)U(5, 60);
    }
    r.pathLen = len; r.maxSpeed = maxS;
    r.endUs = t + (int64_t)(dur * 1e6);
    r.exe = "clipstudiopaint.exe";
    r.title = "作品_线稿.csp - Clip Studio Paint";
    r.isTarget = true;
    AppendEvent(r);
    t = r.endUs;
}

static void EmitFocus(int64_t t, const char* exe, const char* title, bool target) {
    RawRecord r;
    r.type = 'f'; r.tUs = t; r.exe = exe; r.title = title; r.isTarget = target;
    AppendEvent(r);
}

// 生成一个作画会话
static void EmitSession(int64_t& t, const char* exe, const char* title) {
    EmitFocus(t, exe, title, true);
    bool lastWasUndo = false;
    int nOps = UI(40, 220);
    // 会话内部偶尔有短暂思考停顿（暂停绘画，不打断）
    for (int i = 0; i < nOps; ++i) {
        int pick = UI(0, 100);
        if (pick < 62)       EmitPenStroke(t, PEN_EXTERNAL, (UI(0, 100) < 8));
        else if (pick < 88)  EmitKey(t, lastWasUndo);
        else                 EmitMouse(t);

        // 操作间隔：大部分很短（普通间隔），偶尔较长（暂停）
        double gap = U(0.1, 0.9);
        if (UI(0, 100) < 8) gap = U(2.0, 12.0);   // 思考停顿 → 暂停绘画
        t += (int64_t)(gap * 1e6);

        // 撤回后常常紧跟一个修正动作（制造长程关联）
        if (lastWasUndo && UI(0, 100) < 50) {
            EmitPenStroke(t, PEN_EXTERNAL, false);
            t += (int64_t)(U(0.2, 0.6) * 1e6);
            lastWasUndo = false;
        }
    }
    EmitFocus(t, "explorer.exe", "桌面", false);   // 切出
}

int main() {
    Clock::Init();
    // 隔离目录：合成数据绝不能写用户真实数据目录（%LOCALAPPDATA%\PaintingBuddy），
    // 否则会与真实绘画记录混在同一个 .jsonl 里，只能靠时间戳逐条甄别清除。
    OverrideDataDir(SampleDataDir());
    CfgReload();
    printf("合成数据目录: %s\n", DataDir().c_str());

    // 清空该隔离目录里已有的合成数据（不影响用户真实数据）
    for (const auto& f : ListFiles(LogDir(), ".jsonl")) {
        std::string p = LogDir() + "\\" + f;
        DeleteFileA(p.c_str());
    }

    // 以「今天往前推 34 天」为起点，连续 5 天，避免污染用户真实数据
    int64_t now = Clock::NowUs();
    int64_t day0 = DayStartUs(now) - 34LL * 86400LL * 1000000LL;

    const char* exes[]   = { "clipstudiopaint.exe", "photoshop.exe", "sai2.exe" };
    const char* titles[] = { "作品_线稿.csp - Clip Studio Paint",
                             "角色_上色.psd - Adobe Photoshop",
                             "涂鸦_01.sai2 - PaintTool SAI" };

    int totalSessions = 0;
    for (int d = 0; d < 5; ++d) {
        int64_t t = day0 + (int64_t)d * 86400LL * 1000000LL
                    + (int64_t)U(9, 20) * 3600LL * 1000000LL;  // 当天 9~20 点开始
        int sessions = UI(1, 3);
        for (int s = 0; s < sessions; ++s) {
            int which = UI(0, 2);
            EmitSession(t, exes[which], titles[which]);
            ++totalSessions;
            // 会话之间：短则几分钟（暂停），长则数小时（打断）
            double between = U(3, 40) * 60.0;
            if (UI(0, 100) < 40) between = U(2, 9) * 3600.0;   // 打断
            t += (int64_t)(between * 1e6);
            if (t > day0 + (int64_t)(d + 1) * 86400LL * 1000000LL) break;  // 不跨天太多
        }
    }
    Events().Flush();
    Events().Close();

    // 统计生成结果
    int64_t bytes = 0;
    std::vector<std::string> days = ListDays();
    for (const auto& dy : days) {
        std::string p = LogDir() + "\\" + dy + ".jsonl";
        int64_t sz = FileSizeBytes(p);
        bytes += sz;
        printf("  %s.jsonl  %lld 字节\n", dy.c_str(), (long long)sz);
    }
    printf("共 %d 天，%d 个会话，%lld 字节\n", (int)days.size(), totalSessions, (long long)bytes);

    // 立即验证可加载
    if (!days.empty()) {
        DayEvents de;
        if (LoadDay(days.back(), de))
            printf("回读校验：最后一天 %s 载入 %zu 条记录\n", days.back().c_str(), de.records.size());
        else
            printf("回读校验失败\n");
    }
    return 0;
}
