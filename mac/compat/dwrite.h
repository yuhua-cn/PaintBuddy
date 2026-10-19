#pragma once
// ============================================================
//  DirectWrite 类型垫片 —— macOS 端
//
//  Ui.h:268-270 的私有方法 GetFormat / MakeLayout 返回
//  IDWriteTextFormat* / IDWriteTextLayout*，Ui.h:273/292 的
//  dwrite_ 与 fmts_ 成员也用到这些类型。macOS 端由 Core Text 实现，
//  本头文件只负责让声明原样编译通过。
//
//  ⚠️ FmtKey（Ui.h:288-291）以 int 存储对齐方式，说明 Ui.cpp 内部把
//     TAlign/TWeight 转成了 int 做缓存键。本垫片保留同名枚举常量，
//     但不假定其数值与真实 SDK 一致——macOS 端 Ui_mac.mm 自行定义映射。
// ============================================================

#include <cstdint>

// ------------------------------------------------------------
//  枚举：文字对齐与字重
// ------------------------------------------------------------
typedef enum DWRITE_TEXT_ALIGNMENT {
    DWRITE_TEXT_ALIGNMENT_LEADING    = 0,
    DWRITE_TEXT_ALIGNMENT_CENTER     = 2,
    DWRITE_TEXT_ALIGNMENT_TRAILING   = 3,
    DWRITE_TEXT_ALIGNMENT_JUSTIFIED  = 4,
} DWRITE_TEXT_ALIGNMENT;

typedef enum DWRITE_PARAGRAPH_ALIGNMENT {
    DWRITE_PARAGRAPH_ALIGNMENT_NEAR   = 0,
    DWRITE_PARAGRAPH_ALIGNMENT_CENTER = 1,
    DWRITE_PARAGRAPH_ALIGNMENT_FAR    = 2,
} DWRITE_PARAGRAPH_ALIGNMENT;

typedef enum DWRITE_WORD_WRAPPING {
    DWRITE_WORD_WRAPPING_WRAP       = 0,
    DWRITE_WORD_WRAPPING_NO_WRAP    = 1,
} DWRITE_WORD_WRAPPING;

typedef enum DWRITE_TRIMMING_GRANULARITY {
    DWRITE_TRIMMING_GRANULARITY_NONE      = 0,
    DWRITE_TRIMMING_GRANULARITY_CHARACTER = 1,
    DWRITE_TRIMMING_GRANULARITY_WORD      = 2,
} DWRITE_TRIMMING_GRANULARITY;

typedef struct DWRITE_TRIMMING {
    DWRITE_TRIMMING_GRANULARITY granularity;
    uint32_t delimiter;
    uint32_t delimiterCount;
} DWRITE_TRIMMING;

// 文字度量：Painter::MeasureW/MeasureH 需要它换算高度
typedef struct DWRITE_TEXT_METRICS {
    float left;
    float top;
    float width;
    float widthIncludingTrailingWhitespace;
    float height;
    float layoutWidth;
    float layoutHeight;
    uint32_t maxBidiRewritingDepth;
    uint32_t lineCount;
} DWRITE_TEXT_METRICS;

typedef enum DWRITE_FONT_WEIGHT {
    DWRITE_FONT_WEIGHT_REGULAR  = 400,
    DWRITE_FONT_WEIGHT_MEDIUM   = 500,
    DWRITE_FONT_WEIGHT_SEMI_BOLD = 600,
    DWRITE_FONT_WEIGHT_BOLD     = 700,
} DWRITE_FONT_WEIGHT;

typedef enum DWRITE_FONT_STYLE {
    DWRITE_FONT_STYLE_NORMAL = 0,
    DWRITE_FONT_STYLE_OBLIQUE = 1,
    DWRITE_FONT_STYLE_ITALIC  = 2,
} DWRITE_FONT_STYLE;

typedef enum DWRITE_FONT_STRETCH {
    DWRITE_FONT_STRETCH_NORMAL = 5,
} DWRITE_FONT_STRETCH;

// ------------------------------------------------------------
//  macOS 端实现载体（Core Text 对象以 void* 承载，避免本头文件
//  依赖 CoreText.h —— 与 d2d1.h 垫片同样的理由）
// ------------------------------------------------------------

// 文字格式：对应 Ui.h 的 fmts_ 缓存条目。
// 缓存键是 (字号量化值, 字重, 对齐)，故这里只需记住这三项 +
// macOS 端预先构造好的 CTFontRef 与段落样式，避免每帧重建。
struct IDWriteTextFormat {
    float    fontSize   = 13.f;
    int      weight     = 0;        // 对应 pb::TWeight
    int      align      = 0;        // 对应 pb::TAlign
    void*    font       = nullptr;  // CTFontRef
    void*    paragraph  = nullptr;  // CTParagraphStyleRef
};

// 文字排版结果：一次 Measure / DrawText 的产物
struct IDWriteTextLayout {
    void*   line     = nullptr;     // CTLineRef
    float   maxWidth = 0.f;
    float   width    = 0.f;         // 实际占用宽度
    float   height   = 0.f;         // 实际占用高度
    uint32_t lineCount = 1;
};

struct IDWriteFactory {
    // macOS 端 Core Text 无需工厂对象，保留类型只为让 Ui.h 的
    // dwrite_ 成员声明编译通过。
    int placeholder = 0;
};
