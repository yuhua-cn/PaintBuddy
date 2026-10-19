// ============================================================
//  数位设备解析层离线测试
//
//  为什么存在：开发者只有 Wacom，无法为 Huion / XP-Pen / Gaomon /
//  汉王等品牌做真机验证。而 PHIDP_PREPARSED_DATA 无法离线构造
//  （Windows SDK 没有任何接受原始 report descriptor 字节数组的函数），
//  所以「造一个 Huion 描述符喂给解析器」这条路走不通。
//
//  本测试改用接缝注入：PenInput::SetHidFetchForTest 替换掉唯一直接
//  接触 HidP_* 的那一步，由 mock 按参数直填 X/Y/压感/Usage 列表，
//  从而在没有对应硬件的情况下驱动 ExtractSample → 坐标映射 → 压感
//  归一 全链路。品牌差异真正体现的地方不是字节偏移（HidP 已按描述符
//  处理），而是「解析出的逻辑值如何被解释」——这些全都能离线验证。
//
//  刻意不调用 EnsureDirs / OverrideDataDir：g_dataDir 保持为空时
//  LogF 直接返回，测试全程零磁盘写入、不建窗口、不装钩子。
// ============================================================
#include "PenInput.h"
#include "PenOverrides.h"
#include "PenInject.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace pb;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; printf("  [PASS] %s\n", msg); } \
    else      { ++g_fail; printf("  [FAIL] %s   (line %d)\n", msg, __LINE__); } \
} while (0)

// HID Digitizer 用量常量（与 PenInput.cpp 内部保持一致）
static const USAGE UP_DIGITIZER = 0x0D;
static const USAGE U_DIG_IN_RANGE      = 0x32;
static const USAGE U_DIG_TOUCH         = 0x33;
static const USAGE U_DIG_TIP_PRESSURE  = 0x30;
static const USAGE U_DIG_TIP_SWITCH    = 0x42;
static const USAGE U_DIG_BARREL_SWITCH = 0x44;
static const USAGE U_DIG_ERASER        = 0x45;

// ------------------------------------------------------------
//  mock HID 取值实现
// ------------------------------------------------------------
struct MockHid {
    // 要求的 Report ID 前缀；0xFF = 不校验前缀（形态 0 直读即可成功）
    BYTE  needPrefix = 0xFF;
    bool  succeed    = true;
    LONG  x = 0, y = 0, p = 0;
    bool  hasX = true, hasY = true, hasP = false;
    std::vector<USAGE> usages;
    int   calls = 0;

    // 阶段 1 新增：按 link collection 精确取值的模拟
    //   onlyLink < 0  = 任何 link 都成功（不校验）
    //   onlyLink >= 0 = 仅当传入的 linkCollection == onlyLink 时才成功
    // 用于验证 FetchWithFallback 的「先按 penLink 精确取、失败退回 0」两条路径。
    int    onlyLink = -1;
    USHORT lastLink = 0xFFFF;   // 记录最后一次调用收到的 linkCollection，供断言
    int    linkCalls = 0;       // onlyLink>=0 时，命中该 link 的调用次数
};

static bool MockFetch(MockHid& m, PHIDP_PREPARSED_DATA, const BYTE* buf, ULONG len,
                      USHORT linkCollection,
                      USAGE* usages, ULONG& nUsages,
                      LONG& vx, LONG& vy, LONG& vp,
                      bool& gx, bool& gy, bool& gp) {
    ++m.calls;
    m.lastLink = linkCollection;
    if (!m.succeed) return false;
    if (m.needPrefix != 0xFF) {
        if (len < 1 || buf[0] != m.needPrefix) return false;
    }
    // link collection 校验：模拟「X/Y 只在特定集合里」的复合设备
    if (m.onlyLink >= 0) {
        if ((int)linkCollection != m.onlyLink) return false;
        ++m.linkCalls;
    }
    nUsages = 0;
    for (USAGE u : m.usages) { if (nUsages < 64) usages[nUsages++] = u; }
    vx = m.x; vy = m.y; vp = m.p;
    gx = m.hasX; gy = m.hasY; gp = m.hasP;
    return gx && gy;   // 与真实实现同一判据
}

static void Install(PenInput& pen, MockHid& m) {
    pen.SetHidFetchForTest([&m](PHIDP_PREPARSED_DATA prep, const BYTE* buf, ULONG len,
                                USHORT linkCollection,
                                USAGE* usages, ULONG& nUsages,
                                LONG& vx, LONG& vy, LONG& vp,
                                bool& gx, bool& gy, bool& gp) {
        return MockFetch(m, prep, buf, len, linkCollection, usages, nUsages, vx, vy, vp, gx, gy, gp);
    });
}

// ------------------------------------------------------------
//  设备构造辅助
//
//  BuildMapping 是 private 且要枚举真实指针设备，测试直接手工填映射
//  系数（screen = logical * s + o），把逻辑坐标线性映射到 1920x1080。
// ------------------------------------------------------------
static const int SCREEN_W = 1920, SCREEN_H = 1080;

static Dev MakeAbsPenDev(LONG lxMax, LONG lyMax, LONG pMax, bool hasPressure) {
    Dev d;
    d.name      = "test-device";
    // 假非空指针：mock 不会解引用它，但 ExtractSample 要求 preparsed 非空
    d.preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(static_cast<uintptr_t>(1));
    d.type      = PEN_EXTERNAL;
    d.hasX = true; d.hasY = true;
    d.lxMin = 0;   d.lxMax = lxMax;
    d.lyMin = 0;   d.lyMax = lyMax;
    d.hasPressure = hasPressure;
    d.pressMin = 0;
    d.pressMax = pMax > 0 ? pMax : 1;
    d.scratch.resize(64);
    d.sx = (double)SCREEN_W / (double)lxMax; d.ox = 0;
    d.sy = (double)SCREEN_H / (double)lyMax; d.oy = 0;
    d.mappingValid = true;
    d.mappingSource = 1;
    return d;
}

// 一条长度固定的假报文（内容无关，mock 只看 buf[0] 前缀）
// 数组要够大：长度用例会喂到 19 字节，TryParseForms 内部还会
// memcpy(scratch+1, rep, size) 读满 size 字节，越界即未定义行为。
static const ULONG REP_LEN = 10;   // Wacom 式 10 字节
static BYTE g_rep[64];
static void FillReport(BYTE firstByte) {
    memset(g_rep, 0, sizeof(g_rep));
    g_rep[0] = firstByte;
}

static bool Near(double a, double b, double eps = 1.0) { return fabs(a - b) <= eps; }

int main() {
    printf("==== 绘画伴侣 数位设备解析层离线测试 ====\n");
    printf("（无真机依赖：HID 取值由 mock 注入）\n\n");

    // ========================================================
    printf("-- A. 接缝基本可用性 --\n");
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.x = 100; m.y = 200; m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        FillReport(0x00);
        PenSample s;
        bool ok = pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(ok, "注入 mock 后 ExtractSample 能产出采样");
        CHECK(m.calls > 0, "mock 确实被调用（接缝生效）");
        CHECK(s.x == (int32_t)llround(100.0 * d.sx), "屏幕坐标由逻辑坐标线性映射得出");
        CHECK(s.logicalX == 100 && s.logicalY == 200, "logicalX/Y 回传原始逻辑值（供标定）");
    }
    {
        PenInput pen; MockHid m; Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        d.preparsed = nullptr;
        FillReport(0x00);
        PenSample s;
        CHECK(!pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s), "preparsed 为空时拒绝解析");
    }
    {
        PenInput pen; MockHid m; Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        PenSample s;
        CHECK(!pen.ExtractSample(d, g_rep, 0, 1000000, s), "零长度报文被拒绝");
    }
    {
        // 传空应恢复真实实现，且不能崩溃。恢复后立即设回 mock，
        // 绝不触发真实 HidP_*（那会解引用上面的假 preparsed 指针）
        PenInput pen; MockHid m; m.x = 5; m.y = 6;
        Install(pen, m);
        pen.SetHidFetchForTest(nullptr);
        int before = m.calls;
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        CHECK(pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s), "SetHidFetchForTest(nullptr) 后可重新注入");
        CHECK(m.calls > before, "重新注入的 mock 生效");
    }

    // ========================================================
    printf("\n-- B. 压感量程自适应（品牌差异核心） --\n");
    // 各品牌原生量程不同，必须靠描述符的 pressMin/pressMax 归一，
    // 不能按品牌硬编码。喂「半量程」原始值，都应归一到 ~0.5。
    // ========================================================
    {
        struct Case { const char* name; LONG pMax; };
        const Case cases[] = {
            { "Wacom 0..8191",       8191 },
            { "Huion/XP-Pen 0..4095", 4095 },
            { "老款 0..1023",         1023 },
            { "极老款 0..255",         255 },
        };
        for (const auto& c : cases) {
            PenInput pen; MockHid m;
            m.hasP = true; m.p = c.pMax / 2;
            m.x = 10; m.y = 10;
            m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE };
            Install(pen, m);
            Dev d = MakeAbsPenDev(32767, 32767, c.pMax, true);
            FillReport(0x00);
            PenSample s;
            // 期望值按与实现同一公式算（含整除截断），这样既严格又不受
            // 奇数量程（如 255/2=127）的截断影响
            float expected = (float)((c.pMax / 2 - 0) / (double)c.pMax);
            char buf[128];
            snprintf(buf, sizeof(buf), "%s：半量程归一到 %.4f", c.name, expected);
            CHECK(pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s) &&
                  fabs(s.pressure - expected) < 0.0001f, buf);
        }
    }
    {
        PenInput pen; MockHid m;
        m.hasP = true; m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        FillReport(0x00);
        PenSample s;
        m.p = 0;     pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.pressure == 0.f, "压感原始值 0 归一到 0.0");
        m.p = 8191;  pen.ExtractSample(d, g_rep, REP_LEN, 1001000, s);
        CHECK(s.pressure == 1.f, "压感原始值满量程归一到 1.0");
        m.p = 99999; pen.ExtractSample(d, g_rep, REP_LEN, 1002000, s);
        CHECK(s.pressure == 1.f, "超出量程的压感被夹取到 1.0（防越界）");
    }

    // ========================================================
    printf("\n-- B2. 旧版本 Wacom 兼容 --\n");
    // 依据：本机 pb.log 实测存在 pressure=1(0..1024) 的设备
    // （VID_0000&PID_0000 usage=0D:02），且 Wacom 各代压感档差异很大。
    // 旧款还有非零压感下限、橡皮独立 tool 且不带压感字段等情况。
    // ========================================================
    {
        // 低分辨率压感档（旧 Wacom 常见）
        struct Case { const char* name; LONG pMax; };
        const Case cases[] = {
            { "旧 Wacom 0..1024", 1024 },
            { "更旧 0..512",       512 },
            { "极旧 0..256",       256 },
        };
        for (const auto& c : cases) {
            PenInput pen; MockHid m;
            m.hasP = true; m.p = c.pMax / 2;
            m.x = 10; m.y = 10;
            m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE };
            Install(pen, m);
            Dev d = MakeAbsPenDev(16704, 10506, c.pMax, true);   // 旧 Wacom 非归一逻辑范围
            FillReport(0x00);
            PenSample s;
            float expected = (float)((c.pMax / 2) / (double)c.pMax);
            char buf[128];
            snprintf(buf, sizeof(buf), "%s：半量程归一到 %.4f", c.name, expected);
            CHECK(pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s) &&
                  fabs(s.pressure - expected) < 0.0001f, buf);
        }
    }
    {
        // 非零压感下限：归一必须减 pressMin，否则轻压区被压缩
        PenInput pen; MockHid m;
        m.hasP = true; m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(16704, 10506, 1023, true);
        d.pressMin = 100; d.pressMax = 1023;    // 下限非 0
        FillReport(0x00);
        PenSample s;
        m.p = 100;  pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.pressure == 0.f, "压感等于 pressMin 时归一到 0.0（下限已扣除）");
        m.p = 1023; pen.ExtractSample(d, g_rep, REP_LEN, 1001000, s);
        CHECK(s.pressure == 1.f, "压感等于 pressMax 时归一到 1.0");
        m.p = (100 + 1023) / 2; pen.ExtractSample(d, g_rep, REP_LEN, 1002000, s);
        CHECK(fabs(s.pressure - 0.5f) < 0.01f, "非零下限时中点仍归一到约 0.5");
        m.p = 50;   pen.ExtractSample(d, g_rep, REP_LEN, 1003000, s);
        CHECK(s.pressure == 0.f, "低于 pressMin 的压感被夹取到 0.0（不出现负值）");
    }
    {
        // 橡皮独立 tool 且不带压感字段（旧 Wacom 常见）：
        // gp=false → 走回退，笔尖按下记 1.0，橡皮标志仍须保留
        PenInput pen; MockHid m;
        m.hasP = false;          // 该 tool 的描述符里没有压感
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH, U_DIG_ERASER };
        Install(pen, m);
        Dev d = MakeAbsPenDev(16704, 10506, 1023, false);
        FillReport(0x00);
        PenSample s;
        CHECK(pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s), "橡皮 tool 无压感字段仍能解析");
        CHECK(s.pressure == 1.0f, "橡皮按下无压感时回退为 1.0");
        CHECK(s.eraser, "橡皮标志正确保留（不因缺压感字段而丢失）");
    }
    {
        // Wacom 走形态 0 直读（本机实测 form=0 direct），不进穷举路径，
        // 因此阶段 1 新增的范围校验对 Wacom 完全不生效——钉住这一点，
        // 避免将来有人把校验误加到形态 0 上导致 Wacom 丢数据。
        PenInput pen; MockHid m;
        m.needPrefix = 0xFF;     // 不校验前缀 → 形态 0 直接成功
        m.x = 8000; m.y = 5000;
        m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(16704, 10506, 1023, true);
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedForm == 0, "Wacom 式形态 0 直读，不进穷举");
        CHECK(d.bruteRejected == 0, "形态 0 不触发范围校验（bruteRejected 恒为 0）");
    }

    // ========================================================
    printf("\n-- C. 逻辑坐标范围与映射 --\n");
    // ========================================================
    {
        // 国产品牌普遍把逻辑范围归一化到 0..32767
        PenInput pen; MockHid m;
        m.x = 32767 / 2; m.y = 32767 / 2;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(32767, 32767, 4095, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(Near(s.x, SCREEN_W / 2.0) && Near(s.y, SCREEN_H / 2.0),
              "逻辑范围 0..32767 的中点映射到屏幕中点");
    }
    {
        // Wacom 式非归一范围（各型号各异，如 0..31488）
        PenInput pen; MockHid m;
        m.x = 31488; m.y = 19680;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(31488, 19680, 8191, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.x == SCREEN_W && s.y == SCREEN_H, "非归一范围的最大值映射到屏幕右下角");
    }
    {
        PenInput pen; MockHid m;
        m.x = 0; m.y = 0; m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(20000, 12500, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.x == 0 && s.y == 0, "逻辑原点映射到屏幕原点");
    }

    // ========================================================
    printf("\n-- D. 相对模式（数位板设为鼠标模式） --\n");
    // LogicalMin < 0 即判定为相对模式，累加增量而非绝对定位
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.x = 10; m.y = -5;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(127, 127, 1023, true);
        d.lxMin = -127; d.lyMin = -127;
        d.xRel = true;  d.yRel = true;
        d.haveRelBase = true;      // 预设基准，避免依赖真实光标位置
        d.relX = 500;   d.relY = 400;
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(d.xRel && d.yRel, "LogicalMin<0 的设备标记为相对模式");
        CHECK(s.x == 510 && s.y == 395, "相对模式累加增量（+10, -5）");
        m.x = -20; m.y = 30;
        pen.ExtractSample(d, g_rep, REP_LEN, 1001000, s);
        CHECK(s.x == 490 && s.y == 425, "相对模式支持负增量与连续累加");
    }
    {
        // 边界夹取：不允许跑出虚拟桌面
        PenInput pen; MockHid m;
        m.x = 100000; m.y = 100000;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(127, 127, 1023, true);
        d.lxMin = -127; d.lyMin = -127;
        d.xRel = true; d.yRel = true;
        d.haveRelBase = true; d.relX = 10; d.relY = 10;
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        int vxMax = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vyMax = GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        CHECK(s.x <= vxMax && s.y <= vyMax, "相对模式累加越界时被夹取到虚拟桌面内");
    }

    // ========================================================
    printf("\n-- E. 报文解析形态探测 --\n");
    // 驱动差异：系统可能剥掉 Report ID 前缀，需补回才能解析
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.needPrefix = 0xFF;   // 不校验前缀 → 形态 0 直读即成功
        m.x = 7; m.y = 8; m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedForm == 0, "报文自带前缀时走形态 0 直读并缓存");
        CHECK(vx == 7 && vy == 8, "形态 0 取出正确的 X/Y");
        CHECK(gx && gy, "形态 0 报告 X/Y 均已取到");
    }
    {
        // 形态 1a：能力表里的 Report ID 命中
        PenInput pen; MockHid m;
        m.needPrefix = 0x05;   // 只认前缀 0x05
        m.x = 11; m.y = 12; m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        d.reportIDs = { 0x02, 0x05, 0x09 };   // 含正确 ID
        FillReport(0x00);                     // rep[0]=0，形态 0 必失败
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedForm == 1 && d.parseCachedId == 0x05,
              "形态 1a 用能力表 Report ID 命中并缓存该 ID");
        int callsAfter1a = m.calls;
        // 缓存复用：第二条报文应直接走缓存，不再重新探测
        ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && (m.calls - callsAfter1a) <= 2, "形态缓存复用，后续报文不再穷举探测");
    }
    {
        // 形态 1b：能力表无对应 ID，靠穷举 1..255 命中
        PenInput pen; MockHid m;
        m.needPrefix = 0x7F;
        m.x = 21; m.y = 22; m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        d.reportIDs = { 0x02, 0x03 };   // 不含 0x7F
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedForm == 1 && d.parseCachedId == 0x7F,
              "形态 1b 穷举 1..255 命中正确的 Report ID");
        CHECK(m.calls > 100, "穷举确实逐个尝试（命中 0x7F 前已试上百次）");
    }
    {
        // 全部形态失败
        PenInput pen; MockHid m;
        m.succeed = false;
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        int failsBefore = d.parseFails;
        CHECK(!pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s), "所有形态都失败时 ExtractSample 返回 false");
        CHECK(d.parseCachedForm == -1, "失败后不缓存任何形态");
        (void)failsBefore;
    }
    {
        // 不同报文长度（品牌差异：Wacom 10 字节 / Huion 8 字节 / 6 字节无 Report ID）
        // mock 不关心长度，此处验证 ExtractSample 对各长度都能正常走完链路
        const ULONG lens[] = { 6, 8, 10, 12, 19 };
        bool allOk = true;
        for (ULONG L : lens) {
            PenInput pen; MockHid m;
            m.x = 50; m.y = 50; m.usages = { U_DIG_TIP_SWITCH };
            Install(pen, m);
            Dev d = MakeAbsPenDev(100, 100, 1023, true);
            FillReport(0x00);
            PenSample s;
            if (!pen.ExtractSample(d, g_rep, L, 1000000, s)) allOk = false;
        }
        CHECK(allOk, "6/8/10/12/19 字节报文长度均可解析（长度差异对解析透明）");
    }

    // ========================================================
    printf("\n-- F. 按钮与开关语义 --\n");
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE, U_DIG_ERASER, U_DIG_BARREL_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.tipDown,  "TipSwitch(0x42) → tipDown");
        CHECK(s.inRange,  "InRange(0x32) → inRange");
        CHECK(s.eraser,   "Eraser(0x45) → eraser");
        CHECK(s.barrel,   "BarrelSwitch(0x44) → barrel");
    }
    {
        // 只有 InRange、无 TipSwitch = 悬空
        PenInput pen; MockHid m;
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.inRange && !s.tipDown, "仅 InRange 时为悬空状态（inRange 真、tipDown 假）");
    }
    {
        // 仅 TipSwitch、无 InRange：inRange 应由 tipDown 推导
        PenInput pen; MockHid m;
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.tipDown && s.inRange, "落笔时 inRange 由 tipDown 推导为真");
    }
    {
        // 纯触控屏：只有 Touch(0x33)，没有 TipSwitch(0x42)
        // 阶段 2：触摸数据直接丢弃、不做任何处理。
        // 纯手指触摸（有 Touch 无 TipSwitch）→ ExtractSample 返回 false，
        // 不产出采样、不计入统计、不刷新速度/全局活跃状态。
        PenInput pen; MockHid m;
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TOUCH, U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(32767, 32767, 0, false);
        d.type = PEN_TOUCH_SCREEN;
        FillReport(0x00);
        PenSample s;
        bool produced = pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(!produced, "纯触摸（Touch 无 TipSwitch）被丢弃，不产出采样");
        CHECK(!pen.TipDown() && !pen.InRange(),
              "纯触摸不置全局活跃标志（避免录屏误判为在作画）");
        CHECK(!d.havePrev, "纯触摸不更新速度追踪状态（不污染后续笔速度）");
    }
    {
        // 笔尖 + 触摸并存（部分数位屏笔按下时也会带 Touch）：应保留为笔接触
        PenInput pen; MockHid m;
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH, U_DIG_TOUCH, U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(32767, 32767, 4095, true);
        d.type = PEN_TOUCH_SCREEN;
        FillReport(0x00);
        PenSample s;
        bool produced = pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(produced && s.tipDown, "TipSwitch+Touch 并存时保留为笔接触（tipDown 真）");
        CHECK(s.device == PEN_TOUCH_SCREEN, "设备类型正确透传到采样");
    }


    // ========================================================
    printf("\n-- G. 无压力传感器的设备 --\n");
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.hasP = false;          // 描述符里没有压感字段
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1, false);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.pressure == 1.0f, "无压感设备按下时压力记 1.0");
        m.usages = { U_DIG_IN_RANGE };   // 抬起悬空
        pen.ExtractSample(d, g_rep, REP_LEN, 1001000, s);
        CHECK(s.pressure == 0.0f, "无压感设备悬空时压力记 0.0");
    }

    // ========================================================
    printf("\n-- H. 速度与时间戳 --\n");
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.x = 0; m.y = 0; m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1920, 1080, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.speed == 0.f, "首个采样速度为 0（无前一点可比）");
        CHECK(s.tUs == 1000000, "采样时间戳透传");
        m.x = 100; m.y = 0;   // 移动 100 逻辑单位 = 100 屏幕像素
        pen.ExtractSample(d, g_rep, REP_LEN, 1100000, s);   // dt = 0.1s
        CHECK(s.speed > 0.f, "第二个采样起速度非零");
        CHECK(Near(s.speed, 1000.0, 1.0), "速度 = 像素距离 / 秒（100px / 0.1s = 1000）");
        m.x = 100; m.y = 0;
        pen.ExtractSample(d, g_rep, REP_LEN, 1200000, s);
        CHECK(s.speed == 0.f, "位置不变时速度为 0");
    }

    // ========================================================
    printf("\n-- I. 错位数据（阶段 1 将加范围校验） --\n");
    // 穷举 Report ID 时，错误的前缀可能让 HidP 读出「看似合法」的 X/Y
    // （它只按描述符偏移读字节，不校验数值合理性）。命中后会被永久缓存，
    // 之后所有报文坐标都是错的，而日志形似成功。
    //
    // 【设计】范围校验**只加在穷举路径（形态 1b）**。形态 0 / 1a 的 Report ID
    // 来自设备自己声明的能力表，可信度高；若对它们也强制校验，描述符有小瑕疵
    // 的未知品牌会从「坐标略偏」退化成「完全丢数据」——在无法真机验证的前提下，
    // 这个代价比保留可疑数据更高。故下面两组用例断言不同：
    //   I-1/I-2 走形态 0 → 越界仍被接受（刻意不校验，符合设计）
    //   I-3     走形态 1b 穷举 → 越界必须被拒绝（校验生效）
    //   I-4     走形态 1b 穷举 → 量程内正常采纳
    // ========================================================
    {
        // I-1：形态 0（needPrefix=0xFF 直读）越界 → 刻意接受
        PenInput pen; MockHid m;
        m.x = 999999; m.y = 888888;      // 远超 lxMax/lyMax
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1023, true);
        FillReport(0x00);
        PenSample s;
        bool accepted = pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(accepted, "形态 0 越界 X/Y 仍被接受（设计上不校验可信 Report ID）");
        CHECK(s.x > SCREEN_W * 4, "形态 0 越界逻辑值直接映射出离谱屏幕坐标");
    }
    {
        // I-2：压感越界 → 夹取到 1.0（不产生非法值）
        PenInput pen; MockHid m;
        m.hasP = true; m.p = 5000;       // 超过 pressMax=1023
        m.x = 10; m.y = 10;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.pressure == 1.0f, "越界压感被夹取到 1.0（不产生非法值）");
    }
    {
        // I-3：形态 1b 穷举，命中 ID 但 X/Y 越界 → 必须拒绝，不缓存
        PenInput pen; MockHid m;
        m.needPrefix = 0x40;             // 只有 rid=0x40 时 mock 才成功
        m.x = 999999; m.y = 888888;      // 越界
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1023, true);
        d.reportIDs.clear();             // 清空 1a，强制走 1b 穷举
        FillReport(0x00);                // rep[0]=0，形态 0 必失败
        PenSample s;
        bool accepted = pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(!accepted, "穷举命中 ID 但 X/Y 越界 → 拒绝（不采纳错位数据）");
        CHECK(d.bruteRejected > 0, "越界穷举命中被计入 bruteRejected");
        CHECK(d.parseCachedForm == -1, "拒绝后不缓存任何形态");
    }
    {
        // I-4：形态 1b 穷举，命中 ID 且 X/Y 在量程内 → 正常采纳
        PenInput pen; MockHid m;
        m.needPrefix = 0x40;
        m.x = 500; m.y = 500;            // 量程内
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1023, true);
        d.reportIDs.clear();
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedForm == 1 && d.parseCachedId == 0x40,
              "穷举命中且通过量程校验 → 采纳并缓存该 Report ID");
        CHECK(vx == 500 && vy == 500, "穷举采纳后取出量程内坐标");
    }

    // ========================================================
    printf("\n-- J. 复合设备多 Report ID --\n");
    // Wacom 等复合设备把笔/触摸/ExpressKeys 放在不同 Report ID 上
    // ========================================================
    {
        PenInput pen; MockHid m;
        m.needPrefix = 0x10;            // 只认笔集合的 Report ID
        m.x = 33; m.y = 44;
        m.usages = { U_DIG_TIP_SWITCH, U_DIG_IN_RANGE };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        d.reportIDs = { 0x02, 0x10, 0x21 };   // 触摸 0x02 / 笔 0x10 / 按键 0x21
        FillReport(0x02);                     // 首条是触摸子报文（rep[0]=0x02）
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        // rep[0]=0x02 恰好等于触摸 ID，形态 0 会因前缀不符而失败，
        // 1a 遍历 reportIDs 时用 0x10 前缀才命中笔集合
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && d.parseCachedId == 0x10, "复合设备命中笔集合的 Report ID（非触摸/按键）");
        CHECK(vx == 33 && vy == 44, "复合设备取出笔集合的坐标");
    }

    // ========================================================
    printf("\n-- J2. LinkCollection 精确取值与回退（阶段 1 核心） --\n");
    // 复合设备的笔集合与触摸集合都有 X/Y。FetchWithFallback 应先按
    // d.penLink 精确取笔集合，取不到再退回 link=0（等价改动前行为）。
    // ========================================================
    {
        // 精确取值：X/Y 只在 penLink=3 的集合里
        PenInput pen; MockHid m;
        m.onlyLink = 3;              // 仅 linkCollection==3 时成功
        m.x = 77; m.y = 88;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        d.penLink = 3;               // 压感/TipSwitch 反推出的笔集合
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && vx == 77 && vy == 88, "按 penLink 精确取到笔集合的 X/Y");
        CHECK(d.parseCachedLink == 3, "采纳的 link collection 被缓存");
        CHECK(m.lastLink == 3, "取值确实以 penLink 调用（而非退回 0）");
    }
    {
        // 回退：X/Y 声明在父集合（link=0），penLink 指向的子集合取不到
        PenInput pen; MockHid m;
        m.onlyLink = 0;              // 仅 linkCollection==0 时成功
        m.x = 55; m.y = 66;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 8191, true);
        d.penLink = 5;               // 笔集合是 5，但 X/Y 实际在 0
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && vx == 55 && vy == 66, "penLink 取不到时退回 link=0 仍能解析");
        CHECK(d.parseCachedLink == 0, "回退路径缓存 link=0（等价改动前行为）");
    }
    {
        // penLink=0（单集合设备，如未定位到笔集合）：行为与改动前完全一致
        PenInput pen; MockHid m;
        m.onlyLink = -1;             // 任何 link 都成功
        m.x = 12; m.y = 34;
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        Dev d = MakeAbsPenDev(1000, 1000, 1023, true);
        d.penLink = 0;
        FillReport(0x00);
        USAGE usages[64]; ULONG n = 0;
        LONG vx = 0, vy = 0, vp = 0; bool gx = false, gy = false, gp = false;
        bool ok = pen.TryParseForms(d, g_rep, REP_LEN, usages, n, vx, vy, vp, gx, gy, gp);
        CHECK(ok && vx == 12 && vy == 34, "penLink=0 的单集合设备正常解析");
        CHECK(d.parseCachedLink == 0, "单集合设备 link 恒为 0");
    }


    // ========================================================
    printf("\n-- J3. 指针设备候选消歧（多屏映射，纯函数离线验证） --\n");
    // 多屏下的映射选择无法在单屏开发机上真机验证，故抽成纯函数测试。
    // 依据探针实测：同一块数位屏会暴露两个 PEN 指针条目，一个绑定显示器、
    // 一个 monitor=NULL（映射到整个虚拟桌面），矩形不同，选错会整体偏移。
    // ========================================================
    {
        auto R = [](LONG l, LONG t, LONG r, LONG b) {
            RECT x{}; x.left = l; x.top = t; x.right = r; x.bottom = b; return x;
        };
        bool amb = false;
        // 0 个候选
        CHECK(PenInput::PickPointerCandidate(nullptr, 0, amb) == -1 && !amb,
              "无候选时返回 -1 且不判歧义");

        // 1 个候选：直接采纳（无论是否绑定显示器）
        PenInput::PtrCand one = { R(0, 0, 3840, 2160), true, 1 };
        CHECK(PenInput::PickPointerCandidate(&one, 1, amb) == 0 && !amb,
              "单候选直接采纳");

        // 2 个候选，都绑定显示器且矩形相同 → 无害歧义，采纳第一个
        PenInput::PtrCand same[2] = {
            { R(0,0,3840,2160), true, 3 }, { R(0,0,3840,2160), true, 3 }
        };
        amb = true;
        CHECK(PenInput::PickPointerCandidate(same, 2, amb) == 0 && !amb,
              "多候选但矩形一致 → 采纳（映射结果相同，歧义无害）");

        // 本机真实场景（标定实证）：一个未绑定且矩形=整个虚拟桌面（跨屏），
        // 一个绑定主屏。标定拟合出 sx=7680/32767、ox=-3840，证明驱动真实映射
        // 是跨屏铺展 —— 故必须选跨屏候选，而非绑定者。
        RECT VD = R(-3840, 0, 3840, 2160);   // 显式注入虚拟桌面，测试不依赖真机
        PenInput::PtrCand realCase[2] = {
            { R(-3840,0,3840,2160), false, 2 },   // 跨屏（=虚拟桌面）
            { R(0,0,3840,2160),     true,  1 }    // 绑定主屏
        };
        amb = true;
        int pick = PenInput::PickPointerCandidate(realCase, 2, amb, &VD);
        CHECK(pick == 0 && !amb,
              "跨屏候选优先于绑定候选（标定实证：驱动真实映射是跨屏铺展）");

        // 反过来：绑定者在前，仍应选跨屏者
        PenInput::PtrCand realCase2[2] = {
            { R(0,0,3840,2160),     true,  1 },
            { R(-3840,0,3840,2160), false, 2 }
        };
        amb = true;
        CHECK(PenInput::PickPointerCandidate(realCase2, 2, amb, &VD) == 1 && !amb,
              "绑定候选在首位时仍选跨屏候选");

        // 无跨屏候选时才轮到绑定者：单屏机器上虚拟桌面=那块屏，
        // 绑定候选矩形与虚拟桌面相同，此时它也满足跨屏条件，选它无歧义
        PenInput::PtrCand single[1] = { { R(0,0,1920,1080), true, 1 } };
        RECT VD1 = R(0, 0, 1920, 1080);
        amb = true;
        CHECK(PenInput::PickPointerCandidate(single, 1, amb, &VD1) == 0 && !amb,
              "单屏机器：唯一候选直接采纳");

        // 多个绑定候选但矩形不同、且都不是跨屏 → 真歧义，拒绝猜测
        PenInput::PtrCand conflict[2] = {
            { R(0,0,3840,2160),    true, 1 },
            { R(3840,0,7680,2160), true, 1 }
        };
        amb = false;
        CHECK(PenInput::PickPointerCandidate(conflict, 2, amb, &VD) == -1 && amb,
              "多个绑定候选矩形冲突（非跨屏）→ 判歧义并拒绝猜测");

        // 多个未绑定候选矩形不同、且都不是跨屏 → 同样拒绝猜测
        PenInput::PtrCand nobound[2] = {
            { R(0,0,3840,2160),    false, 1 },
            { R(3840,0,7680,2160), false, 1 }
        };
        amb = false;
        CHECK(PenInput::PickPointerCandidate(nobound, 2, amb, &VD) == -1 && amb,
              "全部未绑定且矩形不同（非跨屏）→ 判歧义并拒绝猜测");

        // 三个候选：跨屏一个 + 两个绑定一致 → 选跨屏的
        PenInput::PtrCand three[3] = {
            { R(-3840,0,3840,2160), false, 2 },
            { R(0,0,3840,2160),     true,  1 },
            { R(0,0,3840,2160),     true,  1 }
        };
        amb = true;
        CHECK(PenInput::PickPointerCandidate(three, 3, amb, &VD) == 0 && !amb,
              "三候选中优先取跨屏者（跳过绑定者）");

        // 两个不同的跨屏候选（理论上不该出现）→ 拒绝猜测
        PenInput::PtrCand twoSpan[2] = {
            { R(-3840,0,3840,2160), false, 2 },
            { R(-7680,0,7680,2160), false, 2 }
        };
        amb = false;
        CHECK(PenInput::PickPointerCandidate(twoSpan, 2, amb, &VD) == -1 && amb,
              "两个不一致的跨屏候选 → 判歧义并拒绝猜测");
    }
    {
        // 设备类型判定：映射与类型分离
        // 场景1（本机 Cintiq Pro 27）：跨屏候选=EXTERNAL_PEN（驱动把跨屏模式
        // 注册成外接笔）、绑定候选=INTEGRATED_PEN（硬件真相是数位屏）。
        // bestIdx 指向跨屏者(0)，但类型应取绑定者(1) → 数位屏。
        int    t1[2] = { POINTER_DEVICE_TYPE_EXTERNAL_PEN, POINTER_DEVICE_TYPE_INTEGRATED_PEN };
        uint8_t m1[2] = { 0, 1 };
        CHECK(PenInput::ResolvePenType(t1, m1, 2, 0) == PEN_INTEGRATED,
              "类型取绑定候选：跨屏被选为映射时，类型仍判定为数位屏（硬件真相）");

        // 场景2：只有跨屏候选（无绑定者）→ 用选中者
        int    t2[1] = { POINTER_DEVICE_TYPE_EXTERNAL_PEN };
        uint8_t m2[1] = { 0 };
        CHECK(PenInput::ResolvePenType(t2, m2, 1, 0) == PEN_EXTERNAL,
              "无绑定候选时类型取选中者");

        // 场景3：触摸（绑定候选与 best 同一个）→ 与旧行为一致
        int    t3[1] = { POINTER_DEVICE_TYPE_TOUCH };
        uint8_t m3[1] = { 1 };
        CHECK(PenInput::ResolvePenType(t3, m3, 1, 0) == PEN_TOUCH_SCREEN,
              "触摸设备类型不受分离规则影响");

        // 场景4：多个绑定候选 → 取第一个绑定者
        int    t4[3] = { POINTER_DEVICE_TYPE_EXTERNAL_PEN,
                         POINTER_DEVICE_TYPE_INTEGRATED_PEN,
                         POINTER_DEVICE_TYPE_TOUCH };
        uint8_t m4[3] = { 0, 1, 1 };
        CHECK(PenInput::ResolvePenType(t4, m4, 3, 0) == PEN_INTEGRATED,
              "多个绑定候选时取第一个绑定者的类型");

        // 场景5：未知类型 → 返回 UNKNOWN，调用方保持原值
        int    t5[1] = { 99 };
        uint8_t m5[1] = { 1 };
        CHECK(PenInput::ResolvePenType(t5, m5, 1, 0) == PEN_UNKNOWN,
              "未知指针类型返回 PEN_UNKNOWN（不改写设备既有类型）");
    }

    // ========================================================
    printf("\n-- J4. 覆写机制解析与校验（阶段 4） --\n");
    // 覆写是「逃生舱口」，风险在于用户写错配置把设备搞坏。
    // 核心保证：单条规则非法只跳过该条、绝不整份失效，且绝不产生非法量程。
    // ========================================================
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 合法规则：十六进制字符串 vid/pid + 量程 + 显示器名
        bool ok = ParseOverrides(R"({"overrides":[
            {"vid":"056A","pid":"03C0","usage":"0D:01",
             "logicalXMin":0,"logicalXMax":32767,"logicalYMin":0,"logicalYMax":32767,
             "pressureMax":8191,"deviceType":"integrated","displayName":"\\\\.\\DISPLAY1"}
        ]})", ovs, errs);
        CHECK(ok && ovs.size() == 1 && errs.empty(), "合法规则解析成功，无错误");
        if (ovs.size() == 1) {
            const PenOverride& o = ovs[0];
            CHECK(o.vid == 0x056A && o.pid == 0x03C0, "十六进制字符串 vid/pid 正确解析");
            CHECK(o.hasUsage && o.usage == 0x01, "\"0D:01\" 解析出尾段 usage=0x01");
            CHECK(o.hasLogical && o.lxMax == 32767, "逻辑量程解析");
            CHECK(o.hasPressureMax && o.pressureMax == 8191, "压感上限解析");
            CHECK(o.hasDeviceType && o.deviceType == PEN_INTEGRATED, "deviceType 字符串 integrated → 数位屏");
            CHECK(o.hasDisplayName && o.displayName.find("DISPLAY1") != std::string::npos, "显示器名解析");
        }
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 数字形式的 vid/pid 同样接受
        ParseOverrides(R"({"overrides":[{"vid":1386,"pid":960,"usage":2}]})", ovs, errs);
        CHECK(ovs.size() == 1 && errs.empty() && ovs[0].vid == 1386 && ovs[0].usage == 2,
              "数字形式 vid/pid/usage 正确解析");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 空文本 = 没有覆写，不算错误
        bool ok = ParseOverrides("", ovs, errs);
        CHECK(ok && ovs.empty() && errs.empty(), "空文本视为无覆写，非错误");
        // 非法 JSON
        ok = ParseOverrides("{ this is not json", ovs, errs);
        CHECK(!ok && !errs.empty(), "非法 JSON 返回失败并给出错误");
        // 缺 overrides 数组（宽容：不崩，提示）
        ovs.clear(); errs.clear();
        ok = ParseOverrides(R"({"foo":1})", ovs, errs);
        CHECK(ok && ovs.empty(), "缺 overrides 数组时不崩，返回空规则");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // UTF-8 BOM：记事本 / PowerShell Set-Content -Encoding UTF8 都会在文件头
        // 写入 EF BB BF。实机验证曾因此解析失败（离线测试传字符串字面量无 BOM，
        // 覆盖不到），修复在 JParse 入口剥 BOM。此处钉住防止回归。
        std::string withBom = std::string("\xEF\xBB\xBF") +
            R"({"overrides":[{"vid":1,"pressureMax":4095}]})";
        bool ok = ParseOverrides(withBom, ovs, errs);
        CHECK(ok && ovs.size() == 1 && errs.empty() && ovs[0].pressureMax == 4095,
              "带 UTF-8 BOM 的覆写文件能正常解析（记事本保存兼容）");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 缺匹配条件：会命中所有设备 → 必须拒绝
        bool ok = ParseOverrides(R"({"overrides":[{"logicalXMin":0,"logicalXMax":100,
            "logicalYMin":0,"logicalYMax":100}]})", ovs, errs);
        CHECK(ok && ovs.empty() && errs.size() == 1, "无匹配条件的规则被拒绝（防止误伤所有设备）");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 非法量程 Max<=Min：会让映射系数为 0/负/除零 → 必须拒绝该条
        ParseOverrides(R"({"overrides":[{"vid":1,"logicalXMin":100,"logicalXMax":50,
            "logicalYMin":0,"logicalYMax":100}]})", ovs, errs);
        CHECK(ovs.empty() && !errs.empty(), "logicalXMax<=Min 的非法量程被拒绝（防除零/负系数）");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 量程四个值只给一半 → 拒绝（量程不完整无法建映射）
        ParseOverrides(R"({"overrides":[{"vid":1,"logicalXMin":0,"logicalXMax":100}]})", ovs, errs);
        CHECK(ovs.empty() && !errs.empty(), "量程四值必须同时给全，缺一即拒绝");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // pressureMax<=0 → 拒绝（否则归一除零）
        ParseOverrides(R"({"overrides":[{"vid":1,"pressureMax":0}]})", ovs, errs);
        CHECK(ovs.empty() && !errs.empty(), "pressureMax<=0 被拒绝（防归一除零）");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 未知 deviceType → 拒绝
        ParseOverrides(R"({"overrides":[{"vid":1,"deviceType":"weird"}]})", ovs, errs);
        CHECK(ovs.empty() && !errs.empty(), "未知 deviceType 字符串被拒绝");
    }
    {
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        // 多条规则：一条非法不影响其余合法条
        bool ok = ParseOverrides(R"({"overrides":[
            {"vid":1,"logicalXMin":100,"logicalXMax":50,"logicalYMin":0,"logicalYMax":100},
            {"vid":2,"pressureMax":4095}
        ]})", ovs, errs);
        CHECK(ok && ovs.size() == 1 && errs.size() == 1 && ovs[0].vid == 2,
              "非法条被跳过，同批合法条仍生效（不整份失效）");
    }
    {
        // 匹配逻辑：AND 关系，未指定即通配
        PenOverride o;
        o.hasVid = true; o.vid = 0x056A;
        o.hasUsage = true; o.usage = 0x01;
        std::string pathLower = "hid#vid_056a&pid_03c0&mi_00&col04";
        CHECK(o.Matches(0x056A, 0x03C0, 0x01, pathLower), "vid+usage 均匹配时命中");
        CHECK(!o.Matches(0x056A, 0x03C0, 0x02, pathLower), "usage 不符时不命中（区分笔/触摸集合）");
        CHECK(!o.Matches(0x2D1F, 0x03C0, 0x01, pathLower), "vid 不符时不命中");
        PenOverride o2; o2.pathContains = "col04";
        CHECK(o2.Matches(0, 0, 0, pathLower), "pathContains 子串匹配（大小写不敏感）");
        CHECK(!o2.Matches(0, 0, 0, "hid#vid_056a&col03"), "pathContains 不含子串时不命中");
    }
    {
        // ApplyOverride 真正生效：覆写量程后设备字段被替换、相对模式重算
        PenInput pen;
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        ParseOverrides(R"({"overrides":[{"vid":1386,"pid":960,
            "logicalXMin":0,"logicalXMax":50000,"logicalYMin":0,"logicalYMax":40000,
            "pressureMax":2047,"deviceType":"integrated"}]})", ovs, errs);
        CHECK(ovs.size() == 1, "为 ApplyOverride 准备的规则解析成功");
        pen.SetOverrides(ovs);
        Dev d = MakeAbsPenDev(32767, 32767, 8191, true);
        d.vid = 1386; d.pid = 960;    // 命中规则
        d.name = "VID_056A&PID_03C0 usage=0D:01";
        pen.ApplyOverride(d);
        CHECK(d.ovApplied, "命中的设备标记 ovApplied");
        CHECK(d.lxMax == 50000 && d.lyMax == 40000, "覆写的逻辑量程已写入设备字段");
        CHECK(d.pressMax == 2047, "覆写的压感上限已写入");
        CHECK(d.type == PEN_INTEGRATED && !d.needPointerType, "覆写的设备类型已写入且不再被指针类型覆盖");
        // 未命中的设备不受影响
        Dev d2 = MakeAbsPenDev(32767, 32767, 8191, true);
        d2.vid = 9999; d2.pid = 9999; d2.name = "other";
        pen.ApplyOverride(d2);
        CHECK(!d2.ovApplied && d2.lxMax == 32767, "未命中的设备字段不被改动");
    }
    {
        // 覆写后的量程要真正参与坐标映射：解析出的坐标随量程缩放
        PenInput pen; MockHid m;
        m.x = 25000; m.y = 20000;   // 落在覆写量程 [0..50000,0..40000] 的中点
        m.usages = { U_DIG_TIP_SWITCH };
        Install(pen, m);
        std::vector<PenOverride> ovs; std::vector<std::string> errs;
        ParseOverrides(R"({"overrides":[{"vid":1386,"pid":960,
            "logicalXMin":0,"logicalXMax":50000,"logicalYMin":0,"logicalYMax":40000}]})", ovs, errs);
        pen.SetOverrides(ovs);
        // 手工构造已映射到 1920x1080 的设备（量程用覆写值）
        Dev d = MakeAbsPenDev(50000, 40000, 1023, true);
        d.vid = 1386; d.pid = 960; d.name = "ov";
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        // 25000/50000=0.5 → 屏幕中点；20000/40000=0.5 → 屏幕中点
        CHECK(Near(s.x, SCREEN_W / 2.0) && Near(s.y, SCREEN_H / 2.0),
              "覆写量程参与映射：中点逻辑值映射到屏幕中点");
    }

    // ========================================================
    printf("\n-- K. 恢复真实实现不受影响 --\n");
    // ========================================================
    {
        // 只验证接缝可反复切换，不调用真实 HidP（假 preparsed 会崩）
        PenInput pen; MockHid m1, m2;
        m1.x = 1; m1.y = 1; m2.x = 2; m2.y = 2;
        Install(pen, m1);
        Dev d = MakeAbsPenDev(100, 100, 1023, true);
        FillReport(0x00);
        PenSample s;
        pen.ExtractSample(d, g_rep, REP_LEN, 1000000, s);
        CHECK(s.logicalX == 1, "首个 mock 生效");
        Install(pen, m2);
        d.parseCachedForm = -1;
        pen.ExtractSample(d, g_rep, REP_LEN, 1001000, s);
        CHECK(s.logicalX == 2, "可切换到另一个 mock");
        pen.SetHidFetchForTest(nullptr);
        CHECK(true, "恢复真实实现不崩溃");
    }

    // ========================================================
    printf("\n-- L. 标定退化解校验（缺陷①，真实报告 diagnostics_20261017_233953） --\n");
    // 报告2实证：最小二乘在退化样本上拟合出 sy≈0.0033（正常≈0.033=1080/32767），
    // 采纳后笔坐标严重错位、污染数据。EvaluateCalibration 用 R² + 斜率合理性双闸拦截。
    // 关键：退化样本 R² 很高（干净线性），只有斜率闸能拦下——这正是旧代码
    // （只查 ay>1e-6）漏掉它的原因。
    // ========================================================
    {
        // 生成 n 个样本：逻辑坐标 0..32767 均匀，屏幕坐标 = 逻辑*斜率 + 交替噪声
        auto gen = [](std::vector<int32_t>& lx, std::vector<int32_t>& sx,
                      std::vector<int32_t>& ly, std::vector<int32_t>& sy,
                      double kx, double ky, int n, int altNoise) {
            lx.clear(); sx.clear(); ly.clear(); sy.clear();
            for (int i = 0; i < n; ++i) {
                double t = (n > 1) ? (double)i / (n - 1) : 0;
                int32_t L = (int32_t)(t * 32767);
                lx.push_back(L); ly.push_back(L);
                int nz = altNoise ? ((i % 2) ? altNoise : -altNoise) : 0;
                sx.push_back((int32_t)llround(L * kx + nz));
                sy.push_back((int32_t)llround(L * ky + nz));
            }
        };
        std::vector<int32_t> lx, sx, ly, sy;
        const double kxOk = 1920.0 / 32767.0;   // ≈0.0586
        const double kyOk = 1080.0 / 32767.0;   // ≈0.0330
        const double LR = 32767;

        // L1 干净线性：应采纳，系数接近真实斜率
        gen(lx, sx, ly, sy, kxOk, kyOk, 64, 0);
        auto r = PenInput::EvaluateCalibration(lx.data(), sx.data(), ly.data(), sy.data(),
                                               lx.size(), LR, LR);
        CHECK(r.accept, "L1 干净线性样本被采纳");
        CHECK(r.accept && fabs(r.sy - kyOk) < 0.002, "L1 采纳的 Y 斜率接近真实值");
        CHECK(r.accept && r.rx2 > 0.99 && r.ry2 > 0.99, "L1 干净样本 R² 接近 1");

        // L2 报告2式退化：Y 斜率病态偏小（跨度仅约108px），R²仍高 → 斜率闸拦
        gen(lx, sx, ly, sy, kxOk, 0.0033, 64, 0);
        r = PenInput::EvaluateCalibration(lx.data(), sx.data(), ly.data(), sy.data(),
                                          lx.size(), LR, LR);
        CHECK(!r.accept, "L2 退化的小斜率被拒绝（报告2 sy≈0.0033 场景）");
        CHECK(!r.accept && r.ry2 > 0.98, "L2 退化样本 R² 仍高——证明是斜率闸而非 R² 闸拦下");

        // L3 强噪声：斜率正常但 R² 低 → R² 闸拦
        gen(lx, sx, ly, sy, kxOk, kyOk, 64, 120);
        r = PenInput::EvaluateCalibration(lx.data(), sx.data(), ly.data(), sy.data(),
                                          lx.size(), LR, LR);
        CHECK(!r.accept && r.ry2 < 0.98, "L3 强噪声样本因 R² 过低被拒绝");

        // L4 斜率异常放大：映射跨度 >40000px → 斜率闸拦
        gen(lx, sx, ly, sy, kxOk, 5.0, 64, 0);
        r = PenInput::EvaluateCalibration(lx.data(), sx.data(), ly.data(), sy.data(),
                                          lx.size(), LR, LR);
        CHECK(!r.accept, "L4 斜率异常放大（跨度过大）被拒绝");

        // L5 样本不足（<16）
        gen(lx, sx, ly, sy, kxOk, kyOk, 10, 0);
        r = PenInput::EvaluateCalibration(lx.data(), sx.data(), ly.data(), sy.data(),
                                          lx.size(), LR, LR);
        CHECK(!r.accept, "L5 样本不足（<16）被拒绝");
    }

    // ========================================================
    printf("\n-- M. 设备类型初判（缺陷④，触控板误标触摸屏，报告2/3） --\n");
    // 报告2/3 实证：Elan 04F3、VID_256C 的 usage=0D:05（Touch Pad）被误标为
    // 「触摸屏」，还参与笔映射（mapSrc=2 虚拟桌面兜底）。ClassifyDeviceType
    // 现单独识别触控板，OnRawInput 早退排除出笔通道。
    // ========================================================
    {
        // 0x05 触控板：无压力 → 触控板（不是触摸屏），不需指针类型细分
        auto r = PenInput::ClassifyDeviceType(0x05, false);
        CHECK(r.type == PEN_TOUCH_PAD, "M1 usage=0D:05 无压力 → 触控板（此前误标触摸屏）");
        CHECK(r.type == PEN_TOUCH_PAD && !r.needPointerType, "M1 触控板不需指针类型细分");
        // 0x05 即使误报有压力也应是触控板（topUsage 优先）
        r = PenInput::ClassifyDeviceType(0x05, true);
        CHECK(r.type == PEN_TOUCH_PAD, "M2 usage=0D:05 有压力仍判触控板（topUsage 优先）");

        // 0x04 触摸屏：仍是触摸屏，不回归
        r = PenInput::ClassifyDeviceType(0x04, false);
        CHECK(r.type == PEN_TOUCH_SCREEN, "M3 usage=0D:04 → 触摸屏（不受触控板改动影响）");

        // 0x02 笔 / 0x0C 多点：外接数位板 + 待指针类型细分
        r = PenInput::ClassifyDeviceType(0x02, true);
        CHECK(r.type == PEN_EXTERNAL && r.needPointerType, "M4 usage=0D:02 → 外接数位板，待指针细分");
        r = PenInput::ClassifyDeviceType(0x0C, true);
        CHECK(r.type == PEN_EXTERNAL && r.needPointerType, "M5 usage=0D:0C 多点 → 外接数位板，待指针细分");

        // 未知 topUsage：有压力→外接板、无压力→触摸屏（保留原兜底语义）
        r = PenInput::ClassifyDeviceType(0x99, true);
        CHECK(r.type == PEN_EXTERNAL, "M6 未知 usage 有压力 → 外接数位板（兜底）");
        r = PenInput::ClassifyDeviceType(0x99, false);
        CHECK(r.type == PEN_TOUCH_SCREEN, "M7 未知 usage 无压力 → 触摸屏（兜底）");

        // 触控板类型名
        CHECK(std::string(PenInput::DeviceTypeName(PEN_TOUCH_PAD)) == "触控板",
              "M8 触控板类型名为「触控板」");
    }

    // ========================================================
    printf("\n-- N. 报文日志决策（缺陷③，报告1 XP-Pen 前5条全悬空） --\n");
    // 旧逻辑只记前 5 条，XP-Pen 前 5 条全悬空(tip=0) → 落笔数据再也不记，
    // 无法看出该设备落笔时压感/坐标是否正常。新逻辑记「前3条基线 + 跳变」。
    // ========================================================
    {
        ReportLogState st;
        const char* tag = "";
        // 前 3 条无条件记（基线）
        CHECK(PenInput::DecideReportLog(st, false, 0.f, false, tag), "N1 第1条(悬空)记为基线");
        CHECK(PenInput::DecideReportLog(st, false, 0.f, false, tag), "N2 第2条(悬空)记为基线");
        CHECK(PenInput::DecideReportLog(st, false, 0.f, false, tag), "N3 第3条(悬空)记为基线");
        // 基线用尽后，纯悬空不再记（这正是报告1的问题：额度被悬空占满）
        CHECK(!PenInput::DecideReportLog(st, false, 0.f, false, tag), "N4 第4条悬空不再记（基线已用尽）");
        CHECK(!PenInput::DecideReportLog(st, false, 0.f, false, tag), "N5 第5条悬空不再记");
        // 关键：落笔跳变必须记——这是旧逻辑漏掉的诊断盲点
        CHECK(PenInput::DecideReportLog(st, true, 0.5f, false, tag), "N6 首次落笔(tip0->1)被记录");
        CHECK(std::string(tag).find("落笔") != std::string::npos, "N6 落笔跳变的 tag 标注正确");
        // 连续落笔不重复记（只记首次）
        CHECK(!PenInput::DecideReportLog(st, true, 0.6f, false, tag), "N7 第2次落笔不重复记");
        CHECK(!PenInput::DecideReportLog(st, true, 0.7f, false, tag), "N8 第3次落笔不重复记");
        // 抬笔跳变记一次
        CHECK(PenInput::DecideReportLog(st, false, 0.f, false, tag), "N9 首次抬笔(tip1->0)被记录");
        CHECK(!PenInput::DecideReportLog(st, false, 0.f, false, tag), "N10 抬笔后再悬空不重复记");
    }
    {
        // 橡皮跳变：独立于落笔/压感，首次出现要记
        ReportLogState st; const char* tag = "";
        PenInput::DecideReportLog(st, true, 0.5f, false, tag);   // baseline1
        PenInput::DecideReportLog(st, true, 0.5f, false, tag);   // baseline2
        PenInput::DecideReportLog(st, true, 0.5f, false, tag);   // baseline3（顺带触发落笔+压感）
        // 基线阶段已置 sawTipDown/sawPressure，橡皮标志仍独立
        CHECK(PenInput::DecideReportLog(st, true, 0.5f, true, tag), "N11 首次橡皮被记录");
        CHECK(!PenInput::DecideReportLog(st, true, 0.5f, true, tag), "N12 第2次橡皮不重复记");
    }
    {
        // 无压感设备：基线用尽后靠 tip 跳变记录（压感恒0不触发 sawPressure，
        // 但落笔跳变仍记，保证无压感设备的落笔也能被诊断看到）
        ReportLogState st; const char* tag = "";
        for (int i = 0; i < 3; ++i) PenInput::DecideReportLog(st, false, 0.f, false, tag);
        CHECK(PenInput::DecideReportLog(st, true, 0.f, false, tag),
              "N13 无压感设备的落笔跳变仍被记录（压感恒0不影响 tip 跳变）");
    }

    // ========================================================
    printf("\n-- O. 诊断日志扫描（缺陷②，报告1 会话口径自相矛盾） --\n");
    // 报告1：采集计数是「当前会话」(penRawReports=0)，日志摘录是「历史」，
    // 却武断提示「从未收到任何数位设备报文」，紧接着下方日志就有上一会话的 OK 行。
    // ScanPenLog 扫描日志区分二者，让导出报告措辞准确。
    // ========================================================
    {
        // 空日志
        auto sc = PenInput::ScanPenLog("");
        CHECK(sc.sessions == 0 && !sc.everGotReport && sc.okLines == 0,
              "O1 空日志：无会话、无笔活动");

        // 报告1 场景：两个会话，历史会话有 OK 行，当前会话只有注册行
        std::string log1 =
            "[2026-10-17 23:03:54] [pen] registered digitizer usages: PAGEONLY 0x0D/*; 4 device(s)\n"
            "[2026-10-17 23:09:16] [pen] OK(baseline) dev=VID_00FF&PID_BACC -> x=2437 tip=0\n"
            "[2026-10-17 23:09:16] [pen] OK(首次落笔 tip0->1) dev=VID_00FF&PID_BACC -> x=2440 tip=1\n"
            "[2026-10-17 23:30:19] [cap] thread stopped\n"
            "[2026-10-17 23:30:23] [pen] registered digitizer usages: PAGEONLY 0x0D/*; 4 device(s)\n"
            "[2026-10-17 23:30:23] [cap] thread started: kb=1 mouse=1 pen=1 fg=1\n";
        sc = PenInput::ScanPenLog(log1);
        CHECK(sc.sessions == 2, "O2 报告1场景：识别出 2 个会话（以 registered 行为界）");
        CHECK(sc.everGotReport, "O2 历史中曾成功解析笔报文（有 OK 行）→ 提示不应说「从未收到」");
        CHECK(sc.okLines == 2, "O2 OK 行计数为 2");

        // 只有注册、从无 OK 行：真正「从未解析到笔」
        std::string log2 =
            "[2026-10-17 23:03:54] [pen] registered digitizer usages: PAGEONLY 0x0D/*; 4 device(s)\n"
            "[2026-10-17 23:03:54] [pen] pointer match OK: logical=... (VID_28BD&PID_096A)\n"
            "[2026-10-17 23:09:16] [pen] report from device VID_00FF&PID_BACC preparsed=1\n";
        sc = PenInput::ScanPenLog(log2);
        CHECK(sc.sessions == 1 && !sc.everGotReport, "O3 无 OK 行：everGotReport=false（可安全提示从未解析到笔）");
        CHECK(sc.everParsedDev, "O3 但有 report from device 行：设备曾上报报文（区分「没报文」与「有报文没解析出」）");

        // 干扰行：pointer match OK / parse FAIL 都含 "OK"/"pen" 但不是成功报文行
        std::string log3 =
            "[t] [pen] pointer match OK: logical=[0..32767] (VID_056A)\n"
            "[t] [pen] parse FAIL #1 dev=VID_056A raw=[..]\n"
            "[t] [pen] skip: 触控板非绘图设备 dev=VID_04F3\n";
        sc = PenInput::ScanPenLog(log3);
        CHECK(!sc.everGotReport && sc.okLines == 0,
              "O4 pointer match OK / parse FAIL 不误计为成功笔报文（只认 [pen] OK 前缀）");

        // 非 [pen] 行不参与扫描（[cap]/[fg]/[rec] 等）
        std::string log4 =
            "[t] [cap] thread started\n"
            "[t] [rec] session started 3840x2160\n"
            "[t] [fg] exe=photoshop.exe\n";
        sc = PenInput::ScanPenLog(log4);
        CHECK(sc.sessions == 0 && !sc.everGotReport && !sc.everParsedDev,
              "O5 非 [pen] 行不参与扫描（避免 registered 之外的行误判会话）");
    }

    // ============================================================
    // P. 数位笔注入识别（PenInject）——签名无关判据
    //
    // 背景：真实用户报告 diagnostics_20261018_144042/_144046 显示
    // penRawReports=0 且 mouseStrokes=73/123、penStrokes=0，而 undo=7、
    // redo=30 证明用户确实在作画 —— 笔被 Ink/厂商驱动接管后以注入鼠标
    // 事件出现，但不带 FROMTOUCH 签名，于是全被记成鼠标笔画。
    // 本节验证新增的 IMDT_PEN 判据能识别这类事件，且不误伤真实鼠标。
    // ============================================================
    {
        PenPositionMatch noPos;   // 无位置吻合上下文

        // P1 标准路径回归：FROMTOUCH 签名仍被识别为笔（本机 Wacom 走这条）
        InjectEvidence e1;
        e1.extraInfo = PB_INJ_SIG_FROMTOUCH | 7;   // 签名 + pointerId=7
        e1.srcOk = 0;                              // 刻意不提供 IMS：只靠签名
        InjectReason r1 = InjectReason::kNotPen;
        CHECK(IsPenInjectedEvent(e1, true, &noPos, &r1) && r1 == InjectReason::kFromTouchSignature,
              "P1 FROMTOUCH 签名被识别为笔（标准路径不回归）");

        // P2 核心修复：无签名，但系统输入源报告为笔 → 识别为笔
        InjectEvidence e2;
        e2.extraInfo = 0x00000000;                 // 无签名
        e2.flags = PB_INJ_LLMHF_INJECTED;
        e2.srcOk = 1;
        e2.deviceType = PB_INJ_IMDT_PEN;           // 系统认定来源是笔
        e2.originId = 0x00000004;                  // IMO_SYSTEM
        InjectReason r2 = InjectReason::kNotPen;
        CHECK(IsPenInjectedEvent(e2, true, &noPos, &r2) && r2 == InjectReason::kImsDevicePen,
              "P2 无签名但 IMDT_PEN 被识别为笔（修复用户报告的漏识别）");

        // P3 deviceType 是位掩码：PEN|TOUCH 组合仍应判为笔
        InjectEvidence e3 = e2;
        e3.deviceType = PB_INJ_IMDT_PEN | PB_INJ_IMDT_TOUCH;
        CHECK(IsPenInjectedEvent(e3, true, &noPos, nullptr),
              "P3 deviceType 含 PEN 位即判为笔（位掩码语义正确）");

        // P4 关键：真实鼠标不能被误判为笔（否则鼠标操作会被记成笔画）
        InjectEvidence e4;
        e4.extraInfo = 0x00000000;
        e4.flags = PB_INJ_LLMHF_INJECTED;          // 远程桌面/宏软件也会注入
        e4.srcOk = 1;
        e4.deviceType = PB_INJ_IMDT_MOUSE;         // 但来源是鼠标
        e4.originId = 0x00000002;                  // IMO_INJECTED（普通应用注入）
        InjectReason r4 = InjectReason::kFromTouchSignature;
        CHECK(!IsPenInjectedEvent(e4, true, &noPos, &r4) && r4 == InjectReason::kNotPen,
              "P4 注入的普通鼠标事件不被误判为笔");

        // P5 硬件鼠标（非注入）不判为笔
        InjectEvidence e5;
        e5.extraInfo = 0; e5.flags = 0; e5.srcOk = 1;
        e5.deviceType = PB_INJ_IMDT_MOUSE; e5.originId = 0x00000001;   // IMO_HARDWARE
        CHECK(!IsPenInjectedEvent(e5, true, &noPos, nullptr),
              "P5 硬件鼠标不判为笔");

        // P6 位置吻合兜底仍有效：无签名、无 IMS，但近期有笔采样且坐标接近
        InjectEvidence e6;
        e6.extraInfo = 0; e6.flags = PB_INJ_LLMHF_INJECTED; e6.srcOk = 0;
        PenPositionMatch pos;
        pos.recentPenSample = true;
        pos.mx = 1000; pos.my = 2000;
        pos.lastPenX = 1003; pos.lastPenY = 1998;   // 距离平方 = 9+4 = 13 <= 100
        InjectReason r6 = InjectReason::kNotPen;
        CHECK(IsPenInjectedEvent(e6, false, &pos, &r6) && r6 == InjectReason::kPositionMatch,
              "P6 位置吻合兜底仍识别为笔（原有路径不回归）");

        // P7 位置吻合但坐标相差过远 → 不判为笔（阈值生效）
        // 注意：变量名不能用 far —— 它是 windef.h 里的遗留宏（#define far），
        // 展开后变量名消失，会报 C2513。
        PenPositionMatch farPos = pos;
        farPos.mx = 1000; farPos.my = 2000;
        farPos.lastPenX = 1100; farPos.lastPenY = 2100;   // 距离平方 = 20000 > 100
        CHECK(!IsPenInjectedEvent(e6, false, &farPos, nullptr),
              "P7 位置不吻合时兜底不生效（阈值有效）");

        // P8 兜底需要「近期确有笔采样」：recentPenSample=false 时不判为笔。
        // 这正是用户报告里兜底永久失效的原因（penRawReports=0 → 无采样），
        // 现在由 P2 的 IMS 判据补上，兜底本身的保守语义保持不变。
        PenPositionMatch stale = pos;
        stale.recentPenSample = false;
        CHECK(!IsPenInjectedEvent(e6, true, &stale, nullptr),
              "P8 无近期笔采样时兜底不生效（保守语义保持）");

        // P9 IMS 查询失败时退回签名/兜底判定，不因 srcOk=0 崩溃或误判
        InjectEvidence e9;
        e9.extraInfo = 0; e9.flags = PB_INJ_LLMHF_INJECTED;
        e9.srcOk = 0;                              // GetCurrentInputMessageSource 失败
        e9.deviceType = 0xFFFFFFFF;                // 脏数据，不应被采信
        CHECK(!IsPenInjectedEvent(e9, true, &noPos, nullptr),
              "P9 IMS 查询失败时不采信 deviceType 脏数据");

        // P10 触摸判定：确证 TOUCH 且不含 PEN → 是手指（应丢弃）
        InjectEvidence t1;
        t1.extraInfo = PB_INJ_SIG_FROMTOUCH | 3;
        t1.srcOk = 1; t1.deviceType = PB_INJ_IMDT_TOUCH;
        CHECK(IsTouchEvent(t1, 3, PB_INJ_PT_TOUCH, true),
              "P10 确证 TOUCH 判为手指触摸");

        // P11 触摸判定：含 PEN 位时不算手指（笔优先，避免误伤笔）
        InjectEvidence t2 = t1;
        t2.deviceType = PB_INJ_IMDT_PEN | PB_INJ_IMDT_TOUCH;
        CHECK(!IsTouchEvent(t2, 3, PB_INJ_PT_TOUCH, true),
              "P11 deviceType 含 PEN 时不判为手指（笔优先）");

        // P12 触摸判定：类型不明确时保守返回 false（当作笔，不误丢）
        InjectEvidence t3;
        t3.extraInfo = PB_INJ_SIG_FROMTOUCH | 5;
        t3.srcOk = 1; t3.deviceType = PB_INJ_IMDT_MOUSE;   // 不明确
        CHECK(!IsTouchEvent(t3, 5, 0, false),
              "P12 输入源类型不明确时不判为手指（保守）");

        // P13 触摸判定：IMS 不可用时退回 pointerType
        InjectEvidence t4;
        t4.extraInfo = PB_INJ_SIG_FROMTOUCH | 9;
        t4.srcOk = 0;
        CHECK(IsTouchEvent(t4, 9, PB_INJ_PT_TOUCH, true) &&
              !IsTouchEvent(t4, 9, 0 /*PT_POINTER*/, true),
              "P13 IMS 不可用时用 pointerType 判定触摸");

        // P14 原因名可打印（日志用，非空且各不相同）
        CHECK(InjectReasonName(InjectReason::kImsDevicePen)[0] != '\0' &&
              InjectReasonName(InjectReason::kNotPen)[0] != '\0',
              "P14 判定原因名可用于日志");
    }

    // ==================== Q. 最后一层回退门控 ====================
    // 场景（用户复现 + 两份报告实证）：CSP 开启 Wintab 后驱动独占笔尖通道，
    // Raw Input 只剩悬停报文，驱动另行合成普通鼠标事件（无 FROMTOUCH 签名、
    // 系统输入源报 IMDT_MOUSE、内核注入不带 LLMHF_INJECTED）→ 三判据全落空，
    // 笔画被记成鼠标。门控是纯函数，因为该环境无法在开发机复现（本机 Wacom 正常）。
    {
        // Q1 报告 20261018_155346 的状态：有笔设备、759 条采样全是悬停
        // （everPenTipDown=false）、注入路径没认出来 → 必须启用回退。
        MouseFallbackCtx c1;
        c1.hasPenDevice = true; c1.everPenTipDown = false; c1.everInjectedPen = false;
        c1.penInRangeRecently = true; c1.eventSynthesized = false;
        CHECK(ShouldUseMouseFallback(c1), "Q1 悬停采样不阻止回退（报告 155346 状态）");

        // Q2 报告 20261018_155341 的状态：penRawReports=0（笔完全静默）、
        // 注册回读正常、内核注入不带 INJECTED 标志 → 两条正向证据都不成立，
        // 仍必须启用回退。这是把正向证据从「硬门槛」降为「仅留证」的原因。
        MouseFallbackCtx c2;
        c2.hasPenDevice = true; c2.everPenTipDown = false; c2.everInjectedPen = false;
        c2.penInRangeRecently = false; c2.eventSynthesized = false;
        CHECK(ShouldUseMouseFallback(c2), "Q2 笔完全静默且无注入标志时仍启用回退（报告 155341 状态）");

        // Q3 否决①：没有数位笔设备。用户用的就是真鼠标，记成笔是纯粹错报。
        MouseFallbackCtx c3 = c2;
        c3.hasPenDevice = false;
        CHECK(!ShouldUseMouseFallback(c3), "Q3 无数位笔设备时不启用回退");

        // Q4 否决②：收到过真实落笔数据 → 原始通道工作正常，绝不能抢它的记录。
        // 这是保护开发者本机（Wacom 正常）与所有健康环境的关键判据。
        MouseFallbackCtx c4 = c2;
        c4.everPenTipDown = true;
        CHECK(!ShouldUseMouseFallback(c4), "Q4 已有真实落笔数据时不启用回退");

        // Q5 否决③：注入路径已识别出笔 → 那条路能拿到压感与橡皮，语义更准，
        // 不应退化到有损回退。
        MouseFallbackCtx c5 = c2;
        c5.everInjectedPen = true;
        CHECK(!ShouldUseMouseFallback(c5), "Q5 注入路径已识别笔时不启用回退");

        // Q6 三条否决同时不满足（有笔设备 + 有落笔 + 注入也认出来了）→ 关闭。
        MouseFallbackCtx c6;
        c6.hasPenDevice = true; c6.everPenTipDown = true; c6.everInjectedPen = true;
        CHECK(!ShouldUseMouseFallback(c6), "Q6 健康环境三条件齐备时不启用回退");

        // Q7 默认构造（全 false）→ hasPenDevice=false 即否决，不会误开。
        // 防止「设备还没枚举完就先来了鼠标按下」时把普通点击记成笔。
        MouseFallbackCtx c7;
        CHECK(!ShouldUseMouseFallback(c7), "Q7 默认上下文不启用回退（设备未枚举时安全）");
    }

    printf("\n==================== 结果 ====================\n");
    printf("结果：%d 通过，%d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("[OK] 解析层离线测试全部通过\n");
    else             printf("[FAILED] 有 %d 项未通过\n", g_fail);
    printf("==============================================\n");
    return g_fail == 0 ? 0 : 1;
}
