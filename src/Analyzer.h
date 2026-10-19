#pragma once
// ============================================================
//  分析层：把原始事件流转换为「可可视化的时间线」
//
//  核心概念（严格对应需求规格）
//  ─────────────────────────────────────────────────────────
//  操作原子(Op)：撤回 / 重做 / 画笔 / 普通，均带时间戳
//  间隔(Gap)：相邻活动之间的空档，按长度分三类
//    · 普通间隔 (gap <  n1)   —— 连续作画
//    · 暂停绘画 (n1 <= gap < n2) —— 计入序列，不算打断
//    · 打断绘画 (gap >= n2)   —— 显示上限截断为 1 小时
//  n1 / n2：统计用户操作习惯后得出的「最佳分类时间点」
//  深度 / 浅度绘画：块内典型间隔（中位间隔）越小越密集 → 深度
//  ─────────────────────────────────────────────────────────
//  为什么用「中位间隔」而不是「操作数/秒」：
//  一次长鼠标拖拽或一笔长笔画只算 1 个操作，用计数会把
//  「缓慢但连续」的作画误判为浅度。间隔才真实反映密集程度，
//  也与需求原文「普通间隔小就视为深度绘画」一致。
// ============================================================
#include "Common.h"
#include "Config.h"
#include "Store.h"
#include <string>
#include <vector>

namespace pb {

// ------------------------------------------------------------
// 序列状态（5 态）：甘特色块与马尔可夫链共用
//   Normal    普通操作
//   Correct   撤回 / 重做（修正类特殊操作）
//   Brush     画笔
//   NormalGap 普通间隔
//   Pause     暂停绘画
// ------------------------------------------------------------
enum class State : uint8_t {
    Normal    = 0,
    Correct   = 1,
    Brush     = 2,
    NormalGap = 3,
    Pause     = 4,
    COUNT     = 5,
};
const char* StateName(State s);
const char* StateNameCn(State s);

// 长程关联的最大滞后阶数
static const int MAX_LAG = 5;

// 连续事件合并窗口：1 秒。
// 除「画笔」与「撤回/重做」外，相邻间隔小于此窗口的操作合并为一个事件，
// 合并后事件的时间戳 = 组内最早那个操作的时间戳。
// 刻意做成常量而非用户可配项：与录屏的活跃窗口同理，避免设置页参数膨胀。
constexpr int64_t EVENT_MERGE_WINDOW_US = 1000000LL;

// 操作原子（分析层视图）
struct Op {
    int64_t t   = 0;    // 起始（epoch 微秒）；合并事件取组内最早的时间戳
    int64_t end = 0;    // 结束
    OpKind  kind = OpKind::Normal;
    uint8_t src = 0;    // 0=键盘 1=鼠标 2=数位笔
    float   intensity = 0.f;
    int     merged = 1; // 本事件由几个原始操作合并而来（1 = 未合并）
    // 设备无关的「绘画动作」计数：数位笔落笔与鼠标左键笔画都算 1。
    //
    // 为什么不能用 kind==Brush 或 src==2 来数绘画动作：RecordsToOps 里
    // 鼠标笔画('M')的 kind 是 Normal、src 是 1，数位笔在有键按住时 kind
    // 也会变成 Normal。实测一份真实日志中有个块 273 个操作里含 136 次
    // 真实鼠标笔画、0 次数位笔，若按 kind==Brush 统计会得到「零画笔」，
    // 从而把纯鼠标作画的用户误判成没在画画。
    //
    // 用计数器而非 bool：MergeOps 合并时可直接累加，不丢组内笔画数。
    int     paintCount = 0;
    std::string note;
};

// 合并后的活动区间（若干重叠 Op 的并集）
struct Activity {
    int64_t start = 0, end = 0;
    State   dominant = State::Normal;
    int     opCount = 0;
    int     undoCount = 0, brushCount = 0, normalCount = 0;
    float   sumIntensity = 0.f;
    // 注意：这里刻意不保存指向 Analysis::ops 元素的指针。
    // Analysis 会被整体拷贝进图表（GanttView::data_ = a），拷贝后原 ops
    // 向量地址不再有效；而「边画边自动重算」会频繁替换 Analysis 对象，
    // 任何跨对象持有的 Op* 都会变成悬空指针。该字段此前只写不读，故直接移除。
};

enum class GapType : uint8_t { Normal = 0, Pause = 1, Interrupt = 2 };
struct Gap {
    int64_t start = 0, end = 0;
    GapType type = GapType::Normal;
};

// 作画块：被「打断绘画」切开的连续作画区间（内部允许普通间隔与暂停）
struct PaintBlock {
    int64_t start = 0, end = 0;
    bool    deep = false;
    double  medianGapSec = 0.0;   // 块内典型间隔 → 深/浅判定依据
    double  opsPerSec = 0.0;      // 展示用
    int     activityCount = 0;
    int     opCount = 0;
    int     undoCount = 0, brushCount = 0, normalCount = 0;
    int     paintCount = 0;       // 设备无关的绘画动作数（数位笔落笔 + 鼠标笔画）
    // 撤回 / 绘画动作。当前仅作统计输出，界面未展示，深浅分类也未使用它 ——
    // 曾尝试用它做降级规则，但实测无实证依据（见 Analyzer.cpp 内说明）。
    // 保留此值是为了后续积累更多数据后能校准阈值，而不必重新采集。
    double  undoPerPaint = 0.0;
    int64_t pauseUs = 0;
    size_t  actBegin = 0, actEnd = 0;  // 在 Analysis::activities 中的区间
    size_t  gapBegin = 0, gapEnd = 0;  // 在 Analysis::gaps 中的区间
};

// 阈值集合
struct Thresholds {
    double n1Sec = 45.0;         // 暂停绘画下限（>= 此值的空档才算「暂停」）
    double n2Sec = 300.0;        // 打断下限（默认 5 分钟）
    double depthGapSec = 2.0;    // 块内中位间隔 <= 此值 → 深度绘画
    double mergeSec = 3.0;       // 活动合并窗口（与 n1 解耦，见下方说明）
    bool   n1Auto = true, n2Auto = true, depthAuto = true, mergeAuto = true;
    int    gapSamples = 0;       // 参与统计的「停顿候选」间隔数
    int    blockSamples = 0;     // 参与深度分界统计的块数
    std::string method;          // 推导方法说明（界面展示）
};
//
// 为什么 mergeSec 要和 n1 分开：
//   n1 此前同时承担两个职责 —— ①「多久算一次暂停」②活动合并窗口
//   （Analyze 里 `o.t <= last.end + n1Us` 决定相邻操作是否并成一个活动）。
//   这两个职责需要的量级完全不同：活动合并要贴合运笔节奏（2~3 秒），
//   而「暂停」是人类有意识地停下手（几十秒）。
//   共用一个值的后果（三份真实日志实测）：要把暂停次数压到个位数，
//   n1 得抬到 25~45s，活动数会从 69 塌到 9，甘特图糊成一片；
//   反过来 n1 保持 2.5s 时暂停高达 67 次，绝大多数只是落笔间的凝视。
//   拆开后两者互不干扰。
//
// 分带结构：
//   < 1s        同一连续动作内（MergeOps 已合并，不参与停顿统计）
//   1s ~ n1     正常作画（含看画、比对参考等短暂停顿）
//   n1 ~ n2     暂停绘画
//   > n2        打断绘画（另起作画块）
//
// mergeSec 只影响「活动」这一展示/统计单位，不参与上述分带。

// ------------------------------------------------------------
// 甘特时间轴：把「打断」折叠为最多 1 小时后的显示坐标
// ------------------------------------------------------------
struct GanttCell {
    int64_t  dispStart = 0, dispEnd = 0;   // 显示时间（已折叠）
    int64_t  realStart = 0, realEnd = 0;   // 真实时间
    uint8_t  kind = 0;                     // GanttKind
    State    state = State::Normal;        // 操作类单元的状态色
    bool     truncated = false;            // 打断段被截断
    float    intensity = 0.f;
};
enum GanttKind : uint8_t {
    GK_BAND_DEEP = 0,      // 深度绘画底色带
    GK_BAND_SHALLOW,       // 浅度绘画底色带
    GK_OP,                 // 操作小段（按 state 取色）
    GK_GAP_NORMAL,         // 普通间隔
    GK_GAP_PAUSE,          // 暂停绘画
    GK_INTERRUPT,          // 打断绘画（截断显示）
};
struct GanttBreak {
    int64_t dispPos = 0;       // 折叠标记在显示轴上的位置
    int64_t realStart = 0, realEnd = 0;
};
// 显示时间 → 真实时间 的分段线性映射
struct DispSegment {
    int64_t dispStart = 0, dispEnd = 0;
    int64_t realStart = 0;      // real = realStart + (disp - dispStart)
};
struct GanttTimeline {
    std::vector<GanttCell>   cells;
    std::vector<GanttBreak>  breaks;
    std::vector<DispSegment> segments;
    int64_t dispStart = 0, dispEnd = 0;
    int64_t realStart = 0, realEnd = 0;
    int64_t RealToDisp(int64_t realUs) const;
    int64_t DispToReal(int64_t dispUs) const;
};

// ------------------------------------------------------------
// 分析结果
// ------------------------------------------------------------
struct Analysis {
    std::vector<Op>         ops;         // 全部操作（时间序）
    std::vector<Activity>   activities;  // 合并后的活动区间
    std::vector<Gap>        gaps;        // 全部间隔（含打断）
    std::vector<PaintBlock> blocks;      // 作画块
    Thresholds              th;

    // 马尔可夫序列（仅块内，跨打断不计转移）
    std::vector<State>  sequence;
    std::vector<size_t> seqBlockEnd;     // 每个块在 sequence 中的结束下标（含）

    // 一阶转移
    double      trans[(size_t)State::COUNT][(size_t)State::COUNT] = {};
    long long   transCount[(size_t)State::COUNT][(size_t)State::COUNT] = {};
    // 长程关联：滞后 k 阶
    long long   lagCount[MAX_LAG][(size_t)State::COUNT][(size_t)State::COUNT] = {};
    double      lagProb[MAX_LAG][(size_t)State::COUNT][(size_t)State::COUNT] = {};
    // 二阶条件转移：P(当前 | 前前, 前)
    long long   lag2Count[(size_t)State::COUNT][(size_t)State::COUNT][(size_t)State::COUNT] = {};
    double      lag2Prob[(size_t)State::COUNT][(size_t)State::COUNT][(size_t)State::COUNT] = {};
    long long   stateTotal[(size_t)State::COUNT] = {};

    // 汇总
    int     undoTotal = 0, redoTotal = 0, brushTotal = 0, normalTotal = 0;
    int     penHoverTotal = 0;
    int     blockCount = 0, deepBlocks = 0, shallowBlocks = 0;
    int     normalGapCount = 0, pauseCount = 0, interruptCount = 0;
    int64_t activePaintUs = 0;
    int64_t pauseUs = 0;
    int64_t interruptUs = 0;     // 真实时长（未截断）
    int64_t spanStart = 0, spanEnd = 0;
    bool    empty = true;

    // 最长的长程关联（用于界面直接给出结论）
    struct NotableLag {
        bool  valid = false;
        int   lag = 0;
        State from = State::Normal, to = State::Normal;
        double prob = 0, baseline = 0, lift = 0;
        long long n = 0;
    };
    std::vector<NotableLag> notables;
};

// —— 接口 ——
std::vector<Op> RecordsToOps(const std::vector<RawRecord>& recs);
// 连续事件合并：除「画笔」与「撤回/重做」外，相邻间隔 < 合并窗口的操作
// 合并为一个事件，时间戳取组内最早者，merged 记录被合并的原始操作数。
// 画笔与修正类操作各自独立，不参与合并也不吸收相邻事件。
std::vector<Op> MergeOps(std::vector<Op> ops,
                         int64_t windowUs = EVENT_MERGE_WINDOW_US);
Thresholds      DeriveThresholds(const std::vector<Op>& ops, const Config& cfg);
Analysis        Analyze(std::vector<Op> ops, const Thresholds& th);
GanttTimeline   BuildGantt(const Analysis& a, bool collapseInterrupts = true);

bool AnalyzeDay(const std::string& day, Analysis& out, const Config& cfg);
bool AnalyzeRange(int64_t fromUs, int64_t toUs, Analysis& out, const Config& cfg);
// 加载全部可用数据（供习惯统计）
std::vector<Op> LoadAllOps(size_t maxDays = 0);

constexpr int64_t INTERRUPT_DISPLAY_CAP_US = 3600LL * 1000000LL; // 打断显示上限：1 小时

} // namespace pb
