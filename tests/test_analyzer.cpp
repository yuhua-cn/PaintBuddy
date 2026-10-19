// ============================================================
//  分析层 headless 测试：喂入合成操作序列，严格验证核心算法
//  不链接 GUI / 采集 / 录屏，只测纯计算，可自动化运行
// ============================================================
#include "Analyzer.h"
#include "Config.h"
#include "Store.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>

using namespace pb;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; printf("  [PASS] %s\n", msg); } \
    else      { ++g_fail; printf("  [FAIL] %s   (line %d)\n", msg, __LINE__); } \
} while (0)

static const int64_t BASE = 1600000000000000LL;   // epoch 微秒基数
static const int64_t US = 1000000LL;

// 追加一个操作，返回推进后的时间游标
static void AddOp(std::vector<Op>& ops, int64_t& cur, int64_t gapUs, OpKind kind) {
    Op o;
    o.t = cur;
    o.end = cur + 100000;   // 持续 0.1s
    o.kind = kind;
    o.src = (kind == OpKind::Brush) ? 2 : 0;
    o.intensity = 1.f;
    ops.push_back(o);
    cur = o.end + gapUs;    // 下一个 op 的起点 = 本 op 结束 + 间隔
}

int main() {
    printf("==== 绘画伴侣 分析层测试 ====\n");

    Config cfg;
    cfg.LoadDefaults();
    // 关闭用户固定阈值，全部走自动统计
    cfg.n1Sec = 0; cfg.n2Sec = 0; cfg.depthGapSec = 0;

    // --------------------------------------------------------
    // 合成场景：
    //   段A 30 个密集操作(间隔0.5s) —— 深度绘画
    //   15s 看画/比对参考 —— 新口径下算「正常作画」，不再是暂停
    //   段B 20 个密集操作(间隔0.5s)
    //   200s 长停顿 —— 暂停绘画（暂停下限 n1 >= 30s）
    //   3 小时离开（打断 → 折叠到 1 小时）
    //   段C 25 个稀疏操作(间隔8s) —— 浅度绘画
    // 每 7 个插入一次撤回，每 5 个插入一次画笔
    // --------------------------------------------------------
    std::vector<Op> ops;
    int64_t cur = BASE;
    int idx = 0;
    auto pickKind = [&](int64_t gapUs) -> OpKind {
        (void)gapUs;
        if (idx % 7 == 3) return OpKind::Undo;
        if (idx % 5 == 0) return OpKind::Brush;
        return OpKind::Normal;
    };

    // 段A
    for (int i = 0; i < 30; ++i) { AddOp(ops, cur, (int64_t)(0.5 * US), pickKind(0)); ++idx; }
    // A→B 15s 看画停顿（新口径：短于暂停下限 → 正常作画）
    AddOp(ops, cur, (int64_t)(15.0 * US), pickKind(0)); ++idx;
    // 段B
    for (int i = 0; i < 20; ++i) { AddOp(ops, cur, (int64_t)(0.5 * US), pickKind(0)); ++idx; }
    // 200s 长停顿 → 暂停绘画（不切块，因为 < n2）
    AddOp(ops, cur, (int64_t)(200.0 * US), pickKind(0)); ++idx;
    // B→C 打断：先加一个 op，其 gap = 3 小时
    AddOp(ops, cur, (int64_t)(10800.0 * US), pickKind(0)); ++idx;
    // 段C（稀疏 8s）
    for (int i = 0; i < 25; ++i) { AddOp(ops, cur, (int64_t)(8.0 * US), pickKind(0)); ++idx; }

    printf("\n[1] 阈值自动推导\n");
    Thresholds th = DeriveThresholds(ops, cfg);
    printf("    n1=%.3fs  n2=%.2fs  mergeSec=%.3fs  depthGap=%.3fs  gapSamples=%d\n",
           th.n1Sec, th.n2Sec, th.mergeSec, th.depthGapSec, th.gapSamples);
    printf("    method=%s\n", th.method.c_str());
    // mergeSec（活动合并窗口）由全池的谷检测得出：0.5s（密集运笔）与 8.0s
    // （稀疏）之间 16 倍跳变，几何均值 sqrt(0.5*8) = 2.0s。
    // 谷检测跑在全池上，故 <1s 的密集间隔仍参与「找谷」，只是不参与分位数兜底。
    CHECK(th.mergeSec > 1.9 && th.mergeSec < 2.1,
          "mergeSec 由真实谷检出（0.5s↔8s 的 16 倍跳变 → 2.0s）");
    CHECK(th.mergeSec >= 1.0 && th.mergeSec <= 15.0, "mergeSec 在 [1,15]s 语义区间内");
    // n1（暂停下限）取停顿候选池 97 百分位再夹到 [30,90]。
    // 本场景 p97 = 2532s（被 200s 与 10800s 两个样本拉高）→ 夹到上限 90s。
    CHECK(th.n1Sec >= 30.0 && th.n1Sec <= 90.0, "n1（暂停下限）在 [30,90]s 语义区间内");
    CHECK(th.n1Sec > th.mergeSec * 5.0,
          "n1 与 mergeSec 量级已解耦（暂停阈值远大于活动合并窗口）");
    // n2：>=300s 的区间只有 1 个样本（10800s），守卫要求 >=20 个 → 拒绝跳变，
    // 退回过滤池 95 百分位（144.5s），再抬到语义地板 300s。
    CHECK(th.n2Sec >= 300.0, "n2 不低于 5 分钟语义地板");
    CHECK(th.n2Sec < 10800.0, "n2 能区分暂停与 3 小时打断");
    CHECK(th.n2Sec >= th.n1Sec * 3.0, "n2 与 n1 保持至少 3 倍区分度");
    // gapSamples 语义：只统计 >= 合并窗口(1s) 的「停顿候选」，不含运笔节奏间隔。
    // 本场景 77 个间隔中 27 个 >= 1s（8s×24 + 15s×1 + 200s×1 + 10800s×1）。
    CHECK(th.gapSamples == 27, "停顿候选样本量 = 27（已剔除 50 个 <1s 的密集间隔）");

    printf("\n[2] 主分析：块切分 / 深浅 / 汇总\n");
    Analysis a = Analyze(ops, th);
    CHECK(!a.empty, "分析结果非空");
    printf("    blocks=%d  interrupt=%d  pause=%d  normalGap=%d\n",
           a.blockCount, a.interruptCount, a.pauseCount, a.normalGapCount);
    CHECK(a.blockCount == 2, "被 3 小时打断切成 2 个作画块");
    CHECK(a.interruptCount == 1, "打断次数 = 1");
    // 新口径：15s 看画/比对参考属「正常作画」，只有 200s 长停顿算暂停。
    // 旧口径下 15s 会被计为暂停，导致「暂停次数」虚高（真实日志实测虚高 10~21 倍）。
    CHECK(a.pauseCount == 1, "暂停次数 = 1（200s 长停顿；15s 看画不算暂停）");
    CHECK(a.normalGapCount >= 20, "存在普通间隔（密集段内）");
    bool foundPause = false;
    for (const auto& g : a.gaps)
        if (g.type == GapType::Pause && (g.end - g.start) >= 190LL * US) foundPause = true;
    CHECK(foundPause, "暂停间隔时长 ≈ 200s");

    if (a.blocks.size() == 2) {
        printf("    block0: medianGap=%.2fs deep=%d ops=%d | block1: medianGap=%.2fs deep=%d ops=%d\n",
               a.blocks[0].medianGapSec, (int)a.blocks[0].deep, a.blocks[0].opCount,
               a.blocks[1].medianGapSec, (int)a.blocks[1].deep, a.blocks[1].opCount);
        CHECK(a.blocks[0].deep == true,  "密集段判定为深度绘画");
        CHECK(a.blocks[1].deep == false, "稀疏段判定为浅度绘画");
        CHECK(a.deepBlocks == 1 && a.shallowBlocks == 1, "深浅块各一个");
    }

    // 打断真实时长 ≈ 3 小时
    bool foundLongInterrupt = false;
    for (const auto& g : a.gaps)
        if (g.type == GapType::Interrupt && (g.end - g.start) >= 10000LL * US) foundLongInterrupt = true;
    CHECK(foundLongInterrupt, "打断真实时长 ≈ 3 小时");

    printf("\n[3] 操作分类计数\n");
    printf("    undo=%d  brush=%d  normal=%d  total=%zu\n",
           a.undoTotal, a.brushTotal, a.normalTotal, a.ops.size());
    CHECK(a.undoTotal > 0, "统计到撤回操作");
    CHECK(a.brushTotal > 0, "统计到画笔操作");
    CHECK(a.normalTotal > 0, "统计到普通操作");
    CHECK((size_t)(a.undoTotal + a.brushTotal + a.normalTotal + a.redoTotal) == a.ops.size(),
          "四类操作计数之和 == 总操作数");

    printf("\n[4] 马尔可夫序列一致性\n");
    long long stateSum = 0;
    for (int i = 0; i < (int)State::COUNT; ++i) stateSum += a.stateTotal[i];
    printf("    sequence=%zu  stateSum=%lld\n", a.sequence.size(), stateSum);
    CHECK(stateSum == (long long)a.sequence.size(), "状态频次之和 == 序列长度");

    // 每个有出边的状态，转移概率行和 ≈ 1
    bool rowOk = true;
    int rowsChecked = 0;
    for (int i = 0; i < (int)State::COUNT; ++i) {
        double s = 0;
        for (int j = 0; j < (int)State::COUNT; ++j) s += a.trans[i][j];
        if (s > 1e-9) { ++rowsChecked; if (fabs(s - 1.0) > 1e-6) rowOk = false; }
    }
    CHECK(rowOk && rowsChecked >= 3, "一阶转移矩阵每行归一化（和≈1）");

    // 长程关联矩阵同样归一化
    bool lagOk = true;
    for (int k = 0; k < MAX_LAG; ++k)
        for (int i = 0; i < (int)State::COUNT; ++i) {
            double s = 0;
            for (int j = 0; j < (int)State::COUNT; ++j) s += a.lagProb[k][i][j];
            if (s > 1e-9 && fabs(s - 1.0) > 1e-6) lagOk = false;
        }
    CHECK(lagOk, "长程关联矩阵每行归一化");

    // 二阶条件转移每 (i,j) 行归一化
    bool lag2Ok = true;
    for (int i = 0; i < (int)State::COUNT; ++i)
        for (int j = 0; j < (int)State::COUNT; ++j) {
            double s = 0;
            for (int k = 0; k < (int)State::COUNT; ++k) s += a.lag2Prob[i][j][k];
            if (s > 1e-9 && fabs(s - 1.0) > 1e-6) lag2Ok = false;
        }
    CHECK(lag2Ok, "二阶条件转移每个前缀组合归一化");

    printf("\n[5] 甘特时间轴：打断折叠到 1 小时\n");
    GanttTimeline tl = BuildGantt(a, true);
    CHECK(!tl.cells.empty(), "甘特单元非空");
    int interruptCells = 0, truncatedCells = 0;
    int64_t maxInterruptDisp = 0;
    for (const auto& c : tl.cells) {
        if (c.kind == GK_INTERRUPT) {
            ++interruptCells;
            int64_t dlen = c.dispEnd - c.dispStart;
            if (dlen > maxInterruptDisp) maxInterruptDisp = dlen;
            if (c.truncated) ++truncatedCells;
        }
    }
    printf("    interruptCells=%d  truncated=%d  maxInterruptDisplay=%.2f min\n",
           interruptCells, truncatedCells, (double)maxInterruptDisp / 60e6);
    CHECK(interruptCells == 1, "甘特含 1 个打断单元");
    CHECK(maxInterruptDisp == INTERRUPT_DISPLAY_CAP_US, "打断显示时长恰为 1 小时上限");
    CHECK(truncatedCells == 1, "打断单元标记为已截断");

    // 显示总跨度应远小于真实跨度（因打断被折叠）
    int64_t realSpan = a.spanEnd - a.spanStart;
    int64_t dispSpan = tl.dispEnd - tl.dispStart;
    printf("    realSpan=%.1f h  dispSpan=%.1f h\n",
           (double)realSpan / 3600e6, (double)dispSpan / 3600e6);
    CHECK(dispSpan < realSpan, "显示跨度 < 真实跨度（打断被压缩）");
    CHECK(realSpan - dispSpan >= (10800LL - 3600LL) * US - 2 * US,
          "压缩量 ≈ 被打断折叠掉的 2 小时");

    // 时间轴映射往返一致性
    int64_t mid = (tl.dispStart + tl.dispEnd) / 2;
    int64_t rt = tl.DispToReal(mid);
    int64_t back = tl.RealToDisp(rt);
    CHECK(llabs(back - mid) < 2 * US, "显示↔真实时间映射往返一致");

    // --------------------------------------------------------
    // [6] 键盘归属规则（回归）：落笔瞬间有键按下 → 整次记为键盘操作(src=0)
    //     此前鼠标笔画的 kbAttr 未落盘、且 RecordsToOps 的 'M' 分支硬编码
    //     src=1 忽略该标记，导致「按住键时的鼠标/笔操作记为键盘」这条规则
    //     对鼠标笔画与鼠标移动完全不生效。这里用序列化→反序列化→转 Op
    //     的全链路断言，锁定规则不再退化。
    // --------------------------------------------------------
    printf("\n[6] 键盘归属规则（鼠标笔画 / 鼠标移动 / 数位笔）\n");
    {
        auto roundTrip = [](const RawRecord& in) -> Op {
            // 走一遍落盘用的序列化 + 加载用的反序列化（ParseEventLine），
            // 再转成分析用的 Op，验证 kbAttr 在全链路中不丢。
            std::string line = SerializeEvent(in);
            RawRecord out;
            if (!ParseEventLine(line, out)) return Op();
            std::vector<RawRecord> recs{ out };
            std::vector<Op> ops = RecordsToOps(recs);
            return ops.empty() ? Op() : ops.front();
        };

        // 鼠标笔画：按住键 → src=0（键盘）
        RawRecord mKb;
        mKb.tUs = BASE; mKb.endUs = BASE + 500000; mKb.type = 'M';
        mKb.button = 0; mKb.kbAttr = true; mKb.pathLen = 100.f;
        Op o1 = roundTrip(mKb);
        printf("    鼠标笔画(kb=1) -> src=%d kind=%d note=%s\n", o1.src, (int)o1.kind, o1.note.c_str());
        CHECK(o1.src == 0, "按住键的鼠标笔画归为键盘操作(src=0)");
        CHECK(o1.kind == OpKind::Normal, "按住键的鼠标笔画分类为普通操作");

        // 鼠标笔画：无键 → src=1（鼠标）
        RawRecord mNo;
        mNo.tUs = BASE; mNo.endUs = BASE + 500000; mNo.type = 'M';
        mNo.button = 0; mNo.kbAttr = false; mNo.pathLen = 100.f;
        Op o2 = roundTrip(mNo);
        CHECK(o2.src == 1, "未按键的鼠标笔画归为鼠标(src=1)");

        // 鼠标移动段：按住键 → src=0
        RawRecord mvKb;
        mvKb.tUs = BASE; mvKb.endUs = BASE + 300000; mvKb.type = 'm';
        mvKb.button = 0; mvKb.kbAttr = true; mvKb.pathLen = 50.f;
        Op o3 = roundTrip(mvKb);
        CHECK(o3.src == 0, "按住键的鼠标移动归为键盘操作(src=0)");

        // 数位笔：按住键 → 归键盘(Normal)，不计画笔
        RawRecord pKb;
        pKb.tUs = BASE; pKb.endUs = BASE + 800000; pKb.type = 'p';
        pKb.kbAttr = true; pKb.hover = false; pKb.pathLen = 200.f;
        Op o4 = roundTrip(pKb);
        CHECK(o4.src == 0 && o4.kind == OpKind::Normal, "按住键的数位笔笔画归为键盘操作");

        // 数位笔：无键 → 画笔(Brush)
        RawRecord pNo;
        pNo.tUs = BASE; pNo.endUs = BASE + 800000; pNo.type = 'p';
        pNo.kbAttr = false; pNo.hover = false; pNo.pathLen = 200.f;
        Op o5 = roundTrip(pNo);
        CHECK(o5.src == 2 && o5.kind == OpKind::Brush, "未按键的数位笔笔画归为画笔");

        // kbAttr 必须真正落盘（序列化后再解析仍为 true）
        RawRecord chk;
        chk.tUs = BASE; chk.endUs = BASE + 100000; chk.type = 'M';
        chk.button = 0; chk.kbAttr = true; chk.pathLen = 10.f;
        std::string ser = SerializeEvent(chk);
        CHECK(ser.find("\"kb\"") != std::string::npos,
              "鼠标笔画的键盘归属标记已写入序列化结果（此前漏写）");
    }

    // --------------------------------------------------------
    // [7] 连续事件合并（1 秒窗口）：
    //     除画笔与撤回/重做外，相邻间隔 < 1s 的操作合并为一个事件，
    //     时间戳 = 组内最早那个操作的时间戳。
    // --------------------------------------------------------
    printf("\n[7] 连续事件合并（窗口 %lld ms）\n", (long long)(EVENT_MERGE_WINDOW_US / 1000));
    {
        auto mk = [](int64_t t, int64_t dur, OpKind k, uint8_t src) {
            Op o; o.t = t; o.end = t + dur; o.kind = k; o.src = src;
            o.intensity = 1.f; o.merged = 1;
            return o;
        };

        // 场景 A：三个普通操作，间隔各 0.3s（< 1s）→ 应合并成 1 个
        std::vector<Op> a;
        a.push_back(mk(BASE,                100000, OpKind::Normal, 0));
        a.push_back(mk(BASE +  400000,      100000, OpKind::Normal, 1));
        a.push_back(mk(BASE +  800000,      100000, OpKind::Normal, 0));
        std::vector<Op> ra = MergeOps(a);
        printf("    A: 3 个连续普通操作 -> %zu 个事件\n", ra.size());
        CHECK(ra.size() == 1, "间隔小于 1 秒的连续普通操作合并为一个事件");
        if (ra.size() == 1) {
            CHECK(ra[0].t == BASE, "合并后时间戳 = 组内最早操作的时间戳");
            CHECK(ra[0].merged == 3, "合并计数 = 3（原始操作数守恒）");
            CHECK(ra[0].end == BASE + 900000, "合并后结束时刻 = 组内最晚结束时刻");
        }

        // 场景 B：间隔超过 1 秒 → 不合并
        std::vector<Op> b;
        b.push_back(mk(BASE,            100000, OpKind::Normal, 0));
        b.push_back(mk(BASE + 1500000,  100000, OpKind::Normal, 0));   // 间隔 1.4s
        std::vector<Op> rb = MergeOps(b);
        CHECK(rb.size() == 2, "间隔达到 1 秒以上的操作不合并");

        // 场景 C：画笔独立成组，不被相邻普通操作吸收，也不吸收别人
        std::vector<Op> c;
        c.push_back(mk(BASE,           100000, OpKind::Normal, 0));
        c.push_back(mk(BASE +  200000, 300000, OpKind::Brush,  2));   // 画笔
        c.push_back(mk(BASE +  600000, 100000, OpKind::Normal, 0));
        std::vector<Op> rc = MergeOps(c);
        printf("    C: 普通-画笔-普通 -> %zu 个事件\n", rc.size());
        CHECK(rc.size() == 3, "画笔独立成组，前后普通操作不被合并进去");
        int brushCnt = 0;
        for (const auto& o : rc) if (o.kind == OpKind::Brush) ++brushCnt;
        CHECK(brushCnt == 1, "画笔事件数量不因合并而丢失");

        // 场景 D：撤回独立成组
        std::vector<Op> d;
        d.push_back(mk(BASE,           100000, OpKind::Normal, 0));
        d.push_back(mk(BASE +  200000,  50000, OpKind::Undo,   0));   // 撤回
        d.push_back(mk(BASE +  300000, 100000, OpKind::Normal, 0));
        std::vector<Op> rd = MergeOps(d);
        CHECK(rd.size() == 3, "撤回独立成组，不与相邻普通操作合并");
        int undoCnt = 0;
        for (const auto& o : rd) if (o.kind == OpKind::Undo) ++undoCnt;
        CHECK(undoCnt == 1, "撤回事件数量不因合并而丢失");

        // 场景 E：重做同样独立（与撤回同属修正类）
        std::vector<Op> e;
        e.push_back(mk(BASE,          100000, OpKind::Normal, 0));
        e.push_back(mk(BASE + 200000,  50000, OpKind::Redo,   0));
        std::vector<Op> re = MergeOps(e);
        CHECK(re.size() == 2, "重做独立成组");

        // 场景 F：原始操作总数守恒（merged 之和 == 输入个数）
        std::vector<Op> f;
        f.push_back(mk(BASE,                100000, OpKind::Normal, 0));
        f.push_back(mk(BASE +  300000,      100000, OpKind::Brush,  2));
        f.push_back(mk(BASE +  500000,      100000, OpKind::Normal, 0));
        f.push_back(mk(BASE +  700000,      100000, OpKind::Normal, 0));
        f.push_back(mk(BASE + 3000000,      100000, OpKind::Undo,   0));
        std::vector<Op> rf = MergeOps(f);
        long long sumMerged = 0;
        for (const auto& o : rf) sumMerged += o.merged;
        printf("    F: %zu 个原始操作 -> %zu 个事件，merged 合计 %lld\n",
               f.size(), rf.size(), sumMerged);
        CHECK(sumMerged == (long long)f.size(), "合并计数之和 == 原始操作数（无丢失无重复）");
    }

    // --------------------------------------------------------
    // [8] 滚轮合并与归类：
    //     一次物理滚动在系统层是几十条 WM_MOUSEWHEEL，采集层按 keyMergeMs
    //     合并成一条记录（时间戳取第一次，count=合并条数）。
    //     这里锁定合并结果在「序列化 → 反序列化 → 转 Op」全链路的行为：
    //     count 不丢、滚轮归类为键盘操作(src=0)、鼠标移动不受影响。
    // --------------------------------------------------------
    printf("\n[8] 滚轮合并与归类（视为键盘操作）\n");
    {
        auto wheelRT = [](const RawRecord& in) -> Op {
            std::string line = SerializeEvent(in);
            RawRecord out;
            if (!ParseEventLine(line, out)) return Op();
            std::vector<RawRecord> recs{ out };
            std::vector<Op> ops = RecordsToOps(recs);
            return ops.empty() ? Op() : ops.front();
        };

        // 滚轮：合并了 12 条消息
        RawRecord w;
        w.tUs = BASE; w.endUs = BASE + 260000;   // 组内首次到最后一次
        w.type = 'm'; w.button = 3; w.kind = OpKind::Normal; w.count = 12;
        Op ow = wheelRT(w);
        printf("    滚轮(count=12) -> src=%d intensity=%.0f note=%s\n",
               ow.src, ow.intensity, ow.note.c_str());
        CHECK(ow.src == 0, "滚轮归类为键盘操作(src=0)");
        CHECK(ow.kind == OpKind::Normal, "滚轮为普通操作（不是撤回/画笔）");
        CHECK((int)ow.intensity == 12, "滚轮强度 = 合并次数（count 全链路不丢）");
        CHECK(ow.note.find("滚轮") != std::string::npos, "滚轮备注标明「滚轮」");
        CHECK(ow.note.find("x12") != std::string::npos, "滚轮备注带合并次数 x12（与键盘同记法）");
        CHECK(ow.t == BASE, "滚轮时间戳 = 组内第一次（不是最后一次）");

        // 单条滚轮（未合并）：count 缺省为 1，备注不带 x 后缀
        RawRecord w1;
        w1.tUs = BASE; w1.endUs = BASE; w1.type = 'm'; w1.button = 3; w1.count = 1;
        Op ow1 = wheelRT(w1);
        CHECK(ow1.src == 0 && (int)ow1.intensity == 1, "单条滚轮也归为键盘操作且强度为 1");
        CHECK(ow1.note.find("x") == std::string::npos, "单条滚轮备注不带 x 后缀");

        // 对照组：鼠标自由移动段不能被误判成滚轮（同为 'm'，靠 button 区分）
        RawRecord mv;
        mv.tUs = BASE; mv.endUs = BASE + 300000;
        mv.type = 'm'; mv.button = 0; mv.kbAttr = false; mv.pathLen = 50.f;
        Op om = wheelRT(mv);
        CHECK(om.src == 1 && om.note == "鼠标移动",
              "鼠标移动段仍归为鼠标（未被滚轮分支误吞）");

        // 对照组：按住键的移动段归键盘
        RawRecord mvk;
        mvk.tUs = BASE; mvk.endUs = BASE + 300000;
        mvk.type = 'm'; mvk.button = 0; mvk.kbAttr = true; mvk.pathLen = 50.f;
        Op omk = wheelRT(mvk);
        CHECK(omk.src == 0, "按住键的鼠标移动仍归为键盘操作");

        // 右键/中键点击：保持鼠标归类，不是滚轮
        RawRecord rc;
        rc.tUs = BASE; rc.endUs = BASE; rc.type = 'm'; rc.button = 1; rc.count = 1;
        Op orc = wheelRT(rc);
        CHECK(orc.src == 1, "右键点击仍归为鼠标（只有滚轮算键盘）");

        // 合并后的滚轮参与连续事件合并时不应丢失（滚轮是 Normal，可被吸收）
        std::vector<Op> seq;
        seq.push_back(ow);                                   // 滚轮 @BASE
        Op nk; nk.t = BASE + 300000; nk.end = BASE + 400000; // 300ms 后的普通操作
        nk.kind = OpKind::Normal; nk.src = 0; nk.intensity = 1; nk.merged = 1;
        seq.push_back(nk);
        std::vector<Op> mg = MergeOps(seq);
        long long kept = 0;
        for (const auto& o : mg) kept += o.merged;
        printf("    滚轮+普通(间隔300ms) 合并后 -> %zu 个事件，merged 合计 %lld\n",
               mg.size(), kept);
        CHECK(mg.size() == 1, "间隔小于 1 秒的滚轮与后续普通操作被合并为一个事件");
        CHECK(kept == 2, "合并不丢失原始操作数");
    }

    // --------------------------------------------------------
    // [9] 设备无关的绘画动作计数（paintCount）：
    //     鼠标笔画('M')的 kind 是 Normal、src 是 1，按 kind==Brush 统计
    //     会把纯鼠标作画的用户算成「零画笔」。实测一份真实日志中，
    //     一个含 285 次真实鼠标笔画的块，按 brushCount 只数出 0。
    //     paintCount 把数位笔落笔与鼠标左键笔画统一计入，且合并时守恒。
    // --------------------------------------------------------
    printf("\n[9] 设备无关的绘画动作计数（paintCount）\n");
    {
        auto rt = [](const RawRecord& in) -> Op {
            std::string line = SerializeEvent(in);
            RawRecord out;
            if (!ParseEventLine(line, out)) return Op();
            std::vector<RawRecord> recs{ out };
            std::vector<Op> ops = RecordsToOps(recs);
            return ops.empty() ? Op() : ops.front();
        };

        // 鼠标左键笔画 → 算绘画动作（但 kind 是 Normal，brushCount 数不到）
        RawRecord mStroke;
        mStroke.tUs = BASE; mStroke.endUs = BASE + 500000; mStroke.type = 'M';
        mStroke.button = 0; mStroke.kbAttr = false; mStroke.pathLen = 100.f;
        Op ms = rt(mStroke);
        printf("    鼠标左键笔画 -> kind=%d(Normal=%d) paintCount=%d\n",
               (int)ms.kind, (int)OpKind::Normal, ms.paintCount);
        CHECK(ms.paintCount == 1, "鼠标左键笔画计入绘画动作（paintCount=1）");
        CHECK(ms.kind == OpKind::Normal,
              "鼠标笔画的 kind 仍是 Normal（故 brushCount 会漏计它）");

        // 数位笔落笔 → 算绘画动作
        RawRecord pStroke;
        pStroke.tUs = BASE; pStroke.endUs = BASE + 800000; pStroke.type = 'p';
        pStroke.kbAttr = false; pStroke.hover = false; pStroke.pathLen = 200.f;
        Op ps = rt(pStroke);
        CHECK(ps.paintCount == 1, "数位笔落笔计入绘画动作（paintCount=1）");
        CHECK(ps.kind == OpKind::Brush, "无键按住的数位笔落笔仍是画笔");

        // 数位笔有键按住 → kind 变 Normal，但确实画了东西，仍计数
        RawRecord pKb;
        pKb.tUs = BASE; pKb.endUs = BASE + 800000; pKb.type = 'p';
        pKb.kbAttr = true; pKb.hover = false; pKb.pathLen = 200.f;
        Op pk = rt(pKb);
        CHECK(pk.kind == OpKind::Normal && pk.paintCount == 1,
              "按住键的数位笔落笔 kind 变 Normal 但仍计入绘画动作");

        // 鼠标移动段 → 不算绘画动作
        RawRecord mMove;
        mMove.tUs = BASE; mMove.endUs = BASE + 300000; mMove.type = 'm';
        mMove.button = 0; mMove.kbAttr = false; mMove.pathLen = 50.f;
        Op mm = rt(mMove);
        CHECK(mm.paintCount == 0, "鼠标自由移动不计入绘画动作");

        // 鼠标右键/中键 → 不算绘画动作（多为菜单与平移）
        RawRecord mRight;
        mRight.tUs = BASE; mRight.endUs = BASE + 100000; mRight.type = 'M';
        mRight.button = 1; mRight.pathLen = 10.f;
        Op mr = rt(mRight);
        CHECK(mr.paintCount == 0, "鼠标右键笔画不计入绘画动作");

        // 数位笔悬空 → 整条被丢弃
        RawRecord pHover;
        pHover.tUs = BASE; pHover.endUs = BASE + 300000; pHover.type = 'p';
        pHover.hover = true; pHover.pathLen = 50.f;
        Op ph = rt(pHover);
        CHECK(ph.paintCount == 0 && ph.t == 0, "数位笔悬空移动不产生绘画动作");

        // 键盘与滚轮 → 不算绘画动作
        RawRecord key;
        key.tUs = BASE; key.endUs = BASE + 100000; key.type = 'k';
        key.kind = OpKind::Normal; key.vk = 'B'; key.count = 1;
        Op ok = rt(key);
        CHECK(ok.paintCount == 0, "键盘操作不计入绘画动作");

        // 合并守恒：多个鼠标笔画被吸收进同一事件后，paintCount 之和不变
        std::vector<Op> seq;
        for (int i = 0; i < 5; ++i) {
            Op o;
            o.t = BASE + (int64_t)i * 200000;       // 间隔 200ms < 1s 合并窗口
            o.end = o.t + 100000;
            o.kind = OpKind::Normal;                // 鼠标笔画的 kind
            o.src = 1;
            o.paintCount = 1;
            o.merged = 1;
            seq.push_back(o);
        }
        std::vector<Op> mg = MergeOps(seq);
        int sumPaint = 0;
        for (const auto& o : mg) sumPaint += o.paintCount;
        printf("    5 个鼠标笔画(间隔200ms) 合并后 -> %zu 个事件，paintCount 合计 %d\n",
               mg.size(), sumPaint);
        CHECK(mg.size() == 1, "5 个密集鼠标笔画合并为一个事件");
        CHECK(sumPaint == 5, "合并后绘画动作数守恒（5 个不丢）");
    }

    // --------------------------------------------------------
    // [10] 块级绘画动作计数：纯鼠标作画的块不应被算成「零画笔」
    //      这是 paintCount 存在的理由 —— brushCount 在这种块上恒为 0。
    // --------------------------------------------------------
    printf("\n[10] 块级 paintCount（纯鼠标作画场景）\n");
    {
        std::vector<Op> mouse;
        int64_t t = BASE;
        for (int i = 0; i < 30; ++i) {
            Op o;
            o.t = t; o.end = t + 100000;
            o.kind = OpKind::Normal;   // 鼠标笔画
            o.src = 1;
            o.paintCount = 1;
            o.merged = 1;
            mouse.push_back(o);
            t = o.end + 1200000;       // 间隔 1.2s > 合并窗口，各自独立成事件
        }
        // 掺入撤回，构造「密集作画 + 频繁修正」的块
        for (int i = 0; i < 10; ++i) {
            Op u;
            u.t = t; u.end = t + 50000;
            u.kind = OpKind::Undo; u.src = 0; u.paintCount = 0; u.merged = 1;
            mouse.push_back(u);
            t = u.end + 1200000;
        }
        Thresholds fix;
        fix.n1Auto = fix.n2Auto = fix.depthAuto = fix.mergeAuto = false;
        fix.n1Sec = 45.0; fix.n2Sec = 300.0; fix.mergeSec = 2.0; fix.depthGapSec = 2.0;
        Analysis ma = Analyze(mouse, fix);
        CHECK(ma.blockCount == 1, "纯鼠标作画场景构成 1 个作画块");
        if (!ma.blocks.empty()) {
            const PaintBlock& b = ma.blocks[0];
            printf("    paintCount=%d  brushCount=%d  undoCount=%d  undoPerPaint=%.4f\n",
                   b.paintCount, b.brushCount, b.undoCount, b.undoPerPaint);
            CHECK(b.paintCount == 30, "块级绘画动作数 = 30（全部鼠标笔画都数到了）");
            CHECK(b.brushCount == 0,
                  "块级 brushCount = 0（证明按画笔统计会把这个块误判为零绘画）");
            CHECK(b.undoPerPaint > 0.3 && b.undoPerPaint < 0.34,
                  "撤回率 = 10/30 ≈ 0.333（分母用的是设备无关的绘画动作数）");
        }
    }

    // --------------------------------------------------------
    // [11] 目标绘画软件的默认清单：优动漫 PAINT 必须独立列出
    //
    // 背景：清单里曾写着「优动漫 PAINT 可执行文件与 CSP 同名，故一并覆盖」。
    // 该假设是错的——优动漫是独立打包发行的产品（厦门联合优创），进程名与
    // CSP 不同。后果是优动漫用户的作画完全不被识别、静默不记录：没有任何
    // 报错，用户只会发现「明明画了，软件里却没有数据」。
    // 用户实测进程名为 UDMPaintPRO.exe。
    //
    // 这组断言锁住三件事：
    //   1. 默认清单确实含优动漫进程名（不是靠 clipstudiopaint.exe 覆盖）
    //   2. IsTarget 在采集层的小写归一化口径下能命中真实进程名
    //   3. 其余既有目标未被误删；清单每项都是小写全名（精确匹配的前提）
    // --------------------------------------------------------
    printf("\n[11] 目标软件默认清单（优动漫 PAINT）\n");
    {
        // 采集层口径：Capture.cpp 的 UpdateForeground 先对进程名做 ToLowerA，
        // 再调 Cfg().IsTarget(exe)，而 IsTarget 是 t == exeLower 的精确相等。
        // 所以清单必须写成小写全名，大小写或后缀不符都会静默不命中。
        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return s;
        };

        size_t n = 0;
        const char* const* defs = DefaultTargets(n);
        bool hasUdm = false, hasCsp = false, allLower = true;
        const char* badItem = "";
        for (size_t i = 0; i < n; ++i) {
            std::string t = lower(defs[i]);
            if (t != std::string(defs[i])) { allLower = false; badItem = defs[i]; }
            if (t == "udmpaintpro.exe")       hasUdm = true;
            if (t == "clipstudiopaint.exe")   hasCsp = true;
        }
        if (!allLower) printf("     非小写项: %s\n", badItem);
        CHECK(allLower, "默认清单每项都是小写全名（IsTarget 精确匹配的前提）");
        CHECK(hasUdm, "默认清单含优动漫 PAINT 进程名 udmpaintpro.exe");
        CHECK(hasCsp, "默认清单仍含 Clip Studio Paint");
        CHECK(n >= 6, "默认清单条目数不少于 6（防止误删既有目标）");

        Config c;
        c.LoadDefaults();
        // 真实进程名是大小写混合的 UDMPaintPRO.exe，经采集层归一化后应命中
        CHECK(c.IsTarget(lower("UDMPaintPRO.exe")),
              "IsTarget 命中优动漫（真实大小写 UDMPaintPRO.exe）");
        CHECK(!c.IsTarget("udmpaintpro"), "IsTarget 不接受缺 .exe 后缀的名字");
        CHECK(!c.IsTarget("paintpro.exe"), "IsTarget 不接受部分匹配");
        CHECK(!c.IsTarget("udmpaint.exe"), "IsTarget 不接受未列入清单的同族名");

        // 关键回归：优动漫**不能**靠 CSP 的进程名覆盖。
        // 若将来有人重新相信「同名即可」而删掉 udmpaintpro.exe，此断言会失败。
        Config cspOnly;
        cspOnly.LoadDefaults();
        cspOnly.targets.clear();
        cspOnly.targets.push_back("clipstudiopaint.exe");
        cspOnly.targets.push_back("clipstudio.exe");
        CHECK(!cspOnly.IsTarget(lower("UDMPaintPRO.exe")),
              "仅有 CSP 进程名时无法命中优动漫（证伪「可执行文件同名」假设）");

        // 既有目标未被误删
        CHECK(c.IsTarget("photoshop.exe"), "默认清单仍含 Photoshop");
        CHECK(c.IsTarget("sai2.exe"), "默认清单仍含 PaintTool SAI 2");
        CHECK(c.IsTarget("sai.exe"), "默认清单仍含 PaintTool SAI 1");
    }

    printf("\n==== 结果：%d 通过，%d 失败 ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
