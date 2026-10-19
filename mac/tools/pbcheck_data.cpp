// ============================================================
//  pbcheck_data —— 数据兼容性验收工具（无头，CI 可跑，不需要 TCC 权限）
//
//  用途：证明「macOS 端对 jsonl 数据格式的理解与 Windows 端完全一致」。
//
//  为什么用往返测试而不是「与 Windows 端基线 JSON 比对」：
//    开发机是 Windows 且**没有安装任何 C++ 编译器**（clang/gcc/cl/cmake
//    全部缺失，已实测），无法先在 Windows 上生成基线文件。
//    而往返测试只依赖 Store.cpp 自己的 ParseEventLine / SerializeEvent
//    —— 两者是同一份源码里的互逆函数（Store.h:82-83 明确说明
//    ParseEventLine 的用途就是「验证落盘→重载往返一致」）。
//
//  测试逻辑（语义往返，而非字节相等）：
//    读 Windows 端录制的真实 jsonl 每一行
//      → ParseEventLine 解析成 RawRecord A
//      → SerializeEvent 重新序列化
//      → ParseEventLine 再解析成 RawRecord B
//      → A 与 B **逐字段比对**（含 pts/ppts 轨迹数组的每个元素）
//
//  ⚠️ 为什么不能要求「字节相等」（曾经这么写，是错的）：
//    SerializeRecord 对 type='p' **无条件**写 did 字段（Store.cpp:551），
//    但样本数据里的旧记录没有 did（该字段是后来加的，ParseRecord:583
//    的注释明确写了「旧日志无 did 字段 → 默认 0」）。
//    于是字节比对会在**实现完全正确**的情况下判红。
//    语义往返才是真正要验证的东西：数据经过「解析→序列化→再解析」
//    一个循环后所有字段无损 —— 这正是跨端互通的充分条件
//    （Windows 写 → Mac 读 → Mac 写 → Windows 读）。
//
//    字节差异仍会被统计并打印，作为信息项（不判失败），
//    用于人工确认差异只出现在已知的 did 补写上。
//
//  ⚠️ 必须调用 OverrideDataDir() 把数据目录重定向到隔离位置。
//     Common.h:351-356 说明得很清楚：合成数据生成器与测试工具绝不能写
//     用户真实数据目录，否则测试记录会与真实绘画记录混在同一个 .jsonl 里，
//     只能靠时间戳逐条甄别才能清除。
//
//  用法：
//    pbcheck_data <sampleDir> roundtrip   # 往返测试（默认）
//    pbcheck_data <sampleDir> analysis    # 输出分析结果 JSON
//
//  退出码：0 = 通过；1 = 有不一致（CI 据此判红）；2 = 无法读取数据。
// ============================================================

#include "Common.h"
#include "Store.h"
#include "Config.h"
#include "Analyzer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>

using namespace pb;

namespace {

// ------------------------------------------------------------
//  输出必须**确定性**：不含时间戳、不含指针值、不含本机路径。
//  否则 CI 每次跑的 diff 都不一样，比对就失去意义。
// ------------------------------------------------------------

// 浮点格式化：与 Json.cpp:43 的 %.9g 保持一致。
// 若这里用了别的精度，输出的分析 JSON 会与 Windows 端不可比。
std::string F(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.9g", v);
    return std::string(buf);
}

std::string I(long long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    return std::string(buf);
}

// 去掉行尾的 \r 与 \n。Store.cpp:644 的读取端也做了同样的 strip，
// 因为 jsonl 可能由 Windows 端以 CRLF 写入。
void StripEol(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// 读整个 jsonl 文件为行数组（保留原始字节，用于逐字节比对）
//
// ⚠️ 必须走垫片的 ReadWholeFile 而非 std::ifstream：OverrideDataDir 设的
//    g_logDir 含反斜杠（Store.cpp:142 用 base + L"\\logs" 拼接），而 macOS 上
//    反斜杠是合法文件名字符、不是分隔符。std::ifstream 不归一化路径，会把
//    "sampledata\logs" 当成一个含反斜杠的目录名 → 打不开文件（return 2）。
//    ReadWholeFile 是垫片实现、内部 NormPath 把 '\' 转 '/'，与 Store.cpp 的
//    LoadDay 走同一条读取路径，故两者对同一天文件的可见性一致。
bool ReadLines(const std::string& path, std::vector<std::string>& out) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;
    // 手动按行切分（与 Store.cpp:638-654 的 LoadDay 逻辑一致）
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(i, end - i);
        StripEol(line);                        // 去掉行尾 \r\n
        if (!line.empty()) out.push_back(line);
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return true;
}

// ------------------------------------------------------------
//  RawRecord 逐字段比对
//
//  必须覆盖 RawRecord 的**每一个**字段（Common.h:306-329）。
//  漏比某个字段，就意味着该字段在跨端往返时丢了也不会被发现 ——
//  而「静默丢字段」正是这个项目历史上反复出现的问题形态
//  （键盘归属 kbAttr 曾被漏写、压感曾因字段错位恒为 0）。
// ------------------------------------------------------------
bool FloatEq(float a, float b) {
    if (a == b) return true;                       // 含两者都是 0
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    // 浮点经 %.9g 序列化再解析，可能有极小的舍入差；
    // 用相对容差而非 == 。9 位有效数字对 float（约 7 位）是过量的，
    // 故正常情况下应严格相等，容差只兜底边界。
    float d = std::fabs(a - b);
    float m = std::max(std::fabs(a), std::fabs(b));
    return d <= m * 1e-6f;
}

// 返回第一个不一致的字段名；全一致返回空串。
std::string DiffRecord(const RawRecord& a, const RawRecord& b) {
    #define CK(cond, name) do { if (!(cond)) return std::string(name); } while (0)

    CK(a.tUs      == b.tUs,      "t");
    CK(a.type     == b.type,     "ty");
    CK(a.kind     == b.kind,     "kind");
    CK(a.vk       == b.vk,       "vk");
    CK(a.mods     == b.mods,     "mods");
    CK(a.count    == b.count,    "count");
    CK(a.endUs    == b.endUs,    "e");
    CK(a.button   == b.button,   "btn");
    CK(a.device   == b.device,   "dev");
    CK(a.deviceId == b.deviceId, "did");
    CK(a.eraser   == b.eraser,   "era");
    CK(a.kbAttr   == b.kbAttr,   "kb");
    CK(a.hover    == b.hover,    "hov");
    CK(FloatEq(a.maxPressure, b.maxPressure), "maxp");
    CK(FloatEq(a.avgPressure, b.avgPressure), "avgp");
    CK(FloatEq(a.maxSpeed,    b.maxSpeed),    "maxs");
    CK(FloatEq(a.avgSpeed,    b.avgSpeed),    "avgs");
    CK(FloatEq(a.pathLen,     b.pathLen),     "len");
    CK(a.exe      == b.exe,      "exe");
    CK(a.title    == b.title,    "title");
    CK(a.isTarget == b.isTarget, "tgt");

    // 鼠标轨迹点
    CK(a.mpts.size() == b.mpts.size(), "mpts.size");
    for (size_t i = 0; i < a.mpts.size() && i < b.mpts.size(); ++i) {
        const auto& x = a.mpts[i];
        const auto& y = b.mpts[i];
        CK(x.dtUs == y.dtUs, "mpts[].dtUs");
        CK(x.x    == y.x,    "mpts[].x");
        CK(x.y    == y.y,    "mpts[].y");
        CK(FloatEq(x.speed, y.speed), "mpts[].speed");
    }

    // 数位笔轨迹点（含 pressure 与 flags）
    CK(a.ppts.size() == b.ppts.size(), "ppts.size");
    for (size_t i = 0; i < a.ppts.size() && i < b.ppts.size(); ++i) {
        const auto& x = a.ppts[i];
        const auto& y = b.ppts[i];
        CK(x.dtUs     == y.dtUs,     "ppts[].dtUs");
        CK(x.x        == y.x,        "ppts[].x");
        CK(x.y        == y.y,        "ppts[].y");
        CK(FloatEq(x.speed,    y.speed),    "ppts[].speed");
        CK(FloatEq(x.pressure, y.pressure), "ppts[].pressure");
        CK(x.flags    == y.flags,    "ppts[].flags");
    }

    #undef CK
    return std::string();
}

int CmdRoundtrip(const std::string& sampleDir) {
    std::vector<std::string> days = ListDays();
    if (days.empty()) {
        fprintf(stderr, "[pbcheck_data] 样本目录里没有 .jsonl: %s\n", sampleDir.c_str());
        fprintf(stderr, "  （请确认样本目录含 logs/YYYYMMDD.jsonl）\n");
        return 2;
    }

    long long total = 0, semanticOK = 0, semanticBad = 0, parseFail = 0;
    long long byteEqual = 0, byteDiff = 0;
    std::vector<std::string> samples;

    for (const auto& day : days) {
        std::string path = LogDir() + "/" + day + ".jsonl";
        std::vector<std::string> lines;
        if (!ReadLines(path, lines)) {
            fprintf(stderr, "[pbcheck_data] 读不到 %s\n", path.c_str());
            return 2;
        }

        for (const auto& orig : lines) {
            ++total;

            // A：原始行 → RawRecord
            RawRecord A;
            if (!ParseEventLine(orig, A)) {
                ++parseFail;
                if (samples.size() < 5)
                    samples.push_back("解析失败: " + orig.substr(0, 120));
                continue;
            }

            // 原始行 → 序列化行
            std::string again = SerializeEvent(A);
            StripEol(again);
            if (again == orig) ++byteEqual; else ++byteDiff;

            // B：序列化行 → RawRecord
            RawRecord B;
            if (!ParseEventLine(again, B)) {
                ++semanticBad;
                if (samples.size() < 5)
                    samples.push_back("重序列化后无法解析: " + again.substr(0, 120));
                continue;
            }

            // 逐字段比对 A 与 B
            std::string diff = DiffRecord(A, B);
            if (diff.empty()) {
                ++semanticOK;
            } else {
                ++semanticBad;
                if (samples.size() < 5) {
                    std::ostringstream os;
                    os << "字段丢失/变化 [" << diff << "]: "
                       << orig.substr(0, 160);
                    samples.push_back(os.str());
                }
            }
        }
    }

    printf("========== 数据往返测试（语义） ==========\n");
    printf("样本目录       : %s\n", sampleDir.c_str());
    printf("日期文件数     : %d\n", (int)days.size());
    printf("事件总行数     : %lld\n", total);
    printf("-- 判据（语义往返，A==B 逐字段） --\n");
    printf("字段全一致     : %lld\n", semanticOK);
    printf("解析失败       : %lld\n", parseFail);
    printf("字段丢失/变化  : %lld\n", semanticBad);
    printf("-- 信息项（不判失败） --\n");
    printf("字节完全相等   : %lld\n", byteEqual);
    printf("字节有差异     : %lld\n", byteDiff);
    printf("=========================================\n");

    if (!samples.empty()) {
        printf("\n---- 前 %d 条问题明细 ----\n", (int)samples.size());
        for (const auto& s : samples) printf("%s\n", s.c_str());
    }

    // 字节差异是预期的：旧记录缺 did，SerializeRecord 会补写（Store.cpp:551）。
    // 故只用「语义往返全一致」作为通过判据。
    bool ok = (total > 0) && (parseFail == 0) && (semanticBad == 0);
    if (ok) {
        printf("\n✅ 通过：解析→序列化→再解析，所有字段无损\n");
        printf("   含义：Windows 录的历史数据可被 Mac 端完整读取，\n");
        printf("         Mac 端写的数据 Windows 也能完整读回。\n");
        if (byteDiff > 0)
            printf("   注：%lld 行字节有差异属预期（旧记录补写 did 字段），"
                   "不影响互通。\n", byteDiff);
        return 0;
    }
    printf("\n❌ 失败：存在解析错误或字段丢失\n");
    printf("   排查方向：DiffRecord 报出的字段名即丢失的键；\n");
    printf("   若是 ppts[].pressure 恒为 0，参见项目历史上\n");
    printf("   「解析成功但字段错位导致压感恒 0」的缺陷记录。\n");
    return 1;
}

int CmdAnalysis(const std::string& sampleDir) {
    std::vector<std::string> days = ListDays();
    if (days.empty()) {
        fprintf(stderr, "[pbcheck_data] 样本目录里没有 .jsonl: %s\n", sampleDir.c_str());
        return 2;
    }

    // 走与生产完全相同的链路：LoadDay → RecordsToOps → MergeOps
    // → DeriveThresholds → Analyze。任何一环在 macOS 上行为不同，
    // 输出的数字就会变，CI diff 立刻暴露。
    std::vector<RawRecord> all;
    for (const auto& day : days) {
        DayEvents de;
        // parsePoints=false：分析层不关心轨迹，只关心时间戳与分类
        // （Store.h:93-95 说明这样能让「边画边自动重算」保持低开销）
        if (LoadDay(day, de, false)) {
            for (auto& r : de.records) all.push_back(std::move(r));
        }
    }

    if (all.empty()) {
        fprintf(stderr, "[pbcheck_data] 解析出 0 条记录\n");
        return 2;
    }

    std::vector<Op> ops = MergeOps(RecordsToOps(all));
    Thresholds th = DeriveThresholds(ops, Cfg());
    Analysis a = Analyze(std::move(ops), th);

    // 输出确定性 JSON。字段顺序固定，浮点用 %.9g（与 Json.cpp 一致）。
    std::ostringstream o;
    o << "{\n";
    o << "  \"records\": " << I((long long)all.size()) << ",\n";
    o << "  \"ops\": " << I((long long)a.ops.size()) << ",\n";

    o << "  \"thresholds\": {\n";
    o << "    \"n1Sec\": " << F(th.n1Sec) << ",\n";
    o << "    \"n2Sec\": " << F(th.n2Sec) << ",\n";
    o << "    \"depthGapSec\": " << F(th.depthGapSec) << ",\n";
    o << "    \"mergeSec\": " << F(th.mergeSec) << ",\n";
    o << "    \"gapSamples\": " << I(th.gapSamples) << ",\n";
    o << "    \"blockSamples\": " << I(th.blockSamples) << "\n";
    o << "  },\n";

    o << "  \"summary\": {\n";
    o << "    \"empty\": " << (a.empty ? "true" : "false") << ",\n";
    o << "    \"undoTotal\": " << I(a.undoTotal) << ",\n";
    o << "    \"redoTotal\": " << I(a.redoTotal) << ",\n";
    o << "    \"brushTotal\": " << I(a.brushTotal) << ",\n";
    o << "    \"normalTotal\": " << I(a.normalTotal) << ",\n";
    o << "    \"penHoverTotal\": " << I(a.penHoverTotal) << ",\n";
    o << "    \"blockCount\": " << I(a.blockCount) << ",\n";
    o << "    \"deepBlocks\": " << I(a.deepBlocks) << ",\n";
    o << "    \"shallowBlocks\": " << I(a.shallowBlocks) << ",\n";
    o << "    \"normalGapCount\": " << I(a.normalGapCount) << ",\n";
    o << "    \"pauseCount\": " << I(a.pauseCount) << ",\n";
    o << "    \"interruptCount\": " << I(a.interruptCount) << ",\n";
    o << "    \"activePaintUs\": " << I(a.activePaintUs) << ",\n";
    o << "    \"pauseUs\": " << I(a.pauseUs) << ",\n";
    o << "    \"interruptUs\": " << I(a.interruptUs) << ",\n";
    o << "    \"spanStart\": " << I(a.spanStart) << ",\n";
    o << "    \"spanEnd\": " << I(a.spanEnd) << ",\n";
    o << "    \"activities\": " << I((long long)a.activities.size()) << ",\n";
    o << "    \"sequenceLen\": " << I((long long)a.sequence.size()) << "\n";
    o << "  },\n";

    // 一阶转移矩阵：习惯页热力图的数据源。5×5 全量输出，
    // 任一格在 macOS 上算错都会在此暴露。
    o << "  \"trans\": [\n";
    for (int i = 0; i < (int)State::COUNT; ++i) {
        o << "    [";
        for (int j = 0; j < (int)State::COUNT; ++j) {
            o << F(a.trans[i][j]);
            if (j + 1 < (int)State::COUNT) o << ", ";
        }
        o << "]" << (i + 1 < (int)State::COUNT ? "," : "") << "\n";
    }
    o << "  ],\n";

    // 状态占比：热力图行/列的边缘分布
    o << "  \"stateTotal\": [";
    for (int i = 0; i < (int)State::COUNT; ++i) {
        o << I(a.stateTotal[i]);
        if (i + 1 < (int)State::COUNT) o << ", ";
    }
    o << "],\n";

    // 显著长程关联：习惯页的「内在关联」清单。
    // 这是分析层最复杂的输出（提升度 ≥1.5 且样本 ≥20 的筛选），
    // 排序与筛选逻辑任何差异都会改变清单内容。
    o << "  \"notables\": [\n";
    for (size_t k = 0; k < a.notables.size(); ++k) {
        const auto& n = a.notables[k];
        o << "    {\"lag\": " << I(n.lag)
          << ", \"from\": " << I((int)n.from)
          << ", \"to\": " << I((int)n.to)
          << ", \"prob\": " << F(n.prob)
          << ", \"baseline\": " << F(n.baseline)
          << ", \"lift\": " << F(n.lift)
          << ", \"n\": " << I(n.n) << "}"
          << (k + 1 < a.notables.size() ? "," : "") << "\n";
    }
    o << "  ]\n";
    o << "}\n";

    printf("%s", o.str().c_str());

    // 基本有效性断言：分析层若没读到数据，上面的数字会全是 0，
    // 那种情况下即使 diff 通过也没有意义。
    if (a.empty || a.ops.empty() || a.blockCount <= 0) {
        fprintf(stderr, "\n[pbcheck_data] 警告：分析结果为空或无作画块，"
                        "数据可能没被正确读入\n");
        return 1;
    }
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    // 数据目录重定向到隔离位置（不碰用户真实数据目录）。
    // 默认取 SampleDataDir()（读 PB_SAMPLE_DIR，未设置时为 build/sampledata）。
    std::string sampleDir = (argc >= 2) ? std::string(argv[1]) : SampleDataDir();

    std::string cmd = (argc >= 3) ? std::string(argv[2]) : std::string("roundtrip");

    OverrideDataDir(sampleDir);
    // ⚠️ 绝不能再调 EnsureDirs()：它无条件用 SHGetFolderPathW 重算
    //    g_dataDir 指向 ~/Library/Application Support/PaintingBuddy，
    //    会把 OverrideDataDir 设的隔离目录**冲掉**，导致 ListDays() 读空、
    //    测试 return 2。两者是二选一的（Common.h:351-356）。
    //    OverrideDataDir 自身已建好 logs/videos 子目录并做可写探测
    //    （Store.cpp:144-157），无需 EnsureDirs。
    CfgReload();           // 读配置（阈值等），失败则用默认值

    if (cmd == "analysis")  return CmdAnalysis(sampleDir);
    if (cmd == "roundtrip") return CmdRoundtrip(sampleDir);

    fprintf(stderr, "未知子命令: %s（应为 roundtrip 或 analysis）\n", cmd.c_str());
    return 2;
}
