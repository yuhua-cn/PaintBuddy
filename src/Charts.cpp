#include "Charts.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pb {

// ============================================================
//  公共：状态取色
// ============================================================
static D2D1_COLOR_F StateColor(const Theme& th, State s) {
    switch (s) {
    case State::Brush:     return th.cBrush;
    case State::Correct:   return th.cUndo;
    case State::Normal:    return th.cNormal;
    case State::NormalGap: return th.cGapNormal;
    case State::Pause:     return th.cPause;
    default:               return th.label3;
    }
}

static std::wstring StateLabel(State s) {
    return U2W(StateNameCn(s));
}

// ============================================================
//  ① GanttView —— 横向时间轴 × 三状态行
//
//  · 列 = 时间，可变单位：每连续段宽 = sqrt(秒)×k，短段可见、
//    长段不吞屏；打断显示封顶 1 小时
//  · 行 = 深度绘画 / 浅度绘画 / 暂停绘画
//  · 打断 = 贯穿三行的竖向通栏块
//  · 深/浅带内按 撤回/画笔/普通 占比画横向三色段；列宽不足时
//    与底色混合，自然「糊」成一片
// ============================================================
void GanttView::SetData(const Analysis& a) {
    data_ = a;
    Rebuild();
    FitAll();
}

// 更新数据但保留当前缩放/平移（供「边画边自动刷新」使用）。
// 不调用 FitAll —— 否则每次后台重算完成都会把视图弹回全图适配，
// 打断用户正在放大的局部。越界的偏移会在下一次 Draw 的 ClampOffset 里修正。
void GanttView::UpdateData(const Analysis& a) {
    data_ = a;
    Rebuild();
}

float GanttView::DuOf(int64_t durUs, bool capped) const {
    double sec = std::max(0.0, (double)durUs / 1e6);
    if (capped) sec = std::min(3600.0, sec);
    double du = 5.0 * sqrt(sec);
    if (du < 2.0) du = 2.0;
    return (float)du;
}

// 淡入淡出时长：太短看不清过渡，太长在连续缩放时显得拖沓
const float GanttView::FADE_SECONDS = 0.28f;
// 每笔操作的平均像素宽度超过此值才切到明细层。
// 低于它时逐笔色块不足 1px，画出来只是一片噪点，不如继续显示概括色块。
const float GanttView::OP_PX_THRESHOLD = 6.0f;

// 计算目标层级并按真实时间推进混合系数。
// 返回 0..1：0 = 只显示概括色块，1 = 只显示逐笔操作明细。
// 用 QPC 计算帧间隔，因此动画速度与刷新率无关（离屏渲染单次调用也正确）。
float GanttView::AdvanceFade(float pxPerDu) {
    if (opMarks_.empty()) { mixTarget_ = 0.f; mixNow_ = 0.f; return 0.f; }
    lastPxPerDu_ = pxPerDu;

    // 目标层级：按「平均每笔操作占多少像素」判断。
    // 总 du 跨度 × 每 du 像素 = 全图总像素宽，除以操作数即平均每笔宽度。
    double totalPx = (double)totalDU_ * (double)pxPerDu;
    double avgPxPerOp = (data_.ops.empty()) ? 0.0 : totalPx / (double)data_.ops.size();
    // 加一点滞回：进入明细要求 ≥ 阈值，退回概括要求 < 阈值的 70%，
    // 否则在临界点附近来回缩放会让两层反复闪烁。
    if (mixTarget_ < 0.5f) {
        if (avgPxPerOp >= (double)OP_PX_THRESHOLD) mixTarget_ = 1.f;
    } else {
        if (avgPxPerOp < (double)OP_PX_THRESHOLD * 0.7) mixTarget_ = 0.f;
    }

    // 按真实时间推进
    int64_t nowUs = Clock::NowUs();
    if (lastAnimUs_ == 0) lastAnimUs_ = nowUs;
    float dtSec = (float)((double)(nowUs - lastAnimUs_) / 1e6);
    lastAnimUs_ = nowUs;
    if (dtSec < 0.f) dtSec = 0.f;
    if (dtSec > 0.5f) dtSec = 0.5f;   // 长时间未重绘后不要一步跳到终点

    float step = (FADE_SECONDS > 0.f) ? (dtSec / FADE_SECONDS) : 1.f;
    if (step >= 1.f) mixNow_ = mixTarget_;
    else if (mixNow_ < mixTarget_) mixNow_ = std::min(mixTarget_, mixNow_ + step);
    else if (mixNow_ > mixTarget_) mixNow_ = std::max(mixTarget_, mixNow_ - step);
    return mixNow_;
}

float GanttView::ContentHeight() const {
    return headH_ + rowH_ * 3.f + 10.f;
}

void GanttView::CenterOnReal(int64_t realUs, const D2D1_RECT_F& rect) {
    // 定位 realUs 对应的 du 坐标：优先落在包含它的段内线性内插；
    // 落在打断压缩区（不在任何段内）时钳制到最近的段边界。
    float targetDu = 0.f;
    if (!segs_.empty()) {
        bool found = false;
        for (const HSeg& s : segs_) {
            if (realUs >= s.realStart && realUs <= s.realEnd) {
                double f = (s.realEnd > s.realStart)
                    ? (double)(realUs - s.realStart) / (double)(s.realEnd - s.realStart)
                    : 0.0;
                targetDu = s.duStart + (float)f * s.duWidth;
                found = true;
                break;
            }
        }
        if (!found) {
            float best = 0.f; double bestDist = 1e18;
            for (const HSeg& s : segs_) {
                double d0 = fabs((double)(s.realStart - realUs));
                double d1 = fabs((double)(s.realEnd - realUs));
                if (d0 < bestDist) { bestDist = d0; best = s.duStart; }
                if (d1 < bestDist) { bestDist = d1; best = s.duStart + s.duWidth; }
            }
            targetDu = best;
        }
    }

    float ppd = PxPerDu(rect);
    if (ppd <= 0) return;
    float viewDU = (rect.right - rect.left - leftGutter_) / ppd;
    offsetDU_ = targetDu - viewDU * 0.5f;
    ClampOffset(rect);
}

void GanttView::FitAll() {
    userScale_ = 1.f;
    offsetDU_ = 0.f;
}

float GanttView::PxPerDu(const D2D1_RECT_F& rect) const {
    float plotW = rect.right - rect.left - leftGutter_;
    if (plotW <= 0 || totalDU_ <= 0) return 1.f;
    return plotW / totalDU_ * userScale_;
}

float GanttView::XOf(float du, const D2D1_RECT_F& rect) const {
    return rect.left + leftGutter_ + (du - offsetDU_) * PxPerDu(rect);
}

float GanttView::DuAt(float x, const D2D1_RECT_F& rect) const {
    float ppd = PxPerDu(rect);
    if (ppd <= 0) return 0.f;
    return offsetDU_ + (x - rect.left - leftGutter_) / ppd;
}

void GanttView::ClampOffset(const D2D1_RECT_F& rect) {
    float ppd = PxPerDu(rect);
    if (ppd <= 0) return;
    float viewDU = (rect.right - rect.left - leftGutter_) / ppd;
    if (userScale_ <= 1.0001f) { offsetDU_ = 0.f; return; }
    float maxOff = std::max(0.f, totalDU_ - viewDU);
    if (offsetDU_ < 0.f) offsetDU_ = 0.f;
    if (offsetDU_ > maxOff) offsetDU_ = maxOff;
}

void GanttView::ZoomAt(float px, float factor, const D2D1_RECT_F& rect) {
    float anchor = DuAt(px, rect);
    float old = userScale_;
    userScale_ = std::max(1.f, std::min(64.f, userScale_ * factor));
    if (userScale_ == old) return;
    // 保持锚点不动
    float ppd = PxPerDu(rect);
    if (ppd > 0) offsetDU_ = anchor - (px - rect.left - leftGutter_) / ppd;
    ClampOffset(rect);
}

void GanttView::Rebuild() {
    segs_.clear();
    opMarks_.clear();
    dateTicks_.clear();
    if (data_.blocks.empty()) { totalDU_ = 1.f; return; }

    const double n1Us = data_.th.n1Sec * 1e6;
    const int64_t DAY = 86400LL * 1000000LL;
    float du = 0.f;

    size_t oi = 0;
    auto advanceTo = [&](int64_t t) {
        while (oi < data_.ops.size() && data_.ops[oi].t < t) ++oi;
    };

    auto addDateTicks = [&](int64_t rs, int64_t re, float duStart, float duW) {
        if (re <= rs || duW <= 0) return;
        const int64_t HOUR = 3600LL * 1000000LL;
        const int64_t DAY = 86400LL * 1000000LL;
        // 日期主刻度：段内跨过的每个 0 点
        int64_t d = DayStartUs(rs) + DAY;
        for (; d < re; d += DAY) {
            float t = (float)(((double)(d - rs) / (double)(re - rs)) * (double)duW);
            DateTick tk;
            tk.du = duStart + t;
            tk.label = FormatDateCn(d);
            tk.isDate = true;
            dateTicks_.push_back(tk);
        }
        // 日内时刻副刻度：按段长选步长（1/2/3/6 小时）
        double hours = (double)(re - rs) / 3600e6;
        int stepH = hours > 24 ? 6 : (hours > 12 ? 3 : (hours > 6 ? 2 : (hours > 2 ? 1 : 0)));
        if (stepH > 0) {
            int64_t step = stepH * HOUR;
            int64_t t0 = ((rs + step - 1) / step) * step;
            for (int64_t t = t0; t < re; t += step) {
                if (t == DayStartUs(t)) continue;   // 0 点已由日期刻度表示
                float f = (float)(((double)(t - rs) / (double)(re - rs)) * (double)duW);
                DateTick tk;
                tk.du = duStart + f;
                tk.label = FormatTimeOfDay(t);
                tk.isDate = false;
                dateTicks_.push_back(tk);
            }
        }
    };

    for (size_t bi = 0; bi < data_.blocks.size(); ++bi) {
        const PaintBlock& b = data_.blocks[bi];

        // —— 块间打断：贯穿三行的通栏块 ——
        if (bi > 0) {
            int64_t gs = data_.blocks[bi - 1].end;
            int64_t ge = b.start;
            if (ge > gs) {
                HSeg s;
                s.kind = HKind::Interrupt;
                s.realStart = gs; s.realEnd = ge;
                s.truncated = (ge - gs) > INTERRUPT_DISPLAY_CAP_US;
                s.duStart = du;
                s.duWidth = DuOf(ge - gs, true);
                segs_.push_back(s);
                addDateTicks(gs, ge, du, s.duWidth);
                du += s.duWidth;
            }
        }

        // —— 块内：按暂停间隔切分连续作画段 ——
        advanceTo(b.start);
        int64_t actStart = -1, actEnd = -1, prevEnd = -1;
        double tC = 0, tB = 0, tN = 0;
        int cOp = 0, cU = 0, cB = 0, cN = 0;
        // 当前段内的操作下标：段的 du 起点要到 flushAct 才确定，
        // 所以先攒着，flush 时再按时间比例换算成 du 坐标。
        std::vector<size_t> curOps;

        auto flushAct = [&]() {
            if (actStart < 0) return;
            HSeg s;
            s.kind = HKind::Act;
            s.deep = b.deep;
            s.realStart = actStart;
            s.realEnd = std::max(actEnd, actStart + 1);
            s.tCorrect = tC; s.tBrush = tB; s.tNormal = tN;
            s.opCount = cOp; s.undoCount = cU; s.brushCount = cB; s.normalCount = cN;
            s.duStart = du;
            s.duWidth = DuOf(s.realEnd - s.realStart, false);
            segs_.push_back(s);
            addDateTicks(s.realStart, s.realEnd, du, s.duWidth);

            // —— 逐笔操作明细：段内按真实时间线性映射到 du ——
            // 注意 du 的 sqrt 压缩只作用在「段」这一级，段内部是线性的，
            // 因此这里用时间比例换算即可，与概括色块严格对齐。
            int64_t span = std::max<int64_t>(1, s.realEnd - s.realStart);
            for (size_t k : curOps) {
                const Op& o = data_.ops[k];
                OpMark m;
                m.realStart = o.t;
                m.realEnd = std::max(o.end, o.t + 1);
                m.kind = o.kind;
                m.src = o.src;
                m.row = b.deep ? 0 : 1;
                m.note = U2W(o.note);
                float f0 = (float)((double)(o.t - s.realStart) / (double)span);
                float f1 = (float)((double)(m.realEnd - s.realStart) / (double)span);
                if (f0 < 0.f) f0 = 0.f;
                if (f1 > 1.f) f1 = 1.f;
                if (f1 < f0) f1 = f0;
                m.duStart = s.duStart + f0 * s.duWidth;
                m.duWidth = std::max(0.02f, (f1 - f0) * s.duWidth);
                opMarks_.push_back(m);
            }
            curOps.clear();

            du += s.duWidth;
            actStart = -1; actEnd = -1;
            tC = tB = tN = 0; cOp = cU = cB = cN = 0;
        };

        while (oi < data_.ops.size() && data_.ops[oi].t < b.end) {
            size_t opIdx = oi;
            const Op& o = data_.ops[oi++];
            double dur = std::max(0.05, (double)(o.end - o.t) / 1e6);

            if (actStart >= 0 && prevEnd >= 0 && (double)(o.t - prevEnd) >= n1Us) {
                flushAct();
                HSeg ps;
                ps.kind = HKind::Pause;
                ps.realStart = prevEnd;
                ps.realEnd = o.t;
                ps.duStart = du;
                ps.duWidth = DuOf(o.t - prevEnd, false);
                segs_.push_back(ps);
                addDateTicks(ps.realStart, ps.realEnd, du, ps.duWidth);
                du += ps.duWidth;
            }
            if (actStart < 0) actStart = o.t;
            actEnd = std::max(actEnd, o.end);
            prevEnd = std::max(prevEnd, o.end);
            switch (o.kind) {
            case OpKind::Undo: case OpKind::Redo: tC += dur; ++cU; break;
            case OpKind::Brush:                   tB += dur; ++cB; break;
            default:                              tN += dur; ++cN; break;
            }
            ++cOp;
            curOps.push_back(opIdx);
        }
        flushAct();
    }
    totalDU_ = std::max(1.f, du);
}

// ------------------------------------------------------------
//  绘制
// ------------------------------------------------------------
float GanttView::Draw(Painter& p, const D2D1_RECT_F& rect, float scrollY) {
    (void)scrollY;
    lastRectLeft_ = rect.left;
    lastRectRight_ = rect.right;
    const Theme& th = p.Th();

    if (segs_.empty()) {
        p.TextWrap(L"暂无数据。在 Photoshop / Clip Studio Paint（优动漫）/ SAI2 中作画后，这里会出现三状态行的总览甘特图。",
                   Inset(rect, 20.f, 0.f), 15.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Center);
        return ContentHeight();
    }

    ClampOffset(rect);
    float plotL = rect.left + leftGutter_;
    float plotR = rect.right;
    if (plotR <= plotL) return ContentHeight();

    float top = rect.top + headH_;
    float rowsTop[3] = { top, top + rowH_, top + rowH_ * 2.f };
    float rowsBot = top + rowH_ * 3.f;

    p.PushClip(rect);

    // —— 行底色与行标签 ——
    static const wchar_t* const rowNames[3] = { L"深度绘画", L"浅度绘画", L"暂停绘画" };
    D2D1_COLOR_F rowCols[3] = { th.cDeep, th.cShallow, th.cPause };
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F rr = RF(rect.left, rowsTop[i], rect.right, rowsTop[i] + rowH_);
        p.FillRect(rr, i == 1 ? th.cardAlt : th.card);
        p.FillRound(RF(rect.left + 8.f, rowsTop[i] + rowH_ * 0.5f - 8.f,
                       rect.left + 14.f, rowsTop[i] + rowH_ * 0.5f + 8.f), 3.f, rowCols[i]);
        p.Text(rowNames[i], RF(rect.left + 20.f, rowsTop[i], plotL - 6.f, rowsTop[i] + rowH_),
               12.f, TWeight::Semibold, th.label, TAlign::Left, TVAlign::Center);
        p.Line(plotL - 4.f, rowsTop[i], plotL - 4.f, rowsTop[i] + rowH_, th.separator, 1.f);
        p.Line(rect.left, rowsTop[i] + rowH_, rect.right, rowsTop[i] + rowH_, th.separator, 1.f);
    }

    // —— 刻度：先画头部标签（带碰撞检测，避免标签重叠）——
    // 竖参考线放在数据层之后，淡色不喧宾夺主
    D2D1_COLOR_F dateLine = th.dark ? Col(255, 255, 255, 0.10f) : Col(0, 0, 0, 0.08f);
    D2D1_COLOR_F subLine  = th.dark ? Col(255, 255, 255, 0.06f) : Col(0, 0, 0, 0.05f);
    {
        std::vector<const DateTick*> dates, subs;
        for (const DateTick& tk : dateTicks_) {
            if (tk.isDate) dates.push_back(&tk); else subs.push_back(&tk);
        }
        std::sort(dates.begin(), dates.end(),
                  [](const DateTick* a, const DateTick* b) { return a->du < b->du; });
        std::sort(subs.begin(), subs.end(),
                  [](const DateTick* a, const DateTick* b) { return a->du < b->du; });

        float lastRight = -1e9f;
        // 第一遍：日期主刻度（优先）
        for (const DateTick* tk : dates) {
            float x = XOf(tk->du, rect);
            if (x < plotL || x > plotR) continue;
            float lw = p.MeasureW(tk->label, 10.f, TWeight::Medium);
            if (x >= lastRight + 10.f) {
                p.Text(tk->label, RF(x + 4.f, rect.top, x + 4.f + lw + 8.f, rect.top + headH_),
                       10.f, TWeight::Medium, th.label2, TAlign::Left, TVAlign::Center);
                lastRight = x + 4.f + lw;
            }
        }
        // 第二遍：时刻副刻度（避让已画标签）
        for (const DateTick* tk : subs) {
            float x = XOf(tk->du, rect);
            if (x < plotL || x > plotR) continue;
            float lw = p.MeasureW(tk->label, 9.f, TWeight::Regular);
            if (x >= lastRight + 10.f) {
                p.Text(tk->label, RF(x + 4.f, rect.top, x + 4.f + lw + 8.f, rect.top + headH_),
                       9.f, TWeight::Regular, th.label3, TAlign::Left, TVAlign::Center);
                lastRight = x + 4.f + lw;
            }
        }
        // 刻度短线（全部画）
        for (const DateTick* tk : dates) {
            float x = XOf(tk->du, rect);
            if (x < plotL || x > plotR) continue;
            p.Line(x, rect.top + headH_ - 6.f, x, rect.top + headH_, th.separator, 1.f);
        }
        for (const DateTick* tk : subs) {
            float x = XOf(tk->du, rect);
            if (x < plotL || x > plotR) continue;
            p.Line(x, rect.top + headH_ - 4.f, x, rect.top + headH_, th.separator, 1.f);
        }
    }

    // —— 段：底色层（深度/浅度/暂停/打断），始终不透明 ——
    // 三色占比段不在这里画：它属于「概括层」，要与「明细层」做交叉淡入淡出，
    // 所以拆到下面单独绘制。底色带代表深浅状态语义，两层切换时都保持可见。
    for (const HSeg& s : segs_) {
        float x0 = XOf(s.duStart, rect);
        float x1 = XOf(s.duStart + s.duWidth, rect);
        if (x1 < plotL - 2 || x0 > plotR + 2) continue;
        float cx0 = std::max(plotL, x0);
        float cx1 = std::min(plotR, x1);

        if (s.kind == HKind::Interrupt) {
            // 贯穿三行的通栏块
            float w = std::max(2.f, cx1 - cx0);
            D2D1_RECT_F bar = RF(cx0, top + 2.f, cx0 + w, rowsBot - 2.f);
            p.FillRoundVGrad(bar, 6.f, th.cInterrupt, MixC(th.cInterrupt, Col(0, 0, 0, 1), 0.18f));
            if (w >= 150.f) {
                wchar_t b[128];
                swprintf_s(b, L"⋯ 打断绘画 %s%s", FormatDurationUs(s.realEnd - s.realStart).c_str(),
                           s.truncated ? L"（显示封顶 1 小时）" : L"");
                p.Text(b, bar, 12.f, TWeight::Medium, Col(255, 255, 255, 0.95f),
                       TAlign::Center, TVAlign::Center);
            } else if (w >= 26.f) {
                p.Text(L"⋯", bar, 14.f, TWeight::Semibold, Col(255, 255, 255, 0.9f),
                       TAlign::Center, TVAlign::Center);
            }
            continue;
        }

        int row = (s.kind == HKind::Pause) ? 2 : (s.deep ? 0 : 1);
        D2D1_RECT_F band = RF(cx0, rowsTop[row] + 6.f, cx1, rowsTop[row] + rowH_ - 6.f);
        if (RW(band) <= 0.5f) continue;

        if (s.kind == HKind::Pause) {
            p.FillRound(band, 5.f, th.cPause);
            continue;
        }

        D2D1_COLOR_F base = s.deep ? th.cDeep : th.cShallow;
        p.FillRoundVGrad(band, 5.f, base, MixC(base, Col(0, 0, 0, 1), 0.22f));
    }

    // —— 概括层 ↔ 明细层：交叉淡入淡出 ——
    // mix=0 只显示概括色块（撤回/画笔/普通的时间占比），
    // mix=1 只显示逐笔操作明细（某一笔、某一次撤回在时间轴上的位置）。
    float mix = AdvanceFade(PxPerDu(rect));
    D2D1_RECT_F plotArea = RF(plotL, top, plotR, rowsBot);

    // 概括层（随放大淡出）
    if (mix < 0.999f) {
        p.PushOpacity(1.f - mix);
        for (const HSeg& s : segs_) {
            if (s.kind != HKind::Act) continue;
            float x0 = XOf(s.duStart, rect);
            float x1 = XOf(s.duStart + s.duWidth, rect);
            if (x1 < plotL - 2 || x0 > plotR + 2) continue;
            int row = s.deep ? 0 : 1;
            D2D1_RECT_F band = RF(std::max(plotL, x0), rowsTop[row] + 6.f,
                                  std::min(plotR, x1), rowsTop[row] + rowH_ - 6.f);
            if (RW(band) <= 1.f) continue;

            D2D1_COLOR_F base = s.deep ? th.cDeep : th.cShallow;
            double tot = s.tCorrect + s.tBrush + s.tNormal;
            if (tot <= 0) continue;
            float cw = RW(band);
            // 色块很窄时与底色按比例混合，自然「糊」成一片（需求规格）
            float blend = cw < 24.f ? 0.45f : (cw < 64.f ? 0.25f : 0.f);
            float x = band.left;
            struct SegC { double t; D2D1_COLOR_F c; };
            SegC sc[3] = { { s.tCorrect, th.cUndo }, { s.tBrush, th.cBrush }, { s.tNormal, th.cNormal } };
            p.PushClip(band);
            for (int k = 0; k < 3; ++k) {
                if (sc[k].t <= 0) continue;
                float w = (float)(sc[k].t / tot) * cw;
                if (w < 0.5f) { x += w; continue; }
                p.FillRect(RF(x, band.top + 2.f, x + w, band.bottom - 2.f),
                           MixC(sc[k].c, base, blend));
                x += w;
            }
            p.PopClip();
        }
        p.PopOpacity();
    }

    // 明细层（随放大淡入）：逐笔操作色块 + 够宽时的操作名标签
    if (mix > 0.001f && !opMarks_.empty()) {
        p.PushOpacity(mix);
        p.PushClip(plotArea);
        for (const OpMark& m : opMarks_) {
            float x0 = XOf(m.duStart, rect);
            float x1 = XOf(m.duStart + m.duWidth, rect);
            if (x1 < plotL - 2 || x0 > plotR + 2) continue;

            D2D1_COLOR_F col;
            switch (m.kind) {
            case OpKind::Undo:  col = th.cUndo;   break;
            case OpKind::Redo:  col = th.cRedo;   break;
            case OpKind::Brush: col = th.cBrush;  break;
            default:            col = th.cNormal; break;
            }

            float w = x1 - x0;
            // 相邻操作之间留缝隙，让「每一次操作」在视觉上可分辨
            float gap = (w > 5.f) ? 1.5f : 0.4f;
            float bw = std::max(1.5f, w - gap);
            int row = (m.row == 0 || m.row == 1) ? m.row : 0;
            D2D1_RECT_F rb = RF(x0, rowsTop[row] + 12.f, x0 + bw, rowsTop[row] + rowH_ - 12.f);
            p.FillRound(rb, 2.5f, col);

            // 够宽时直接标出这是哪一次操作（如「撤回 Ctrl+Z」）
            if (bw >= 46.f && !m.note.empty()) {
                p.Text(m.note, RF(rb.left + 5.f, rb.top, rb.right - 5.f, rb.bottom),
                       10.f, TWeight::Medium, ColHex(0xFFFFFF), TAlign::Left, TVAlign::Center);
            }
        }
        p.PopClip();
        p.PopOpacity();
    }

    // —— 淡色竖参考线（在数据层之上，作为刻度对齐参照，不喧宾夺主）——
    for (const DateTick& tk : dateTicks_) {
        float x = XOf(tk.du, rect);
        if (x < plotL || x > plotR) continue;
        p.Line(x, top, x, rowsBot, tk.isDate ? dateLine : subLine, 1.f);
    }

    // —— 顶部刻度区背景线 ——
    p.Line(rect.left, top - 1.f, rect.right, top - 1.f, th.separator, 1.f);

    // —— 悬停提示 ——
    if (tip_.show) {
        float maxw = 0;
        for (size_t i = 0; i < tip_.lines.size(); ++i)
            maxw = std::max(maxw, p.MeasureW(tip_.lines[i], i == 0 ? 12.f : 11.f, TWeight::Regular));
        float w = maxw + 20.f;
        float h = (float)tip_.lines.size() * 17.f + 12.f;
        float tx = tip_.x + 14.f, ty = tip_.y + 14.f;
        if (tx + w > rect.right) tx = tip_.x - w - 10.f;
        if (ty + h > rect.bottom) ty = rect.bottom - h - 4.f;
        if (tx < rect.left) tx = rect.left + 4.f;
        if (ty < rect.top) ty = rect.top + 4.f;
        D2D1_RECT_F tr = RF(tx, ty, tx + w, ty + h);
        p.FillRound(tr, 8.f, th.dark ? Col(44, 44, 46, 0.97f) : Col(28, 28, 30, 0.94f));
        p.StrokeRound(tr, 8.f, th.separator, 1.f);
        float ly = ty + 7.f;
        for (size_t i = 0; i < tip_.lines.size(); ++i) {
            p.Text(tip_.lines[i], RF(tx + 10.f, ly, tx + w - 10.f, ly + 16.f),
                   i == 0 ? 12.f : 11.f, i == 0 ? TWeight::Semibold : TWeight::Regular,
                   ColHex(0xFFFFFF, i == 0 ? 1.f : 0.78f), TAlign::Left, TVAlign::Center);
            ly += 17.f;
        }
    }

    p.PopClip();
    return ContentHeight();
}

void GanttView::DrawLegend(Painter& p, const D2D1_RECT_F& rect, const Theme& th) {
    struct Item { std::wstring t; D2D1_COLOR_F c; };
    std::vector<Item> items = {
        { L"撤回/重做",  th.cUndo },
        { L"画笔",       th.cBrush },
        { L"普通操作",   th.cNormal },
        { L"深度行",     th.cDeep },
        { L"浅度行",     th.cShallow },
        { L"暂停行",     th.cPause },
        { L"打断（贯穿块）", th.cInterrupt },
    };
    float x = rect.left;
    float cy = rect.top + RH(rect) * 0.5f;
    for (const auto& it : items) {
        p.FillRound(RF(x, cy - 4.5f, x + 9.f, cy + 4.5f), 2.f, it.c);
        float tw = p.MeasureW(it.t, 11.f, TWeight::Regular);
        p.Text(it.t, RF(x + 13.f, rect.top, x + 13.f + tw + 4.f, rect.bottom),
               11.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Center);
        x += 13.f + tw + 16.f;
        if (x > rect.right - 40.f) break;
    }
}

// ------------------------------------------------------------
//  交互：横向平移 / 缩放
// ------------------------------------------------------------
ChartHit GanttView::OnWheel(float x, float y, float delta, const D2D1_RECT_F& rect) {
    (void)y;
    // 滚轮 = 以鼠标为中心放大/缩小（上滚放大、下滚缩小）。
    // ZoomAt 以传入的像素 x 为锚点，缩放时该点下的内容保持不动，
    // 因此视觉上就是「以鼠标为中心」缩放。Ctrl 与否行为一致。
    ZoomAt(x, delta > 0 ? 1.18f : 0.85f, rect);
    BuildTooltipFor(x, y, rect);
    return ChartHit::Wheel;
}

ChartHit GanttView::OnMove(float x, float y, const D2D1_RECT_F& rect) {
    if (dragging_) {
        float ppd = PxPerDu(rect);
        if (ppd > 0) offsetDU_ -= (x - dragLastX_) / ppd;
        ClampOffset(rect);
        dragLastX_ = x;
        BuildTooltipFor(x, y, rect);
        return ChartHit::Drag;
    }
    hoverValid_ = true;
    BuildTooltipFor(x, y, rect);
    return ChartHit::Hover;
}

ChartHit GanttView::OnDown(float x, float y, const D2D1_RECT_F& rect) {
    if (!PtIn(x, y, rect)) return ChartHit::None;
    dragging_ = true;
    dragLastX_ = x;
    return ChartHit::Click;
}

ChartHit GanttView::OnUp(float x, float y, const D2D1_RECT_F& rect) {
    dragging_ = false;
    BuildTooltipFor(x, y, rect);
    return ChartHit::Click;
}

void GanttView::OnLeave() {
    dragging_ = false;
    hoverValid_ = false;
    tip_.show = false;
}

void GanttView::BuildTooltipFor(float x, float y, const D2D1_RECT_F& rect) {
    tip_.show = false;
    tip_.x = x; tip_.y = y;
    tip_.lines.clear();
    if (!hoverValid_ || segs_.empty()) return;

    float plotL = rect.left + leftGutter_;
    float top = rect.top + headH_;
    if (x < plotL || x > rect.right || y < top || y > top + rowH_ * 3.f) return;

    float du = DuAt(x, rect);
    int row = (int)((y - top) / rowH_);
    if (row < 0 || row > 2) return;

    const HSeg* hit = nullptr;
    for (const HSeg& s : segs_) {
        if (du < s.duStart || du >= s.duStart + s.duWidth) continue;
        if (s.kind == HKind::Interrupt) { hit = &s; break; }
        int sr = (s.kind == HKind::Pause) ? 2 : (s.deep ? 0 : 1);
        if (sr == row) { hit = &s; break; }
    }
    if (!hit) return;

    switch (hit->kind) {
    case HKind::Act:
        tip_.lines.push_back(std::wstring(hit->deep ? L"深度绘画" : L"浅度绘画") +
                             L"  " + FormatTimeOfDay(hit->realStart) + L" – " + FormatTimeOfDay(hit->realEnd));
        tip_.lines.push_back(L"时长 " + FormatDurationUs(hit->realEnd - hit->realStart) +
                             L" · " + std::to_wstring(hit->opCount) + L" 次操作");
        {
            wchar_t b[128];
            swprintf_s(b, L"撤回 %d · 画笔 %d · 普通 %d", hit->undoCount, hit->brushCount, hit->normalCount);
            tip_.lines.push_back(b);
        }
        break;
    case HKind::Pause:
        tip_.lines.push_back(L"暂停绘画  " + FormatTimeOfDay(hit->realStart) + L" – " + FormatTimeOfDay(hit->realEnd));
        tip_.lines.push_back(L"时长 " + FormatDurationUs(hit->realEnd - hit->realStart));
        break;
    case HKind::Interrupt:
        tip_.lines.push_back(L"打断绘画  " + FormatDateTimeCn(hit->realStart) + L" – " + FormatDateTimeCn(hit->realEnd));
        tip_.lines.push_back(L"真实时长 " + FormatDurationUs(hit->realEnd - hit->realStart));
        if (hit->truncated) tip_.lines.push_back(L"显示已封顶 1 小时");
        break;
    default: break;
    }
    tip_.show = true;
}

// ============================================================
//  ② HabitView
// ============================================================
void HabitView::SetData(const Analysis& a) {
    data_ = a;
    empty_ = a.empty || a.sequence.size() < 8;
}

float HabitView::ContentHeight(float width) const {
    (void)width;
    const float MATRIX = 294.f;
    float h = 0;
    switch (panel_) {
    case Panel::Transition:
        h = 26.f + 36.f + MATRIX + 8.f + 24.f + 24.f + 24.f + 22.f;
        break;
    case Panel::LongRange: {
        float list = 24.f + 34.f * (float)std::min<size_t>(data_.notables.size(), 6);
        if (data_.notables.empty()) list = 38.f;
        h = 26.f + 36.f + MATRIX + 6.f + 24.f + MATRIX + 8.f + list;
        break;
    }
    case Panel::SecondOrder:
        h = 26.f + 24.f + 36.f + MATRIX + 10.f + 40.f;
        break;
    case Panel::Summary:
        // 136 = 阈值卡片 124 + 卡片后间距 12（与 Draw 里 y += kThCardH + 12.f 对应）
        h = 28.f + 108.f + 108.f + 136.f + 24.f + 32.f * 5.f + 10.f;
        break;
    default: break;
    }
    return h + 40.f;
}

static double CellTransProb(void* ctx, int i, int j) {
    auto* c = (HabitView::MatrixCtx*)ctx;
    if (!c->a) return 0;
    if (c->useLift) {
        long long tot = 0;
        for (int k = 0; k < (int)State::COUNT; ++k) tot += c->a->stateTotal[k];
        if (tot <= 0) return 0;
        double pj = (double)c->a->stateTotal[j] / (double)tot;
        if (pj <= 0) return 0;
        double pij = (c->lag <= 1) ? c->a->trans[i][j] : c->a->lagProb[c->lag - 1][i][j];
        return pij / pj;
    }
    if (c->useLag2) {
        return c->a->lag2Prob[(size_t)i][(size_t)j][(size_t)c->lag];
    }
    if (c->lag <= 1) return c->a->trans[i][j];
    return c->a->lagProb[c->lag - 1][i][j];
}

float HabitView::DrawMatrix(Painter& p, float x, float y, float w, int n,
                            const std::vector<std::wstring>& labels,
                            const std::wstring& rowTitle, const std::wstring& colTitle,
                            double (*cell)(void*, int, int), void* ctx,
                            bool showPercent, int* hoverI, int* hoverJ) {
    const Theme& th = p.Th();
    float labelW = 64.f;
    float topH = 46.f;
    float gw = w - labelW - 8.f;
    float cs = std::min(48.f, gw / (float)n);
    float gx = x + labelW;
    float gy = y + topH;

    p.Text(colTitle, RF(gx, y, gx + cs * n, y + 16.f), 11.f, TWeight::Regular, th.label3,
           TAlign::Center, TVAlign::Center);
    p.Text(rowTitle, RF(x, y + 16.f, x + labelW - 6.f, gy + cs * n), 11.f, TWeight::Regular,
           th.label3, TAlign::Right, TVAlign::Top);

    for (int j = 0; j < n; ++j) {
        D2D1_RECT_F r = RF(gx + cs * j, y + 18.f, gx + cs * (j + 1), gy);
        p.Text(labels[(size_t)j], r, 10.f, TWeight::Medium, th.label2, TAlign::Center, TVAlign::Bottom);
    }
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F r = RF(x, gy + cs * i, gx - 6.f, gy + cs * (i + 1));
        p.Text(labels[(size_t)i], r, 10.f, TWeight::Medium, th.label2, TAlign::Right, TVAlign::Center);
    }

    double maxV = 1e-9;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            maxV = std::max(maxV, cell(ctx, i, j));

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            double v = cell(ctx, i, j);
            float t = (float)(v / maxV);
            D2D1_COLOR_F bg = p.Heat(t);
            D2D1_RECT_F cr = RF(gx + cs * j + 1.f, gy + cs * i + 1.f,
                                gx + cs * (j + 1) - 1.f, gy + cs * (i + 1) - 1.f);
            p.FillRound(cr, 5.f, bg);
            bool hovered = (hoverI && hoverJ && *hoverI == i && *hoverJ == j);
            if (hovered) p.StrokeRound(cr, 5.f, th.accent, 2.f);

            wchar_t buf[32];
            if (showPercent) swprintf_s(buf, L"%.0f%%", v * 100.0);
            else             swprintf_s(buf, L"%.2f", v);
            float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
            D2D1_COLOR_F fg = lum < 0.55f ? ColHex(0xFFFFFF) : th.label;
            if (cs >= 30.f)
                p.Text(buf, cr, 10.f, TWeight::Medium, fg, TAlign::Center, TVAlign::Center);
        }
    }
    cellLeft_ = gx; cellTop_ = gy; cellSize_ = cs;
    return gy + cs * n + 8.f;
}

float HabitView::Draw(Painter& p, const D2D1_RECT_F& rect, float scrollY) {
    lastRect_ = rect;
    lastScrollY_ = scrollY;
    const Theme& th = p.Th();
    if (empty_) {
        p.TextWrap(L"数据不足，无法分析操作习惯。累积至少 8 个以上操作后即可查看转移概率与长程关联。",
                   Inset(rect, 20.f, 0.f), 15.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Center);
        return 0.f;
    }
    p.PushClip(rect);
    float y = rect.top - scrollY + 10.f;
    switch (panel_) {
    case Panel::Transition:  y = DrawTransition(p, rect, y);  break;
    case Panel::LongRange:   y = DrawLongRange(p, rect, y);   break;
    case Panel::SecondOrder: y = DrawSecondOrder(p, rect, y); break;
    case Panel::Summary:     y = DrawSummary(p, rect, y);     break;
    default: break;
    }
    (void)y;

    if (tip_.show) {
        float maxw = 0;
        for (size_t i = 0; i < tip_.lines.size(); ++i)
            maxw = std::max(maxw, p.MeasureW(tip_.lines[i], i == 0 ? 12.f : 11.f, TWeight::Regular));
        float w = maxw + 20.f;
        float h = (float)tip_.lines.size() * 17.f + 12.f;
        float tx = tip_.x + 14.f, ty = tip_.y + 14.f;
        if (tx + w > rect.right) tx = tip_.x - w - 10.f;
        if (ty + h > rect.bottom) ty = rect.bottom - h - 4.f;
        D2D1_RECT_F tr = RF(std::max(rect.left, tx), std::max(rect.top, ty),
                            std::max(rect.left, tx) + w, std::max(rect.top, ty) + h);
        p.FillRound(tr, 8.f, th.dark ? Col(44, 44, 46, 0.97f) : Col(28, 28, 30, 0.94f));
        p.StrokeRound(tr, 8.f, th.separator, 1.f);
        float ly = tr.top + 7.f;
        for (size_t i = 0; i < tip_.lines.size(); ++i) {
            p.Text(tip_.lines[i], RF(tr.left + 10.f, ly, tr.right - 10.f, ly + 16.f),
                   i == 0 ? 12.f : 11.f, i == 0 ? TWeight::Semibold : TWeight::Regular,
                   ColHex(0xFFFFFF, i == 0 ? 1.f : 0.78f), TAlign::Left, TVAlign::Center);
            ly += 17.f;
        }
    }
    p.PopClip();
    return ContentHeight(RW(rect));
}

float HabitView::DrawTransition(Painter& p, const D2D1_RECT_F& r, float y) {
    const Theme& th = p.Th();
    std::vector<std::wstring> labels;
    for (int i = 0; i < (int)State::COUNT; ++i) labels.push_back(StateLabel((State)i));

    p.Text(L"一阶转移概率  P(下一状态 | 当前状态)", RF(r.left, y, r.right, y + 22.f),
           15.f, TWeight::Semibold, th.label, TAlign::Left, TVAlign::Center);
    y += 26.f;
    p.TextWrap(L"读法：行是当前状态，列是紧接着发生的下一个状态。数值为条件概率，颜色深浅表示相对强度。",
               RF(r.left, y, r.right, y + 32.f), 12.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Top);
    y += 36.f;

    MatrixCtx ctx; ctx.a = &data_; ctx.lag = 1;
    y = DrawMatrix(p, r.left, y, std::min(RW(r), 520.f), (int)State::COUNT, labels,
                   L"当前状态", L"下一状态", CellTransProb, &ctx, true, &hoverI_, &hoverJ_);

    y += 8.f;
    long long tot = 0;
    for (int i = 0; i < (int)State::COUNT; ++i) tot += data_.stateTotal[i];
    if (tot > 0) {
        p.Text(L"状态占比", RF(r.left, y, r.right, y + 20.f), 13.f, TWeight::Medium, th.label,
               TAlign::Left, TVAlign::Center);
        y += 24.f;
        float bw = std::min(RW(r), 520.f);
        float x = r.left;
        for (int i = 0; i < (int)State::COUNT; ++i) {
            double frac = (double)data_.stateTotal[i] / (double)tot;
            float w = (float)(frac * (double)bw);
            if (w > 0.5f) {
                p.FillRect(RF(x, y, x + w, y + 18.f), StateColor(th, (State)i));
                if (w > 44.f)
                    p.Text(FormatPercent(frac, 0), RF(x, y, x + w, y + 18.f), 10.f,
                           TWeight::Medium, ColHex(0xFFFFFF), TAlign::Center, TVAlign::Center);
            }
            x += w;
        }
        y += 24.f;
        x = r.left;
        for (int i = 0; i < (int)State::COUNT; ++i) {
            double frac = (double)data_.stateTotal[i] / (double)tot;
            float w = (float)(frac * (double)bw);
            if (w > 40.f) {
                wchar_t b[64];
                // 必须用宽字符版本：StateNameCn 返回 const char*，
                // 直接塞给 L"%s" 是未定义行为（实际会打印乱码）。
                swprintf_s(b, L"%s %lld", StateLabel((State)i).c_str(), data_.stateTotal[i]);
                p.Text(b, RF(x, y, x + w, y + 16.f), 10.f, TWeight::Regular, th.label2,
                       TAlign::Left, TVAlign::Center);
            }
            x += w;
        }
        y += 22.f;
    }
    return y;
}

float HabitView::DrawLongRange(Painter& p, const D2D1_RECT_F& r, float y) {
    const Theme& th = p.Th();
    std::vector<std::wstring> labels;
    for (int i = 0; i < (int)State::COUNT; ++i) labels.push_back(StateLabel((State)i));

    wchar_t title[128];
    swprintf_s(title, L"长程关联  P(第 %d 步后的状态 | 当前状态)", lag_);
    p.Text(title, RF(r.left, y, r.right, y + 22.f), 15.f, TWeight::Semibold, th.label,
           TAlign::Left, TVAlign::Center);
    y += 26.f;
    p.TextWrap(L"跨越多步之后的关联，用来发现「隔几步才显现」的行为规律。切换提升度可以看到相对基线的显著性。",
               RF(r.left, y, r.right, y + 32.f), 12.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Top);
    y += 36.f;

    MatrixCtx ctx; ctx.a = &data_; ctx.lag = lag_; ctx.useLift = false;
    y = DrawMatrix(p, r.left, y, std::min(RW(r), 520.f), (int)State::COUNT, labels,
                   L"当前状态", L"滞后状态", CellTransProb, &ctx, true, &hoverI_, &hoverJ_);
    y += 6.f;

    ctx.useLift = true;
    p.Text(L"提升度（条件概率 ÷ 边缘概率，>1 表示正相关）", RF(r.left, y, r.right, y + 20.f),
           13.f, TWeight::Medium, th.label, TAlign::Left, TVAlign::Center);
    y += 24.f;
    y = DrawMatrix(p, r.left, y, std::min(RW(r), 520.f), (int)State::COUNT, labels,
                   L"当前状态", L"滞后状态", CellTransProb, &ctx, false, &hoverI_, &hoverJ_);
    y += 8.f;

    if (!data_.notables.empty()) {
        p.Text(L"检测到的显著长程关联", RF(r.left, y, r.right, y + 20.f), 13.f, TWeight::Semibold,
               th.label, TAlign::Left, TVAlign::Center);
        y += 24.f;
        for (const auto& nl : data_.notables) {
            wchar_t b[256];
            swprintf_s(b, L"滞后 %d 步：%s → %s，条件概率 %.1f%%（基线 %.1f%%，提升 %.2f 倍，样本 %lld）",
                       nl.lag, U2W(StateNameCn(nl.from)).c_str(), U2W(StateNameCn(nl.to)).c_str(),
                       nl.prob * 100.0, nl.baseline * 100.0, nl.lift, (long long)nl.n);
            D2D1_RECT_F rr = RF(r.left, y, r.left + std::min(RW(r), 620.f), y + 30.f);
            p.FillRound(rr, 8.f, th.cardAlt);
            p.Text(b, Inset(rr, 12.f, 0.f), 12.f, TWeight::Regular, th.label, TAlign::Left, TVAlign::Center, true);
            y += 34.f;
        }
    } else {
        p.TextWrap(L"尚未检测到显著长程关联（要求提升度 ≥1.5 且样本 ≥20）。继续积累数据后会自动出现。",
                   RF(r.left, y, r.left + std::min(RW(r), 620.f), y + 34.f), 12.f, TWeight::Regular,
                   th.label3, TAlign::Left, TVAlign::Top);
        y += 38.f;
    }
    return y;
}

float HabitView::DrawSecondOrder(Painter& p, const D2D1_RECT_F& r, float y) {
    const Theme& th = p.Th();
    std::vector<std::wstring> labels;
    for (int i = 0; i < (int)State::COUNT; ++i) labels.push_back(StateLabel((State)i));

    p.Text(L"二阶条件转移  P(第三状态 | 前两个状态)", RF(r.left, y, r.right, y + 22.f),
           15.f, TWeight::Semibold, th.label, TAlign::Left, TVAlign::Center);
    y += 26.f;
    wchar_t b[128];
    swprintf_s(b, L"当前固定第二个状态：%s（行 = 第一个状态）", U2W(StateNameCn(preB_)).c_str());
    p.Text(b, RF(r.left, y, r.right, y + 20.f), 12.f, TWeight::Regular, th.accent,
           TAlign::Left, TVAlign::Center);
    y += 24.f;
    p.TextWrap(L"一阶转移只能看「下一步」，二阶能看出「连续两件事之后才会出现什么」，用于捕捉更深的行为链。",
               RF(r.left, y, r.right, y + 32.f), 12.f, TWeight::Regular, th.label2, TAlign::Left, TVAlign::Top);
    y += 36.f;

    Analysis tmp;
    for (int i = 0; i < (int)State::COUNT; ++i)
        for (int j = 0; j < (int)State::COUNT; ++j)
            tmp.trans[i][j] = data_.lag2Prob[(size_t)i][(size_t)preB_][(size_t)j];
    MatrixCtx ctx; ctx.a = &tmp; ctx.lag = 1;
    y = DrawMatrix(p, r.left, y, std::min(RW(r), 520.f), (int)State::COUNT, labels,
                   L"前缀状态①", L"第三状态", CellTransProb, &ctx, true, &hoverI_, &hoverJ_);
    y += 10.f;

    p.TextWrap(L"说明：行表示序列中的第一个状态，列表示第三个状态；第二个状态已在上方固定。切换上方选择器可比较不同组合。",
               RF(r.left, y, r.left + std::min(RW(r), 620.f), y + 36.f), 11.f, TWeight::Regular,
               th.label3, TAlign::Left, TVAlign::Top);
    y += 40.f;
    return y;
}

float HabitView::DrawSummary(Painter& p, const D2D1_RECT_F& r, float y) {
    const Theme& th = p.Th();
    const Analysis& a = data_;

    p.Text(L"习惯概览", RF(r.left, y, r.right, y + 22.f), 15.f, TWeight::Semibold, th.label,
           TAlign::Left, TVAlign::Center);
    y += 28.f;

    float tileW = (std::min(RW(r), 900.f) - 24.f) / 3.f;
    auto tile = [&](int col, const std::wstring& t, const std::wstring& v,
                    const std::wstring& s, const D2D1_COLOR_F& ac) {
        D2D1_RECT_F tr = RF(r.left + (float)col * (tileW + 12.f), y, r.left + (float)col * (tileW + 12.f) + tileW, y + 96.f);
        p.StatTile(tr, t, v, s, ac);
    };
    tile(0, L"作画总时长", FormatDurationUs(a.activePaintUs), L"仅统计作画块内", th.cDeep);
    tile(1, L"操作总数", FormatNumber((double)a.ops.size(), 0), L"含键盘、鼠标、数位笔", th.accent);
    tile(2, L"撤回次数", FormatNumber((double)a.undoTotal, 0),
         a.ops.empty() ? L"暂无数据" : (L"占比 " + FormatPercent((double)a.undoTotal / (double)a.ops.size(), 1)),
         th.cUndo);
    y += 108.f;
    tile(0, L"画笔笔画", FormatNumber((double)a.brushTotal, 0),
         a.ops.empty() ? L"" : (L"占比 " + FormatPercent((double)a.brushTotal / (double)a.ops.size(), 1)), th.cBrush);
    tile(1, L"作画块", FormatNumber((double)a.blockCount, 0),
         L"深度 " + FormatNumber((double)a.deepBlocks, 0) + L" · 浅度 " + FormatNumber((double)a.shallowBlocks, 0),
         th.cShallow);
    tile(2, L"打断次数", FormatNumber((double)a.interruptCount, 0),
         L"累计 " + FormatDurationUs(a.interruptUs), th.cInterrupt);
    y += 108.f;

    // 卡片高度按正文行数算：可用正文高度 = 卡片高 - 上下内边距(24) - 标题(22)。
    // 正文最多 5 行（4 行固定文案 + 推导方法可能换行成 2 行），11px 字体行高约 15px
    // → 需要 75px，故卡片高 = 75 + 46 = 121，取 124 留余量。
    // 此前为 92px（只够 3 行），新增「活动合并窗口」一行后需加高，否则文字溢出。
    const float kThCardH = 124.f;
    p.Card(RF(r.left, y, r.left + std::min(RW(r), 900.f), y + kThCardH), 10.f);
    D2D1_RECT_F cr = Inset(RF(r.left, y, r.left + std::min(RW(r), 900.f), y + kThCardH), 14.f, 12.f);
    p.Text(L"分类阈值（由你的操作习惯自动统计得出）", RF(cr.left, cr.top, cr.right, cr.top + 18.f),
           13.f, TWeight::Semibold, th.label, TAlign::Left, TVAlign::Center);
    wchar_t b[512];
    swprintf_s(b, L"正常作画 < %.0f 秒  ≤  暂停绘画 < %.0f 秒  ≤  打断绘画\n活动合并窗口 %.2f 秒（与暂停判定独立）\n深度绘画：块内典型间隔 ≤ %.2f 秒（越小越密集）\n推导方法：%s",
               a.th.n1Sec, a.th.n2Sec, a.th.mergeSec, a.th.depthGapSec,
               U2W(a.th.method).c_str());
    p.TextWrap(std::wstring(b), RF(cr.left, cr.top + 22.f, cr.right, cr.bottom), 11.f, TWeight::Regular,
               th.label2, TAlign::Left, TVAlign::Top);
    y += kThCardH + 12.f;

    p.Text(L"最常发生的状态转移（Top 5）", RF(r.left, y, r.right, y + 20.f), 13.f, TWeight::Semibold,
           th.label, TAlign::Left, TVAlign::Center);
    y += 24.f;
    struct Pair { int i, j; double v; long long n; };
    std::vector<Pair> pairs;
    for (int i = 0; i < (int)State::COUNT; ++i)
        for (int j = 0; j < (int)State::COUNT; ++j)
            if (a.transCount[i][j] > 0) pairs.push_back({ i, j, a.trans[i][j], a.transCount[i][j] });
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.v > y.v; });
    if (pairs.size() > 5) pairs.resize(5);
    for (const auto& pr : pairs) {
        wchar_t bb[192];
        swprintf_s(bb, L"%s  →  %s      %.1f%%      %lld 次",
                   U2W(StateNameCn((State)pr.i)).c_str(), U2W(StateNameCn((State)pr.j)).c_str(),
                   pr.v * 100.0, (long long)pr.n);
        D2D1_RECT_F rr = RF(r.left, y, r.left + std::min(RW(r), 620.f), y + 28.f);
        p.FillRound(rr, 8.f, th.cardAlt);
        p.Circle(rr.left + 14.f, rr.top + 14.f, 4.f, StateColor(th, (State)pr.j));
        p.Text(bb, RF(rr.left + 26.f, rr.top, rr.right - 10.f, rr.bottom), 12.f, TWeight::Regular,
               th.label, TAlign::Left, TVAlign::Center, true);
        y += 32.f;
    }
    return y;
}

ChartHit HabitView::OnMove(float x, float y, const D2D1_RECT_F& rect) {
    tip_.show = false;
    tip_.x = x; tip_.y = y;
    tip_.lines.clear();
    if (empty_ || cellSize_ <= 0) return ChartHit::None;

    int n = (int)State::COUNT;
    int j = (int)floor((x - cellLeft_) / cellSize_);
    int i = (int)floor((y - cellTop_) / cellSize_);
    bool inside = (i >= 0 && i < n && j >= 0 && j < n);
    hoverI_ = inside ? i : -1;
    hoverJ_ = inside ? j : -1;
    if (!inside) return ChartHit::Hover;

    State from = (State)i, to = (State)j;
    double v = 0;
    long long cnt = 0;
    if (panel_ == Panel::SecondOrder) {
        v = data_.lag2Prob[(size_t)i][(size_t)preB_][(size_t)j];
        cnt = data_.lag2Count[(size_t)i][(size_t)preB_][(size_t)j];
        tip_.lines.push_back(U2W(StateNameCn(from)) + L" → " +
                             U2W(StateNameCn(preB_)) + L" → " + U2W(StateNameCn(to)));
    } else {
        int lag = (panel_ == Panel::LongRange) ? lag_ : 1;
        v = (lag <= 1) ? data_.trans[i][j] : data_.lagProb[lag - 1][i][j];
        cnt = (lag <= 1) ? data_.transCount[i][j] : data_.lagCount[lag - 1][i][j];
        tip_.lines.push_back(U2W(StateNameCn(from)) + L" → " + U2W(StateNameCn(to)));
        tip_.lines.push_back(L"滞后步数 " + std::to_wstring(lag));
    }
    wchar_t b[128];
    swprintf_s(b, L"条件概率 %.1f%%", v * 100.0);
    tip_.lines.push_back(b);
    swprintf_s(b, L"样本 %lld", cnt);
    tip_.lines.push_back(b);
    tip_.show = true;
    return ChartHit::Hover;
}

void HabitView::OnLeave() {
    hoverI_ = hoverJ_ = -1;
    tip_.show = false;
}

} // namespace pb
