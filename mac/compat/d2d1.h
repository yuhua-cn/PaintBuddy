#pragma once
// ============================================================
//  Direct2D 类型垫片 —— macOS 端
//
//  Charts.h 的公开接口签名（GanttView::Draw / ZoomAt / OnWheel / …）
//  与 Ui.h 的 Painter 全部以 D2D1_RECT_F、D2D1_COLOR_F 为参数或成员，
//  且 Charts.cpp 直接访问它们的字段。故这两个结构的**字段名与顺序
//  必须与 Windows SDK 一致**，否则 Charts.cpp 无法零修改编译。
//
//  其余 ID2D1* / IDWrite* 接口类型在 Ui.h 中仅作为 Painter 的**私有成员
//  指针**出现。在 macOS 端它们不是 COM 接口，而是本端实现的结构体，
//  由 mac/platform/Ui_mac.mm 创建与填充（内部持有 Core Graphics /
//  Core Text 对象）。保留同名是为了让 Ui.h 的成员声明原样编译通过——
//  Windows 端的 Ui.cpp 在 macOS 构建中不参与编译。
// ============================================================

#include <cstdint>

// ------------------------------------------------------------
//  值类型：形状必须与 Windows SDK 完全一致
// ------------------------------------------------------------
typedef struct D2D1_COLOR_F {
    float r;
    float g;
    float b;
    float a;
} D2D1_COLOR_F;

typedef struct D2D1_RECT_F {
    float left;
    float top;
    float right;
    float bottom;
} D2D1_RECT_F;

typedef struct D2D1_SIZE_F {
    float width;
    float height;
} D2D1_SIZE_F;

typedef struct D2D1_SIZE_U {
    uint32_t width;
    uint32_t height;
} D2D1_SIZE_U;

typedef struct D2D1_POINT_2F {
    float x;
    float y;
} D2D1_POINT_2F;

typedef struct D2D1_MATRIX_4X4_F {
    float m[4][4];
} D2D1_MATRIX_4X4_F;

// Ui.h:29-44 的 Col / ColHex / MixC / RF 依赖 D2D1 命名空间下的这两个工厂。
// 真实 SDK 里 ColorF 还有一组颜色枚举重载，Ui.h 未使用，故不提供。
namespace D2D1 {

inline D2D1_COLOR_F ColorF(float r, float g, float b, float a = 1.0f) {
    D2D1_COLOR_F c;
    c.r = r; c.g = g; c.b = b; c.a = a;
    return c;
}

inline D2D1_RECT_F RectF(float left, float top, float right, float bottom) {
    D2D1_RECT_F r;
    r.left = left; r.top = top; r.right = right; r.bottom = bottom;
    return r;
}

inline D2D1_SIZE_F SizeF(float w, float h) {
    D2D1_SIZE_F s;
    s.width = w; s.height = h;
    return s;
}

inline D2D1_POINT_2F Point2F(float x, float y) {
    D2D1_POINT_2F p;
    p.x = x; p.y = y;
    return p;
}

} // namespace D2D1

// ------------------------------------------------------------
//  枚举：Ui.h / Painter 用到的少量常量
// ------------------------------------------------------------
typedef enum D2D1_ANTIALIAS_MODE {
    D2D1_ANTIALIAS_MODE_ALIASED     = 0,
    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE = 1,
} D2D1_ANTIALIAS_MODE;

typedef enum D2D1_TEXT_ANTIALIAS_MODE {
    D2D1_TEXT_ANTIALIAS_MODE_DEFAULT  = 0,
    D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE = 1,
    D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE = 2,
} D2D1_TEXT_ANTIALIAS_MODE;

typedef enum D2D1_BITMAP_INTERPOLATION_MODE {
    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR = 0,
    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR           = 1,
} D2D1_BITMAP_INTERPOLATION_MODE;

// ------------------------------------------------------------
//  macOS 端实现载体
//
//  下面这些结构体在 Windows 上是 COM 接口（只有虚函数表）。在 macOS 上
//  它们被重新定义为「持有 Core Graphics 对象的本端结构体」，字段全部用
//  void* 以避免本头文件依赖 CoreGraphics.h（保持垫片轻量，也让
//  Charts.cpp / Analyzer.cpp 编译时不必拉入图形框架头）。
//  Ui_mac.mm 里再 static_cast 回真实类型使用。
// ------------------------------------------------------------

// 渲染目标：Painter 的 rt_ 指向它，窗口模式与离屏模式共用同一类型
// （对应 Windows 端「绘制统一走 ID2D1RenderTarget 基类指针」的设计）。
struct ID2D1RenderTarget {
    void*     ctx       = nullptr;  // CGContextRef —— 实际绘制目标
    void*     layer     = nullptr;  // CALayer/CAMetalLayer（窗口模式；离屏为 nullptr）
    uint32_t  pixelW    = 1;        // 物理像素宽
    uint32_t  pixelH    = 1;        // 物理像素高
    float     dpi       = 96.f;     // 逻辑 DPI 基准（96 = 1 倍）
    bool      offscreen = false;    // 是否离屏（对应 Painter::IsOffscreen）
    float     opacity   = 1.f;      // 当前 PushOpacity 累积透明度

    // 逻辑尺寸（DIP）。对应 Ui.h:174-186 的口径约定：
    // W()/H() 必须返回 DIP 而非物理像素——Windows 端曾因把物理像素当 DIP
    // 用于布局，导致 125%/150% 缩放下右边界与下边界溢出被裁掉
    // （「换台机器右侧就没了」，2026-10-17 已修）。
    // macOS 上 Retina 的 backingScaleFactor 等价于 dpi/96，
    // 故此处同样做一次除法还原为逻辑点。
    D2D1_SIZE_F GetSize() const {
        float s = (dpi > 0.0f) ? (dpi / 96.0f) : 1.0f;
        D2D1_SIZE_F sz;
        sz.width  = (float)pixelW / s;
        sz.height = (float)pixelH / s;
        return sz;
    }
};

// 窗口渲染目标：Ui.h 的 hwndRt_ 用它，仅窗口模式非空（Resize 用）
struct ID2D1HwndRenderTarget : ID2D1RenderTarget {
    void* hwnd = nullptr;           // NSView*（Windows 端是 HWND）
};

struct ID2D1Factory {
    // macOS 端 Core Graphics 无需工厂对象，保留此类型只为让
    // Ui.h 的 factory_ 成员声明编译通过。
    int placeholder = 0;
};

struct ID2D1SolidColorBrush {
    D2D1_COLOR_F color = {0.f, 0.f, 0.f, 1.f};
};

struct ID2D1GradientStopCollection {
    D2D1_COLOR_F top    = {0.f, 0.f, 0.f, 1.f};
    D2D1_COLOR_F bottom = {0.f, 0.f, 0.f, 1.f};
};

struct ID2D1LinearGradientBrush {
    D2D1_COLOR_F top    = {0.f, 0.f, 0.f, 1.f};
    D2D1_COLOR_F bottom = {0.f, 0.f, 0.f, 1.f};
};

struct ID2D1Bitmap {
    void*    image = nullptr;       // CGImageRef —— 已解码的位图
    uint32_t w = 0, h = 0;
};

struct ID2D1Layer {
    int placeholder = 0;
};
