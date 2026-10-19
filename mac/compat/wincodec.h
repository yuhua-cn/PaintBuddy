#pragma once
// ============================================================
//  Windows Imaging Component 类型垫片 —— macOS 端
//
//  Ui.h:285-286 的私有成员 wicFactory_ / wicBitmap_ 用到这两个类型，
//  用途是 CreateOffscreen + SavePng（离屏渲染导出 PNG，供
//  ui_preview / render_preview 在无显示器环境下验证图表）。
//
//  macOS 端由 Core Graphics（CGBitmapContextCreate）+
//  ImageIO（CGImageDestination）实现，见 Ui_mac.mm。
//  本头文件只负责让 Ui.h 的成员声明原样编译通过。
// ============================================================

#include <cstdint>

// 像素格式 GUID：Ui.cpp 内部用于创建 32bpp BGRA/PBGRA 位图。
// macOS 端固定使用 kCGImageAlphaPremultipliedFirst + 8bpc（即 BGRA 预乘），
// 与 D2D 的默认格式一致，故这里只保留一个占位类型。
struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
};
typedef GUID WICPixelFormatGUID;

// 离屏位图：承载 CGBitmapContextRef 与其像素缓冲
struct IWICBitmap {
    void*     ctx    = nullptr;   // CGBitmapContextRef
    void*     data   = nullptr;   // 像素缓冲首地址（BGRA 预乘）
    uint32_t  w      = 0;
    uint32_t  h      = 0;
    uint32_t  stride = 0;         // 每行字节数
};

// 成像工厂：macOS 端 Core Graphics 无需工厂对象，
// 保留类型只为让 Ui.h 的 wicFactory_ 成员声明编译通过。
struct IWICImagingFactory {
    int placeholder = 0;
};
