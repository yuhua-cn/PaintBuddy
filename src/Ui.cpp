#include "Ui.h"
#include "Store.h"
#include <cstdio>

#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace pb {

// ============================================================
//  字体：优先「Microsoft YaHei UI」——同时含拉丁与中文字形，
//  避免中英混排出现豆腐块；视觉上是中性无衬线，接近 iOS 风格。
//  缺失时依次回退，保证任何 Windows 上都能出字。
// ============================================================
static const wchar_t* const kFontFamilies[] = {
    L"Microsoft YaHei UI",
    L"Microsoft YaHei",
    L"Segoe UI",
    L"SimHei",
};

static IDWriteFontFamily* PickFontFamily(IDWriteFontCollection* coll) {
    if (!coll) return nullptr;
    for (const wchar_t* fam : kFontFamilies) {
        UINT32 idx = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(coll->FindFamilyName(fam, &idx, &exists)) && exists) {
            IDWriteFontFamily* f = nullptr;
            if (SUCCEEDED(coll->GetFontFamily(idx, &f)) && f) return f;
        }
    }
    return nullptr;
}

static IDWriteFont* PickFont(IDWriteFontCollection* coll, DWRITE_FONT_WEIGHT w,
                             DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch) {
    IDWriteFontFamily* fam = PickFontFamily(coll);
    if (!fam) return nullptr;
    IDWriteFont* f = nullptr;
    fam->GetFirstMatchingFont(w, stretch, style, &f);
    fam->Release();
    return f;
}

// ============================================================
//  Painter
// ============================================================
Painter::Painter() { theme_ = Theme::Light(); }

Painter::~Painter() {
    Destroy();
    if (wicFactory_) { wicFactory_->Release(); wicFactory_ = nullptr; }
    if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
}

bool Painter::Create(HWND hwnd) {
    hwnd_ = hwnd;

    if (!factory_) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
        if (FAILED(hr)) { LogF("[ui] D2D1CreateFactory 失败 0x%08lX", (unsigned long)hr); return false; }
    }
    if (!dwrite_) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory), (IUnknown**)&dwrite_);
        if (FAILED(hr)) { LogF("[ui] DWriteCreateFactory 失败 0x%08lX", (unsigned long)hr); return false; }
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    width_  = (UINT)std::max<int>(1, rc.right - rc.left);
    height_ = (UINT)std::max<int>(1, rc.bottom - rc.top);

    // 真实 DPI：让 1px 发丝线与字号在高分屏上保持视觉一致。
    // 统一走 WindowDpi()，与 App::Run / App::OnCreate 取同一个值 ——
    // 三处各写一遍 GetDeviceCaps 时口径可能不一致，那会直接导致布局错位。
    dpi_ = (float)WindowDpi(hwnd);

    D2D1_RENDER_TARGET_PROPERTIES rtp =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                     dpi_, dpi_);
    D2D1_HWND_RENDER_TARGET_PROPERTIES htp =
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width_, height_),
                                         D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS);
    HRESULT hr = factory_->CreateHwndRenderTarget(rtp, htp, &hwndRt_);
    if (FAILED(hr) || !hwndRt_) {
        LogF("[ui] CreateHwndRenderTarget 失败 0x%08lX", (unsigned long)hr);
        hwndRt_ = nullptr; rt_ = nullptr;
        return false;
    }
    rt_ = hwndRt_;
    offscreen_ = false;
    // 文字抗锯齿：灰度比 ClearType 更接近 iOS 的观感
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (!brush_) {
        hr = rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0), &brush_);
        if (FAILED(hr)) { LogF("[ui] CreateSolidColorBrush 失败"); return false; }
    }
    return true;
}

// ------------------------------------------------------------
//  离屏渲染：WIC 位图 → D2D WIC RenderTarget
// ------------------------------------------------------------
bool Painter::CreateOffscreen(UINT w, UINT h, float dpi) {
    w = std::max<UINT>(1, w);
    h = std::max<UINT>(1, h);
    if (!factory_) {
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory_);
        if (FAILED(hr)) { LogF("[ui] D2D1CreateFactory 失败 0x%08lX", (unsigned long)hr); return false; }
    }
    if (!dwrite_) {
        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory), (IUnknown**)&dwrite_);
        if (FAILED(hr)) { LogF("[ui] DWriteCreateFactory 失败 0x%08lX", (unsigned long)hr); return false; }
    }
    if (!wicFactory_) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
            // 已初始化也算可用
        }
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wicFactory_));
        if (FAILED(hr) || !wicFactory_) {
            LogF("[ui] CoCreateInstance WIC 失败 0x%08lX", (unsigned long)hr);
            return false;
        }
    }

    // 释放旧的离屏位图
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (wicBitmap_) { wicBitmap_->Release(); wicBitmap_ = nullptr; }
    hwndRt_ = nullptr;

    HRESULT hr = wicFactory_->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapCacheOnDemand, &wicBitmap_);
    if (FAILED(hr) || !wicBitmap_) { LogF("[ui] CreateBitmap 失败 0x%08lX", (unsigned long)hr); return false; }

    D2D1_RENDER_TARGET_PROPERTIES rtp =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                     dpi, dpi);
    ID2D1RenderTarget* rt = nullptr;
    hr = factory_->CreateWicBitmapRenderTarget(wicBitmap_, rtp, &rt);
    if (FAILED(hr) || !rt) { LogF("[ui] CreateWicBitmapRenderTarget 失败 0x%08lX", (unsigned long)hr); return false; }
    rt_ = rt;
    width_ = w; height_ = h; dpi_ = dpi; offscreen_ = true;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (!brush_) {
        hr = rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0), &brush_);
        if (FAILED(hr)) { LogF("[ui] CreateSolidColorBrush(offscreen) 失败"); return false; }
    }
    return true;
}

bool Painter::SavePng(const std::string& pathUtf8) {
    if (!offscreen_ || !wicBitmap_ || !wicFactory_) return false;
    std::wstring wp = U2W(pathUtf8);

    IWICStream* stream = nullptr;
    HRESULT hr = wicFactory_->CreateStream(&stream);
    if (FAILED(hr) || !stream) return false;
    hr = stream->InitializeFromFilename(wp.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { stream->Release(); return false; }

    IWICBitmapEncoder* enc = nullptr;
    hr = wicFactory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (FAILED(hr) || !enc) { stream->Release(); return false; }
    hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { enc->Release(); stream->Release(); return false; }

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    hr = enc->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) { enc->Release(); stream->Release(); return false; }
    hr = frame->Initialize(props);
    if (props) props->Release();
    if (FAILED(hr)) { frame->Release(); enc->Release(); stream->Release(); return false; }
    frame->SetSize(width_, height_);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&fmt);
    hr = frame->WriteSource(wicBitmap_, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();

    frame->Release();
    enc->Release();
    stream->Release();
    return SUCCEEDED(hr);
}

void Painter::Destroy() {
    for (auto& kv : fmts_) if (kv.second) kv.second->Release();
    fmts_.clear();
    for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
    bitmaps_.clear();   // 位图绑定在渲染目标上，目标销毁后必须一并释放
    if (gradBrush_) { gradBrush_->Release(); gradBrush_ = nullptr; }
    if (gradStops_) { gradStops_->Release(); gradStops_ = nullptr; }
    gradInit_ = false;
    if (brush_) { brush_->Release(); brush_ = nullptr; }
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    hwndRt_ = nullptr;
    if (wicBitmap_) { wicBitmap_->Release(); wicBitmap_ = nullptr; }
    offscreen_ = false;
}

void Painter::Resize(UINT w, UINT h) {
    if (!rt_ || offscreen_) return;      // 离屏目标尺寸固定，不支持 Resize
    w = std::max<UINT>(1, w);
    h = std::max<UINT>(1, h);
    if (w == width_ && h == height_) return;
    width_ = w; height_ = h;
    if (hwndRt_) hwndRt_->Resize(D2D1::SizeU(w, h));
}

void Painter::SetDpi(float dpi) {
    if (!rt_ || dpi <= 0.f) return;
    dpi_ = dpi;
    rt_->SetDpi(dpi, dpi);
}

// 逻辑尺寸（DIP）。优先问渲染目标本身：它的 pixelSize 是物理像素、
// 绘制坐标系是 DIP，GetSize() 给出的正是二者按 dpi/96 换算后的结果，
// 比自行乘除更可靠（SetDpi 后 pixelSize 不变而 DIP 尺寸会变，自算容易漏）。
// 渲染目标尚未建立时退回物理像素，保证调用方拿到的永远是正数。
float Painter::W() const {
    if (!rt_) return (float)width_;
    return rt_->GetSize().width;
}

float Painter::H() const {
    if (!rt_) return (float)height_;
    return rt_->GetSize().height;
}

void Painter::Begin(const D2D1_COLOR_F* clear) {
    if (!rt_) return;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(clear ? *clear : theme_.bg);
    stateStack_.clear();
}

void Painter::End() {
    if (!rt_) return;
    PopAllStates();
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET && !offscreen_) {
        // 仅窗口模式需要重建（显卡设备重置等）；离屏目标尺寸固定，不重建
        LogF("[ui] 渲染目标丢失，需重建");
        Destroy();
        Create(hwnd_);
    }
}

// ------------------------------------------------------------
//  图元
// ------------------------------------------------------------
void Painter::FillRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!rt_ || RW(r) <= 0 || RH(r) <= 0) return;
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_);
}

void Painter::FillRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
    if (!rt_ || RW(r) <= 0 || RH(r) <= 0) return;
    radius = std::min(radius, std::min(RW(r), RH(r)) * 0.5f);
    brush_->SetColor(c);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    rt_->FillRoundedRectangle(rr, brush_);
}

void Painter::StrokeRound(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w) {
    if (!rt_ || RW(r) <= 0 || RH(r) <= 0) return;
    radius = std::min(radius, std::min(RW(r), RH(r)) * 0.5f);
    brush_->SetColor(c);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(Inset(r, w * 0.5f, w * 0.5f), radius, radius);
    rt_->DrawRoundedRectangle(rr, brush_, w);
}

void Painter::StrokeRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float w) {
    if (!rt_ || RW(r) <= 0 || RH(r) <= 0) return;
    brush_->SetColor(c);
    rt_->DrawRectangle(Inset(r, w * 0.5f, w * 0.5f), brush_, w);
}

void Painter::Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float w) {
    if (!rt_) return;
    brush_->SetColor(c);
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_, w);
}

void Painter::Circle(float cx, float cy, float r, const D2D1_COLOR_F& c) {
    if (!rt_ || r <= 0) return;
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_);
}

void Painter::StrokeCircle(float cx, float cy, float r, const D2D1_COLOR_F& c, float w) {
    if (!rt_ || r <= 0) return;
    brush_->SetColor(c);
    D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r - w * 0.5f, r - w * 0.5f);
    rt_->DrawEllipse(e, brush_, w);
}

void Painter::FillRoundVGrad(const D2D1_RECT_F& r, float radius,
                             const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom) {
    if (!rt_ || RW(r) <= 0 || RH(r) <= 0) return;

    // 颜色变化时才重建渐变资源
    bool same = gradInit_ &&
                gradTop_.r == top.r && gradTop_.g == top.g && gradTop_.b == top.b && gradTop_.a == top.a &&
                gradBot_.r == bottom.r && gradBot_.g == bottom.g && gradBot_.b == bottom.b && gradBot_.a == bottom.a;
    if (!same) {
        if (gradBrush_) { gradBrush_->Release(); gradBrush_ = nullptr; }
        if (gradStops_) { gradStops_->Release(); gradStops_ = nullptr; }
        gradInit_ = false;
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.f, top }, { 1.f, bottom },
        };
        if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2,
                    D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gradStops_))) {
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp =
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.f, 0.f), D2D1::Point2F(0.f, 1.f));
            if (SUCCEEDED(rt_->CreateLinearGradientBrush(lp, gradStops_, &gradBrush_))) {
                gradTop_ = top; gradBot_ = bottom; gradInit_ = true;
            }
        }
    }

    radius = std::min(radius, std::min(RW(r), RH(r)) * 0.5f);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    if (gradBrush_) {
        // 渐变笔刷用单位矩形定义，按目标矩形缩放
        float h = RH(r);
        if (h > 0) gradBrush_->SetTransform(D2D1::Matrix3x2F::Scale(1.f, h) *
                                            D2D1::Matrix3x2F::Translation(r.left, r.top));
        rt_->FillRoundedRectangle(rr, gradBrush_);
    } else {
        brush_->SetColor(MixC(top, bottom, 0.5f));
        rt_->FillRoundedRectangle(rr, brush_);
    }
}

void Painter::PushClip(const D2D1_RECT_F& r) {
    if (!rt_) return;
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    stateStack_.push_back(StateKind::Clip);
}
void Painter::PopClip() {
    PopStateTop();
}

// 透明度图层：让后续一组绘制整体带 alpha（用于两层表示间的淡入淡出）。
// 注意：PushLayer 与 PushAxisAlignedClip 共用同一个 D2D 状态栈，
// 因此弹出必须严格按压入顺序逆向进行 —— 由 stateStack_ 记录类型来保证。
void Painter::PushOpacity(float alpha) {
    if (!rt_) return;
    if (!(alpha >= 0.f)) alpha = 0.f;
    if (alpha > 1.f) alpha = 1.f;
    // alpha 为 0 时整层不可见：仍压入一个空图层以保持 Push/Pop 配对。
    // 注：带 opacity 的图层内不支持 ClearType，文字会自动退回灰度抗锯齿，
    // 这正是淡入淡出期间想要的效果（半透明时 ClearType 会显得发虚）。
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), alpha,
                                         nullptr, D2D1_LAYER_OPTIONS_NONE),
                   nullptr);
    stateStack_.push_back(StateKind::Layer);
}
void Painter::PopOpacity() {
    PopStateTop();
}

void Painter::PopStateTop() {
    if (!rt_ || stateStack_.empty()) return;
    StateKind k = stateStack_.back();
    stateStack_.pop_back();
    if (k == StateKind::Clip) rt_->PopAxisAlignedClip();
    else                      rt_->PopLayer();
}

void Painter::PopAllStates() {
    while (!stateStack_.empty()) PopStateTop();
}

D2D1_COLOR_F Painter::Heat(float t) const {
    if (!(t >= 0.f)) t = 0.f;
    if (t > 1.f) t = 1.f;
    return MixC(theme_.heatLow, theme_.heatHigh, t);
}

// ------------------------------------------------------------
//  位图（缩略图）
//
//  缓存策略：位图必须从属于当前渲染目标，因此
//    · CreateOffscreen 切换目标时旧位图全部失效 → 一并释放
//    · End() 检测到 D2DERR_RECREATE_TARGET 时走 Destroy() → 同样释放
//  调用方（界面）只按 key 取用，key 不存在时重新上传即可自愈。
// ------------------------------------------------------------
bool Painter::HasBitmap(const std::string& key) const {
    auto it = bitmaps_.find(key);
    return it != bitmaps_.end() && it->second != nullptr;
}

void Painter::BitmapDrop(const std::string& key) {
    auto it = bitmaps_.find(key);
    if (it == bitmaps_.end()) return;
    if (it->second) it->second->Release();
    bitmaps_.erase(it);
}

void Painter::BitmapClear() {
    for (auto& kv : bitmaps_) if (kv.second) kv.second->Release();
    bitmaps_.clear();
}

bool Painter::BitmapPut(const std::string& key, int w, int h, const void* bgra32) {
    if (!rt_ || !bgra32 || w <= 0 || h <= 0) return false;
    if (HasBitmap(key)) return true;   // 已缓存，直接复用

    D2D1_BITMAP_PROPERTIES bp = {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // 缩略图不透明，alpha 统一给 1（PNG 解码出的 alpha 可能是 0）
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    bp.dpiX = dpi_;
    bp.dpiY = dpi_;

    ID2D1Bitmap* bmp = nullptr;
    HRESULT hr = rt_->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), bgra32,
                                   (UINT32)(w * 4), bp, &bmp);
    if (FAILED(hr) || !bmp) {
        LogF("[ui] CreateBitmap 失败 0x%08lX", (unsigned long)hr);
        return false;
    }
    bitmaps_[key] = bmp;
    return true;
}

bool Painter::DrawBitmap(const std::string& key, const D2D1_RECT_F& rect, float radius) {
    if (!rt_ || RW(rect) <= 0 || RH(rect) <= 0) return false;
    auto it = bitmaps_.find(key);
    if (it == bitmaps_.end() || !it->second) return false;
    ID2D1Bitmap* bmp = it->second;

    D2D1_SIZE_F sz = bmp->GetSize();
    if (sz.width <= 0.f || sz.height <= 0.f) return false;

    // 等比「填满并居中裁剪」（cover）：短边贴合、长边溢出裁掉，
    // 比 letterbox 更适合缩略图，不会出现黑边。
    float s = std::max(RW(rect) / sz.width, RH(rect) / sz.height);
    float cropW = RW(rect) / s;
    float cropH = RH(rect) / s;
    D2D1_RECT_F src = D2D1::RectF((sz.width - cropW) * 0.5f, (sz.height - cropH) * 0.5f,
                                  (sz.width + cropW) * 0.5f, (sz.height + cropH) * 0.5f);
    // 浮点误差可能让裁剪框略微越界，夹回源图范围内
    if (src.left < 0.f) src.left = 0.f;
    if (src.top < 0.f) src.top = 0.f;
    if (src.right > sz.width) src.right = sz.width;
    if (src.bottom > sz.height) src.bottom = sz.height;

    if (radius > 0.f && factory_) {
        // 圆角：先裁到圆角矩形再画。
        // 注意几何体由 ID2D1Factory 创建（不是渲染目标的方法）。
        ID2D1RoundedRectangleGeometry* geo = nullptr;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, radius, radius);
        if (SUCCEEDED(factory_->CreateRoundedRectangleGeometry(&rr, &geo)) && geo) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(), geo,
                                                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                            D2D1::IdentityMatrix(), 1.f,
                                                            nullptr, D2D1_LAYER_OPTIONS_NONE);
            rt_->PushLayer(lp, nullptr);
            stateStack_.push_back(StateKind::Layer);
            rt_->DrawBitmap(bmp, rect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
            PopStateTop();
            geo->Release();
            return true;
        }
        if (geo) geo->Release();
    }

    rt_->DrawBitmap(bmp, rect, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    return true;
}

// ------------------------------------------------------------
//  文本
// ------------------------------------------------------------
IDWriteTextFormat* Painter::GetFormat(float size, TWeight w, TAlign h) {
    if (!dwrite_) return nullptr;
    size = std::max(4.f, size);
    FmtKey key{ (int)(size * 10.f + 0.5f), (int)w, (int)h };
    auto it = fmts_.find(key);
    if (it != fmts_.end()) return it->second;

    DWRITE_FONT_WEIGHT dw = DWRITE_FONT_WEIGHT_REGULAR;
    switch (w) {
    case TWeight::Medium:   dw = DWRITE_FONT_WEIGHT_MEDIUM; break;
    case TWeight::Semibold: dw = DWRITE_FONT_WEIGHT_SEMI_BOLD; break;
    case TWeight::Bold:     dw = DWRITE_FONT_WEIGHT_BOLD; break;
    default: break;
    }
    DWRITE_TEXT_ALIGNMENT da = DWRITE_TEXT_ALIGNMENT_LEADING;
    switch (h) {
    case TAlign::Center: da = DWRITE_TEXT_ALIGNMENT_CENTER; break;
    case TAlign::Right:  da = DWRITE_TEXT_ALIGNMENT_TRAILING; break;
    default: break;
    }

    IDWriteTextFormat* fmt = nullptr;
    // 逐个候选族名尝试，命中即止；全失败则退回系统默认字体
    for (const wchar_t* fam : kFontFamilies) {
        HRESULT hr = dwrite_->CreateTextFormat(
            fam, nullptr, dw, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &fmt);
        if (SUCCEEDED(hr) && fmt) break;
        fmt = nullptr;
    }
    if (!fmt) {
        HRESULT hr = dwrite_->CreateTextFormat(
            L"", nullptr, dw, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &fmt);
        if (FAILED(hr) || !fmt) return nullptr;
    }
    fmt->SetTextAlignment(da);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmts_[key] = fmt;
    return fmt;
}

IDWriteTextLayout* Painter::MakeLayout(const std::wstring& s, float size, TWeight w,
                                       float maxWidth, bool ellipsis, bool wrap) {
    if (!dwrite_ || s.empty()) return nullptr;
    IDWriteTextFormat* fmt = GetFormat(size, w, TAlign::Left);
    if (!fmt) return nullptr;
    IDWriteTextLayout* lay = nullptr;
    HRESULT hr = dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                           std::max(1.f, maxWidth),
                                           std::max(1.f, 1e6f), &lay);
    if (FAILED(hr) || !lay) return nullptr;
    lay->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis && !wrap) {
        // 超出宽度时用省略号截断；创建省略号标记失败则退化为直接裁剪
        DWRITE_TRIMMING trim{};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        IDWriteInlineObject* dots = nullptr;
        dwrite_->CreateEllipsisTrimmingSign(fmt, &dots);
        lay->SetTrimming(&trim, dots);
        if (dots) dots->Release();
    }
    return lay;
}

void Painter::Text(const wchar_t* s, const D2D1_RECT_F& r, float size, TWeight w,
                   const D2D1_COLOR_F& c, TAlign h, TVAlign v, bool ellipsis) {
    if (!rt_ || !s || !*s || RW(r) <= 0 || RH(r) <= 0) return;
    std::wstring str(s);
    IDWriteTextLayout* lay = MakeLayout(str, size, w, RW(r), ellipsis, false);
    if (!lay) return;

    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    float x = r.left;
    float y = r.top;
    switch (h) {
    case TAlign::Center: x = r.left + (RW(r) - m.widthIncludingTrailingWhitespace) * 0.5f; break;
    case TAlign::Right:  x = r.right - m.widthIncludingTrailingWhitespace; break;
    default: break;
    }
    switch (v) {
    case TVAlign::Center: y = r.top + (RH(r) - m.height) * 0.5f; break;
    case TVAlign::Bottom: y = r.bottom - m.height; break;
    default: break;
    }
    brush_->SetColor(c);
    rt_->DrawTextLayout(D2D1::Point2F(x, y), lay, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    lay->Release();
}

void Painter::TextWrap(const std::wstring& s, const D2D1_RECT_F& r, float size, TWeight w,
                       const D2D1_COLOR_F& c, TAlign h, TVAlign v) {
    if (!rt_ || s.empty() || RW(r) <= 0 || RH(r) <= 0) return;
    IDWriteTextLayout* lay = MakeLayout(s, size, w, RW(r), false, true);
    if (!lay) return;

    DWRITE_TEXT_ALIGNMENT da = DWRITE_TEXT_ALIGNMENT_LEADING;
    if (h == TAlign::Center) da = DWRITE_TEXT_ALIGNMENT_CENTER;
    else if (h == TAlign::Right) da = DWRITE_TEXT_ALIGNMENT_TRAILING;
    lay->SetTextAlignment(da);

    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    float y = r.top;
    if (v == TVAlign::Center) y = r.top + std::max(0.f, (RH(r) - m.height) * 0.5f);
    else if (v == TVAlign::Bottom) y = r.top + std::max(0.f, RH(r) - m.height);

    brush_->SetColor(c);
    rt_->DrawTextLayout(D2D1::Point2F(r.left, y), lay, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    lay->Release();
}

float Painter::MeasureW(const std::wstring& s, float size, TWeight w) {
    if (s.empty()) return 0.f;
    IDWriteTextLayout* lay = MakeLayout(s, size, w, 1e6f, false, false);
    if (!lay) return 0.f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.widthIncludingTrailingWhitespace;
}

float Painter::MeasureH(const std::wstring& s, float width, float size, TWeight w) {
    if (s.empty() || width <= 0) return 0.f;
    IDWriteTextLayout* lay = MakeLayout(s, size, w, width, false, true);
    if (!lay) return 0.f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    lay->Release();
    return m.height;
}

// ------------------------------------------------------------
//  iOS 控件
// ------------------------------------------------------------
void Painter::Card(const D2D1_RECT_F& r, float radius) {
    FillRound(r, radius, theme_.card);
    StrokeRound(r, radius, theme_.separator, 1.f / DpiScale());
}

D2D1_RECT_F Painter::SwitchRect(float right, float centerY, float scale) {
    float w = 51.f * scale, h = 31.f * scale;
    return RF(right - w, centerY - h * 0.5f, right, centerY + h * 0.5f);
}

void Painter::Switch(const D2D1_RECT_F& r, bool on) {
    float w = RW(r), h = RH(r);
    if (w <= 0 || h <= 0) return;
    float radius = h * 0.5f;
    FillRound(r, radius, on ? theme_.accent : theme_.track);

    float knob = h - 4.f;
    float travel = w - knob - 4.f;
    if (travel < 0) travel = 0;
    float cx = r.left + 2.f + knob * 0.5f + (on ? travel : 0.f);
    float cy = r.top + h * 0.5f;

    // 旋钮：纯白圆 + 极淡描边模拟 iOS 的轻微投影
    Circle(cx, cy, knob * 0.5f + 0.5f, Col(0, 0, 0, 0.06f));
    Circle(cx, cy, knob * 0.5f, ColHex(0xFFFFFF));
}

void Painter::Segmented(const D2D1_RECT_F& r, const std::vector<std::wstring>& items,
                        int selected, std::vector<D2D1_RECT_F>* outRects) {
    if (outRects) outRects->clear();
    size_t n = items.size();
    if (n == 0 || RW(r) <= 0 || RH(r) <= 0) return;

    float radius = std::min(8.f, RH(r) * 0.5f);
    FillRound(r, radius, theme_.track);

    float pad = 2.f;
    float innerW = RW(r) - pad * 2.f;
    float segW = innerW / (float)n;
    float innerH = RH(r) - pad * 2.f;

    for (size_t i = 0; i < n; ++i) {
        D2D1_RECT_F sr = RF(r.left + pad + segW * (float)i, r.top + pad,
                            r.left + pad + segW * (float)(i + 1), r.bottom - pad);
        if (outRects) outRects->push_back(sr);
        bool sel = ((int)i == selected);
        if (sel) {
            // 选中的段是白色（暗色模式下是深灰）浮起胶囊
            FillRound(sr, std::min(radius - 1.f, RH(sr) * 0.5f), theme_.dark ? ColHex(0x636366) : ColHex(0xFFFFFF));
        }
        Text(items[i], sr, 13.f, sel ? TWeight::Semibold : TWeight::Regular,
             sel ? theme_.label : theme_.label2, TAlign::Center, TVAlign::Center);
    }
    (void)innerH;
}

void Painter::StatTile(const D2D1_RECT_F& r, const std::wstring& title,
                       const std::wstring& value, const std::wstring& subtitle,
                       const D2D1_COLOR_F& accent) {
    Card(r, 12.f);
    const float pad = 14.f;   // DIP：布局坐标系统一后无需再参与 DPI 换算
    D2D1_RECT_F inner = Inset(r, pad, 12.f);
    Text(title, RF(inner.left, inner.top, inner.right, inner.top + 16.f),
         12.f, TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Center);
    // 数值行左侧带一根强调色竖条，iOS 卡片常见做法
    FillRound(RF(inner.left, inner.top + 22.f, inner.left + 3.f, inner.top + 48.f), 1.5f, accent);
    Text(value, RF(inner.left + 10.f, inner.top + 20.f, inner.right, inner.top + 50.f),
         24.f, TWeight::Semibold, theme_.label, TAlign::Left, TVAlign::Center);
    if (!subtitle.empty()) {
        Text(subtitle, RF(inner.left, inner.bottom - 20.f, inner.right, inner.bottom),
             11.f, TWeight::Regular, theme_.label3, TAlign::Left, TVAlign::Center);
    }
}

void Painter::SectionHeader(const D2D1_RECT_F& r, const std::wstring& text) {
    Text(text, r, 13.f, TWeight::Regular, theme_.label2, TAlign::Left, TVAlign::Bottom);
}

void Painter::ListRow(const D2D1_RECT_F& r, const std::wstring& label,
                      const std::wstring& value, bool showSeparator,
                      const D2D1_COLOR_F* valueColor) {
    float pad = 16.f;
    Text(label, RF(r.left + pad, r.top, r.left + RW(r) * 0.55f, r.bottom),
         15.f, TWeight::Regular, theme_.label, TAlign::Left, TVAlign::Center);
    if (!value.empty()) {
        Text(value, RF(r.left + RW(r) * 0.5f, r.top, r.right - pad, r.bottom),
             15.f, TWeight::Regular, valueColor ? *valueColor : theme_.label2,
             TAlign::Right, TVAlign::Center);
    }
    if (showSeparator) {
        float sy = r.bottom - 1.f / DpiScale();
        Line(r.left + pad, sy, r.right, sy, theme_.separator, 1.f / DpiScale());
    }
}

void Painter::Badge(const D2D1_RECT_F& r, const std::wstring& text,
                    const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg) {
    FillRound(r, RH(r) * 0.5f, bg);
    Text(text, Inset(r, 8.f, 0.f), 11.f, TWeight::Medium, fg, TAlign::Center, TVAlign::Center);
}

void Painter::Button(const D2D1_RECT_F& r, const std::wstring& text,
                     const D2D1_COLOR_F& bg, const D2D1_COLOR_F& fg, bool pressed) {
    FillRound(r, 10.f, pressed ? MixC(bg, Col(0, 0, 0, 1.f), 0.15f) : bg);
    Text(text, r, 15.f, TWeight::Semibold, fg, TAlign::Center, TVAlign::Center);
}

// ============================================================
//  格式化
// ============================================================
static void WAppendF(std::wstring& out, const wchar_t* fmt, ...) {
    wchar_t buf[128];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, sizeof(buf) / sizeof(wchar_t), _TRUNCATE, fmt, ap);
    va_end(ap);
    out += buf;
}

std::wstring FormatDurationUs(int64_t us) {
    if (us < 0) us = 0;
    int64_t totalSec = us / 1000000LL;
    int64_t h = totalSec / 3600;
    int64_t m = (totalSec % 3600) / 60;
    int64_t s = totalSec % 60;
    std::wstring out;
    if (h > 0) {
        WAppendF(out, L"%lld小时%lld分", (long long)h, (long long)m);
    } else if (m > 0) {
        WAppendF(out, L"%lld分%02lld秒", (long long)m, (long long)s);
    } else if (totalSec > 0) {
        int frac = (int)((us % 1000000LL) / 100000LL);
        WAppendF(out, L"%lld.%d秒", (long long)totalSec, frac);
    } else {
        WAppendF(out, L"%lld毫秒", (long long)(us / 1000));
    }
    return out;
}

// "x时xx分"：时/分固定两段，分补零，不含秒。用于总览「今日绘画」磁贴——
// 作画时长以分钟为有意义的粒度，秒数每帧跳动反而让磁贴数字晃眼。
// 与 FormatDurationUs 的自适应省略格式区分开，保证列对齐。
std::wstring FormatDurationHM(int64_t us) {
    if (us < 0) us = 0;
    int64_t totalSec = us / 1000000LL;
    int64_t h = totalSec / 3600;
    int64_t m = (totalSec % 3600) / 60;
    std::wstring out;
    WAppendF(out, L"%lld时%02lld分", (long long)h, (long long)m);
    return out;
}

std::wstring FormatDurationShort(int64_t us) {
    if (us < 0) us = 0;
    int64_t totalSec = us / 1000000LL;
    int64_t h = totalSec / 3600;
    int64_t m = (totalSec % 3600) / 60;
    int64_t s = totalSec % 60;
    std::wstring out;
    if (h > 0) WAppendF(out, L"%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else       WAppendF(out, L"%lld:%02lld", (long long)m, (long long)s);
    return out;
}

std::wstring FormatGapSec(double sec) {
    std::wstring out;
    if (sec < 1.0)      WAppendF(out, L"%.0f毫秒", sec * 1000.0);
    else if (sec < 60)  WAppendF(out, L"%.1f秒", sec);
    else if (sec < 3600)WAppendF(out, L"%.1f分", sec / 60.0);
    else                WAppendF(out, L"%.1f小时", sec / 3600.0);
    return out;
}

std::wstring FormatNumber(double v, int digits) {
    wchar_t buf[64];
    wchar_t fmt[16];
    swprintf_s(fmt, L"%%.%dlf", digits);
    swprintf_s(buf, fmt, v);
    std::wstring s(buf);
    // 千分位
    size_t dot = s.find(L'.');
    size_t intLen = (dot == std::wstring::npos) ? s.size() : dot;
    std::wstring out;
    int cnt = 0;
    bool neg = !s.empty() && s[0] == L'-';
    size_t start = neg ? 1 : 0;
    for (size_t i = intLen; i-- > start; ) {
        out.insert(out.begin(), s[i]);
        if (++cnt % 3 == 0 && i > start) out.insert(out.begin(), L',');
    }
    if (neg) out.insert(out.begin(), L'-');
    if (dot != std::wstring::npos) out += s.substr(dot);
    return out;
}

std::wstring FormatPercent(double ratio, int digits) {
    std::wstring out;
    WAppendF(out, L"%.*f%%", digits, ratio * 100.0);
    return out;
}

std::wstring FormatBytes(int64_t bytes) {
    if (bytes < 0) bytes = 0;
    wchar_t b[64];
    const double KB = 1024.0, MB = 1024.0 * 1024.0, GB = 1024.0 * 1024.0 * 1024.0;
    if (bytes >= (int64_t)GB)      swprintf_s(b, L"%.2f GB", (double)bytes / GB);
    else if (bytes >= (int64_t)MB) swprintf_s(b, L"%.1f MB", (double)bytes / MB);
    else if (bytes >= (int64_t)KB) swprintf_s(b, L"%.0f KB", (double)bytes / KB);
    else                           swprintf_s(b, L"%lld B", (long long)bytes);
    return std::wstring(b);
}

std::wstring FormatTimeOfDay(int64_t us) {
    std::string s = UsToLocalStr(us, "%H:%M");
    return U2W(s);
}

std::wstring FormatDateCn(int64_t us) {
    time_t sec = (time_t)(us / 1000000LL);
    struct tm lt;
    localtime_s(&lt, &sec);
    std::wstring out;
    WAppendF(out, L"%d月%d日", lt.tm_mon + 1, lt.tm_mday);
    return out;
}

std::wstring FormatDateTimeCn(int64_t us) {
    time_t sec = (time_t)(us / 1000000LL);
    struct tm lt;
    localtime_s(&lt, &sec);
    std::wstring out;
    WAppendF(out, L"%d月%d日 %02d:%02d", lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    return out;
}

} // namespace pb
