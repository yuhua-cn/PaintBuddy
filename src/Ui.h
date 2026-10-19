#pragma once
// ============================================================
//  UI 基础层：Direct2D + DirectWrite 封装、iOS 设计语言主题与控件
//
//  设计说明
//  ────────
//  · 全自绘，零第三方依赖。只依赖 d2d1 / dwrite（Win10+ 自带）。
//  · 字体用「Microsoft YaHei UI」：同时含拉丁与中文字形，
//    避免中英混排出现豆腐块；视觉上接近 iOS 的无衬线中性风格。
//  · 卡片不用投影（D2D1.0 无 ID2D1Effect），改用发丝级描边 +
//    分组底色，这也正是 iOS 设置页的实际做法。
// ============================================================
#include "Common.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace pb {

// ------------------------------------------------------------
//  iOS 系统色
// ------------------------------------------------------------
inline D2D1_COLOR_F Col(uint8_t r, uint8_t g, uint8_t b, float a = 1.f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}
inline D2D1_COLOR_F ColHex(uint32_t rgb, float a = 1.f) {
    return Col((uint8_t)((rgb >> 16) & 0xFF), (uint8_t)((rgb >> 8) & 0xFF),
               (uint8_t)(rgb & 0xFF), a);
}
inline D2D1_COLOR_F MixC(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
inline D2D1_RECT_F RF(float l, float t, float r, float b) {
    return D2D1::RectF(l, t, r, b);
}
inline float RW(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float RH(const D2D1_RECT_F& r) { return r.bottom - r.top; }
inline D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) {
    return RF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}
inline bool PtIn(float x, float y, const D2D1_RECT_F& r) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ------------------------------------------------------------
//  主题：语义化颜色，亮/暗两套
// ------------------------------------------------------------
struct Theme {
    bool dark = false;

    D2D1_COLOR_F bg;              // 分组底色（窗口背景）
    D2D1_COLOR_F card;            // 卡片/列表底
    D2D1_COLOR_F cardAlt;         // 次级卡片
    D2D1_COLOR_F separator;       // 分隔线
    D2D1_COLOR_F label;           // 主文字
    D2D1_COLOR_F label2;          // 次要文字
    D2D1_COLOR_F label3;          // 三级文字
    D2D1_COLOR_F accent;          // iOS 蓝
    D2D1_COLOR_F accentSoft;
    D2D1_COLOR_F track;           // 开关轨道（关）

    // 操作类型色（撤回 / 重做 / 画笔 / 普通）
    D2D1_COLOR_F cBrush;
    D2D1_COLOR_F cUndo;
    D2D1_COLOR_F cRedo;
    D2D1_COLOR_F cNormal;
    // 状态色
    D2D1_COLOR_F cDeep;           // 深度绘画
    D2D1_COLOR_F cShallow;        // 浅度绘画
    D2D1_COLOR_F cGapNormal;      // 普通间隔
    D2D1_COLOR_F cPause;          // 暂停绘画
    D2D1_COLOR_F cInterrupt;      // 打断绘画
    // 热力图两端
    D2D1_COLOR_F heatLow;
    D2D1_COLOR_F heatHigh;
    D2D1_COLOR_F heatTextOnHigh;

    static Theme Light() {
        Theme t;
        t.dark = false;
        t.bg        = ColHex(0xF2F2F7);
        t.card      = ColHex(0xFFFFFF);
        t.cardAlt   = ColHex(0xF7F7FA);
        t.separator = Col(60, 60, 67, 0.29f);
        t.label     = ColHex(0x000000);
        t.label2    = Col(60, 60, 67, 0.60f);
        t.label3    = Col(60, 60, 67, 0.30f);
        t.accent    = ColHex(0x007AFF);
        t.accentSoft= ColHex(0x007AFF, 0.12f);
        t.track     = Col(120, 120, 128, 0.16f);

        t.cBrush    = ColHex(0xFF9500);
        t.cUndo     = ColHex(0xFF3B30);
        t.cRedo     = ColHex(0xFF2D55);
        t.cNormal   = ColHex(0x007AFF);
        t.cDeep     = ColHex(0x5856D6);
        t.cShallow  = ColHex(0x5AC8FA);
        t.cGapNormal= ColHex(0xD1D1D6);
        t.cPause    = Col(142, 142, 147, 0.42f);
        t.cInterrupt= Col(72, 72, 74, 0.55f);
        t.heatLow   = ColHex(0xFFFFFF);
        t.heatHigh  = ColHex(0x007AFF);
        t.heatTextOnHigh = ColHex(0xFFFFFF);
        return t;
    }
    static Theme Dark() {
        Theme t;
        t.dark = true;
        t.bg        = ColHex(0x000000);
        t.card      = ColHex(0x1C1C1E);
        t.cardAlt   = ColHex(0x2C2C2E);
        t.separator = Col(84, 84, 88, 0.65f);
        t.label     = ColHex(0xFFFFFF);
        t.label2    = Col(235, 235, 245, 0.60f);
        t.label3    = Col(235, 235, 245, 0.30f);
        t.accent    = ColHex(0x0A84FF);
        t.accentSoft= ColHex(0x0A84FF, 0.18f);
        t.track     = Col(120, 120, 128, 0.32f);

        t.cBrush    = ColHex(0xFF9F0A);
        t.cUndo     = ColHex(0xFF453A);
        t.cRedo     = ColHex(0xFF375F);
        t.cNormal   = ColHex(0x0A84FF);
        t.cDeep     = ColHex(0x5E5CE6);
        t.cShallow  = ColHex(0x64D2FF);
        t.cGapNormal= ColHex(0x3A3A3C);
        t.cPause    = Col(142, 142, 147, 0.45f);
        t.cInterrupt= Col(174, 174, 178, 0.5f);
        t.heatLow   = ColHex(0x1C1C1E);
        t.heatHigh  = ColHex(0x0A84FF);
        t.heatTextOnHigh = ColHex(0xFFFFFF);
        return t;
    }
};

// ------------------------------------------------------------
//  文本样式
// ------------------------------------------------------------
enum class TAlign : uint8_t { Left = 0, Center, Right };
enum class TVAlign : uint8_t { Top = 0, Center, Bottom };
enum class TWeight : uint8_t { Regular = 0, Medium, Semibold, Bold };

// ------------------------------------------------------------
//  Painter：渲染目标 + 字体 + 主题 + 高层绘制
// ------------------------------------------------------------
class Painter {
public:
    Painter();
    ~Painter();

    bool Create(HWND hwnd);
    // 离屏渲染：把绘制内容渲染到 WIC 位图，可导出 PNG。
    // 用于在无显示器环境（测试/预览）下验证图表渲染结果。
    bool CreateOffscreen(UINT w, UINT h, float dpi = 96.f);
    bool SavePng(const std::string& pathUtf8);
    bool IsOffscreen() const { return offscreen_; }
    void Destroy();
    void Resize(UINT w, UINT h);
    void SetDpi(float dpi);
    bool Valid() const { return rt_ != nullptr; }
    void SetTheme(const Theme& t) { theme_ = t; }
    const Theme& Th() const { return theme_; }
    float Dpi() const { return dpi_; }
    float DpiScale() const { return dpi_ / 96.f; }
    // 逻辑尺寸（DIP）——布局与命中测试唯一可用的尺寸口径。
    //
    // width_/height_ 存的是物理像素（GetClientRect / WM_SIZE 给的就是物理像素，
    // 且 Resize、SavePng 也必须用物理像素）。而 D2D 渲染目标的绘制坐标系是 DIP，
    // 内部自动施加 dpi/96 缩放。此前 W()/H() 直接返回物理像素却被当 DIP 用于布局，
    // 等于把缩放乘了两次：100% 缩放时倍数恰为 1 看不出来，125%/150% 时右边界与
    // 下边界整体溢出客户区被 PushClip 裁掉 —— 这就是「换台机器右侧就没了」。
    // 改用 rt_->GetSize() 直接取渲染目标的真实 DIP 尺寸，语义最准且无需自行换算。
    float W() const;
    float H() const;
    // 物理像素尺寸（仅 Resize / PNG 导出等需要真实像素数的场合使用）
    UINT  PixelW() const { return width_; }
    UINT  PixelH() const { return height_; }

    void Begin(const D2D1_COLOR_F* clear = nullptr);
    void End();

    // —— 基本图元 ——
    void FillRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    void FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c);
    void StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w = 1.f);
    void StrokeRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float w = 1.f);
    void Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float w = 1.f);
    void Circle(float cx, float cy, float r, const D2D1_COLOR_F& c);
    void StrokeCircle(float cx, float cy, float r, const D2D1_COLOR_F& c, float w = 1.f);
    // 竖直渐变（用于甘特带、热力条）
    void FillRoundVGrad(const D2D1_RECT_F& r, float radius,
                        const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom);
    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

    // 透明度图层：让一组绘制整体带 alpha，用于两层表示之间的淡入淡出
    // （总览图放大后「概括色块」→「逐笔操作明细」的交叉渐变）。
    // 内部用 ID2D1Layer 栈实现，支持嵌套；End() 会兜底弹空。
    void PushOpacity(float alpha);
    void PopOpacity();

    // —— 文本 ——
    // 单行文本，超出按省略号截断
    void Text(const wchar_t* s, const D2D1_RECT_F& r, float size, TWeight w,
              const D2D1_COLOR_F& c, TAlign h = TAlign::Left, TVAlign v = TVAlign::Center,
              bool ellipsis = true);
    void Text(const std::wstring& s, const D2D1_RECT_F& r, float size, TWeight w,
              const D2D1_COLOR_F& c, TAlign h = TAlign::Left, TVAlign v = TVAlign::Center,
              bool ellipsis = true) {
        Text(s.c_str(), r, size, w, c, h, v, ellipsis);
    }
    // 多行（自动换行）
    void TextWrap(const std::wstring& s, const D2D1_RECT_F& r, float size, TWeight w,
                  const D2D1_COLOR_F& c, TAlign h = TAlign::Left, TVAlign v = TVAlign::Top);
    float MeasureW(const std::wstring& s, float size, TWeight w);
    float MeasureH(const std::wstring& s, float width, float size, TWeight w);

    // —— iOS 控件 ——
    // 卡片（发丝描边 + 卡片底色）
    void Card(const D2D1_RECT_F& r, float radius = 10.f);
    // 开关，返回同一 rect（命中测试用）
    void Switch(const D2D1_RECT_F& r, bool on);
    static D2D1_RECT_F SwitchRect(float right, float centerY, float scale = 1.f);
    // 分段控件：返回每段的 rect
    void Segmented(const D2D1_RECT_F& r, const std::vector<std::wstring>& items,
                   int selected, std::vector<D2D1_RECT_F>* outRects = nullptr);
    // 统计磁贴
    void StatTile(const D2D1_RECT_F& r, const std::wstring& title,
                  const std::wstring& value, const std::wstring& subtitle,
                  const D2D1_COLOR_F& accent);
    // 带标题的分组头
    void SectionHeader(const D2D1_RECT_F& r, const std::wstring& text);
    // 列表行（含右侧值与分隔线）
    void ListRow(const D2D1_RECT_F& r, const std::wstring& label,
                 const std::wstring& value, bool showSeparator,
                 const D2D1_COLOR_F* valueColor = nullptr);
    // 胶囊徽标
    void Badge(const D2D1_RECT_F& r, const std::wstring& text,
               const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg);
    // 按钮
    void Button(const D2D1_RECT_F& r, const std::wstring& text,
                const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg, bool pressed = false);

    // 颜色插值（热力图）
    D2D1_COLOR_F Heat(float t) const;   // t 归一化到 0..1

    // —— 位图（缩略图）——
    // 按 key 缓存已上传的位图，避免每帧重新解码/上传同一张缩略图。
    // 上传 BGRA32 像素；失败返回 false（调用方可画占位图）。
    bool BitmapPut(const std::string& key, int w, int h, const void* bgra32);
    // 按 key 取位图并绘制到 rect（等比缩放填充，居中裁剪）。
    // 不存在该 key 时返回 false。
    bool DrawBitmap(const std::string& key, const D2D1_RECT_F& rect, float radius);
    bool HasBitmap(const std::string& key) const;
    void BitmapDrop(const std::string& key);
    void BitmapClear();

private:
    IDWriteTextFormat* GetFormat(float size, TWeight w, TAlign h);
    IDWriteTextLayout* MakeLayout(const std::wstring& s, float size, TWeight w,
                                  float maxWidth, bool ellipsis, bool wrap);

    ID2D1Factory*           factory_ = nullptr;
    IDWriteFactory*         dwrite_ = nullptr;
    // 绘制统一走基类指针，窗口目标与离屏目标复用同一套绘制代码
    ID2D1RenderTarget*      rt_ = nullptr;
    ID2D1HwndRenderTarget*  hwndRt_ = nullptr;   // 仅窗口模式非空（Resize 用）
    ID2D1SolidColorBrush*   brush_ = nullptr;
    HWND                    hwnd_ = nullptr;
    UINT                    width_ = 1, height_ = 1;
    float                   dpi_ = 96.f;
    bool                    offscreen_ = false;
    Theme                   theme_;

    // 离屏：WIC 位图与工厂
    IWICImagingFactory* wicFactory_ = nullptr;
    IWICBitmap*         wicBitmap_ = nullptr;

    struct FmtKey { int sizeQ; int weight; int align; bool operator<(const FmtKey& o) const {
        if (sizeQ != o.sizeQ) return sizeQ < o.sizeQ;
        if (weight != o.weight) return weight < o.weight;
        return align < o.align; } };
    std::map<FmtKey, IDWriteTextFormat*> fmts_;

    // Direct2D 的 PushAxisAlignedClip 与 PushLayer 共用同一个状态栈，
    // 必须严格后进先出地弹回，顺序错乱会直接导致绘制异常。
    // 混用二者时无法只靠计数器保证顺序，故记录每次压入的类型。
    enum class StateKind : uint8_t { Clip, Layer };
    std::vector<StateKind> stateStack_;
    void PopStateTop();          // 按栈顶实际类型弹出
    void PopAllStates();         // End() 兜底：清空整个状态栈

    // 渐变笔刷缓存：甘特图会连续绘制大量同色渐变带，
    // 每帧重建 GradientStopCollection 代价过高，故按颜色对缓存一个
    D2D1_COLOR_F gradTop_ = {};
    D2D1_COLOR_F gradBot_ = {};
    bool         gradInit_ = false;
    ID2D1GradientStopCollection* gradStops_ = nullptr;
    ID2D1LinearGradientBrush*    gradBrush_ = nullptr;

    // 位图缓存（缩略图）：key → 已上传到渲染目标的 D2D 位图。
    // 每帧重绘都从 PNG 重新解码/上传代价过高，故缓存；渲染目标重建
    // （设备丢失）时旧位图失效，需整体丢弃重建 —— 由 Destroy() 清理。
    std::map<std::string, ID2D1Bitmap*> bitmaps_;
};

// ------------------------------------------------------------
//  数字格式化（界面通用）
// ------------------------------------------------------------
std::wstring FormatDurationUs(int64_t us);           // "3小时24分" / "12分05秒" / "8.4秒"
std::wstring FormatDurationHM(int64_t us);           // "2时05分"（时/分固定两段，分补零；不含秒）
std::wstring FormatDurationShort(int64_t us);        // "3:24" / "12:05"
std::wstring FormatGapSec(double sec);               // "0.8秒" / "3.2秒" / "5分" / "2小时"
std::wstring FormatNumber(double v, int digits = 0);
std::wstring FormatPercent(double ratio, int digits = 0);
std::wstring FormatBytes(int64_t bytes);          // "1.2 MB" / "340 KB"
std::wstring FormatTimeOfDay(int64_t us);            // "14:32"
std::wstring FormatDateCn(int64_t us);               // "10月16日"
std::wstring FormatDateTimeCn(int64_t us);           // "10月16日 14:32"

} // namespace pb
