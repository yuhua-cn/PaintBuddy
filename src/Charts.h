#pragma once
// ============================================================
//  图表层：两张核心可视化
//
//  ① GanttView —— 总览（类睡眠甘特图）
//     · 每个「作画块」一行，按天分组
//     · 底色带：深度绘画 / 浅度绘画
//     · 带内由操作小段组成：撤回(红) / 画笔(橙) / 普通(蓝)
//     · 缩放比例大时，小段宽度不足 1px → 按占比混色，
//       于是深浅块自然「糊」成一片，与需求一致
//     · 打断绘画：显示上限 1 小时，并以省略标记注明真实时长
//     · 暂停绘画 / 普通间隔：不同中性色
//
//  ② HabitView —— 操作习惯与内在关联
//     · 5×5 一阶转移概率热力图
//     · 长程关联（滞后 2..5 阶）热力图 + 提升度
//     · 二阶条件转移（给定前两个状态）
//     · 显著关联清单（按提升度排序）
//     · 支持「全部数据」与「时间窗口」两种口径
// ============================================================
#include "Common.h"
#include "Analyzer.h"
#include "Ui.h"
#include <string>
#include <vector>

namespace pb {

// 交互返回：需要重绘
enum class ChartHit : uint8_t {
    None = 0, Wheel, Drag, Hover, Click,
};

// 悬停提示
struct Tooltip {
    bool show = false;
    float x = 0, y = 0;
    std::vector<std::wstring> lines;   // 首行标题加粗
};

// ------------------------------------------------------------
//  ① 甘特总览 —— 横向时间轴 × 三状态行
//
//  · 列 = 时间，采用「可变单位」：每个连续段的列宽按 sqrt(时长)
//    压缩，短段可见、长段不吞屏；打断的显示时长封顶 1 小时
//  · 行 = 三个状态行：深度绘画 | 浅度绘画 | 暂停绘画
//  · 每个连续作画段在深/浅行内占一条横向带；暂停段在暂停行
//  · 打断绘画 = 贯穿三行的一整块（时间轴上的竖向通栏带），
//    标注真实时长与封顶说明
//  · 深/浅带内按 撤回 / 画笔 / 普通 的时间占比画横向三色段；
//    列宽不足时按比例混色，自然「糊」成一片
// ------------------------------------------------------------
class GanttView {
public:
    void SetData(const Analysis& a);          // 设置数据并复位视图（适应全图）
    void UpdateData(const Analysis& a);        // 更新数据但保留当前缩放/平移（自动刷新用）
    void SetTheme(const Theme& t) { theme_ = t; }
    bool Empty() const { return segs_.empty(); }

    void FitAll();                          // 复位缩放与平移（整图适配）
    void ZoomAt(float px, float factor, const D2D1_RECT_F& rect);
    // 把视图横向居中到指定真实时刻（落在打断区时钳制到最近段边界）。
    // 预览工具用它把放大后的视图对准作画段，以检查逐笔明细层。
    void CenterOnReal(int64_t realUs, const D2D1_RECT_F& rect);

    float Draw(Painter& p, const D2D1_RECT_F& rect, float scrollY);

    ChartHit OnWheel(float x, float y, float delta, const D2D1_RECT_F& rect);
    ChartHit OnMove(float x, float y, const D2D1_RECT_F& rect);
    ChartHit OnDown(float x, float y, const D2D1_RECT_F& rect);
    ChartHit OnUp(float x, float y, const D2D1_RECT_F& rect);
    void     OnLeave();

    const Tooltip& Tip() const { return tip_; }
    bool     IsDragging() const { return dragging_; }

    float ContentHeight() const;
    int   RowCount() const { return (int)segs_.size(); }

    // 淡入淡出是否仍在进行：为真时调用方应持续重绘以推进动画，
    // 为假后可停止重绘，避免无谓的 CPU/GPU 开销。
    bool  Animating() const { return fabsf(mixNow_ - mixTarget_) > 0.002f; }

    // 当前「概括 ↔ 明细」混合系数：0 = 只显示概括色块，1 = 只显示逐笔明细。
    // 公开只读，供预览工具目检淡入淡出是否收敛到预期层级。
    float DetailMix() const { return mixNow_; }

    static void DrawLegend(Painter& p, const D2D1_RECT_F& rect, const Theme& th);

private:
    enum class HKind : uint8_t { Act = 0, Pause = 1, Interrupt = 2 };
    struct HSeg {
        HKind   kind = HKind::Act;
        bool    deep = false;
        float   duStart = 0, duWidth = 0;   // 显示单位（sqrt 秒）坐标
        int64_t realStart = 0, realEnd = 0;
        double  tCorrect = 0, tBrush = 0, tNormal = 0;   // 秒
        int     opCount = 0, undoCount = 0, brushCount = 0, normalCount = 0;
        bool    truncated = false;          // 打断被封顶
    };

    // 逐笔操作明细（放大到一定程度后取代概括色块显示）
    struct OpMark {
        float   duStart = 0, duWidth = 0;   // 与 HSeg 同一 du 坐标系
        int64_t realStart = 0, realEnd = 0;
        OpKind  kind = OpKind::Normal;
        uint8_t src = 0;                    // 0=键盘 1=鼠标 2=数位笔
        int     row = 0;                    // 0=深度行 1=浅度行
        std::wstring note;                  // "撤回 Ctrl+Z x2" / "画笔笔画" / …
    };

    struct DateTick {
        float du = 0.f;
        std::wstring label;
        bool isDate = true;    // 日期主刻度；否则为日内时刻副刻度
    };

    void  Rebuild();
    // 明细层混合系数 0..1：0 = 只看概括色块，1 = 只看逐笔操作。
    // 由当前缩放比例决定，两层用它做交叉淡入淡出。
    float AdvanceFade(float pxPerDu);
    float DuOf(int64_t durUs, bool capped) const;
    float PxPerDu(const D2D1_RECT_F& rect) const;
    float XOf(float du, const D2D1_RECT_F& rect) const;
    float DuAt(float x, const D2D1_RECT_F& rect) const;
    void  ClampOffset(const D2D1_RECT_F& rect);
    void  BuildTooltipFor(float x, float y, const D2D1_RECT_F& rect);

    Analysis          data_;
    std::vector<HSeg> segs_;
    std::vector<OpMark> opMarks_;      // 逐笔操作明细（与 segs_ 同一 du 坐标系）
    std::vector<DateTick> dateTicks_;
    float             totalDU_ = 1.f;
    Theme             theme_;

    // 淡入淡出：目标混合系数由缩放决定，实际值按帧向目标逼近，
    // 于是「概括色块 ↔ 逐笔操作」的切换是一段交叉渐变而非瞬间跳变。
    float             mixTarget_ = 0.f;   // 0=概括 1=明细
    float             mixNow_    = 0.f;   // 当前实际混合系数
    float             lastPxPerDu_ = 1.f; // 上一帧的 du→像素比例（DetailMix 用）
    int64_t           lastAnimUs_ = 0;    // 上一帧时刻，用于按真实时间推进动画
    // 淡入淡出时长（秒）：太短看不清过渡，太长显得拖沓
    static const float FADE_SECONDS;
    // 明细层可见的判定：每笔操作的平均像素宽度超过此值才淡入。
    // 低于它时逐笔色块宽度不足 1px，画出来只会是一片噪点。
    static const float OP_PX_THRESHOLD;

    float userScale_  = 1.f;
    float offsetDU_   = 0.f;
    float rowH_       = 62.f;
    float headH_      = 26.f;
    float leftGutter_ = 104.f;

    bool  dragging_ = false;
    float dragLastX_ = 0;
    bool  hoverValid_ = false;
    Tooltip tip_;
    float lastRectLeft_ = 0, lastRectRight_ = 0;
};

// ------------------------------------------------------------
//  ② 习惯与内在关联
// ------------------------------------------------------------
class HabitView {
public:
    void SetData(const Analysis& a);
    void SetTheme(const Theme& t) { theme_ = t; }
    bool Empty() const { return empty_; }

    float Draw(Painter& p, const D2D1_RECT_F& rect, float scrollY);
    float ContentHeight(float width) const;

    ChartHit OnMove(float x, float y, const D2D1_RECT_F& rect);
    void     OnLeave();
    const Tooltip& Tip() const { return tip_; }

    // 当前展示的子视图。
    // 序号即分段控件的排布顺序：习惯概览放首位并作为默认页，
    // 用户打开习惯页先看到总览统计，再按需切到各阶转移矩阵。
    enum class Panel : uint8_t { Summary = 0, Transition = 1, LongRange = 2, SecondOrder = 3 };
    void  SetPanel(Panel pp) { panel_ = pp; }
    Panel GetPanel() const { return panel_; }
    void  SetLag(int lag) { lag_ = std::max(2, std::min(MAX_LAG, lag)); }
    int   GetLag() const { return lag_; }
    // 二阶视图的固定前缀状态
    void  SetPrefix(State a, State b) { preA_ = a; preB_ = b; }
    State GetPrefixA() const { return preA_; }
    State GetPrefixB() const { return preB_; }

    // 热力图取值上下文（DrawMatrix 的回调需要，故置于公开区）
    struct MatrixCtx {
        const Analysis* a = nullptr;
        int lag = 1;
        bool useLift = false;
        bool useLag2 = false;
        State preA = State::Normal, preB = State::Normal;
    };

private:
    float DrawTransition(Painter& p, const D2D1_RECT_F& r, float y);
    float DrawLongRange(Painter& p, const D2D1_RECT_F& r, float y);
    float DrawSecondOrder(Painter& p, const D2D1_RECT_F& r, float y);
    float DrawSummary(Painter& p, const D2D1_RECT_F& r, float y);

    // 画一个 N×N 热力图，返回占用高度
    float DrawMatrix(Painter& p, float x, float y, float w, int n,
                     const std::vector<std::wstring>& labels,
                     const std::wstring& rowTitle, const std::wstring& colTitle,
                     double (*cell)(void*, int, int), void* ctx,
                     bool showPercent, int* hoverI = nullptr, int* hoverJ = nullptr);

    struct MatrixCtxUnused { int dummy = 0; };

    Analysis  data_;
    Theme     theme_;
    bool      empty_ = true;
    Panel     panel_ = Panel::Summary;   // 默认先看习惯概览
    int       lag_ = 2;
    State     preA_ = State::Normal;
    State     preB_ = State::NormalGap;   // 默认前缀取高频组合，避免空矩阵

    Tooltip   tip_;
    int       hoverI_ = -1, hoverJ_ = -1;
    // 记录各面板绘制区域，用于命中测试
    struct Region { int top = 0, bottom = 0; int kind = 0; };
    std::vector<Region> regions_;
    D2D1_RECT_F lastRect_ = RF(0, 0, 0, 0);
    float       lastScrollY_ = 0;
    float       cellTop_ = 0, cellLeft_ = 0, cellSize_ = 0;
};

} // namespace pb
