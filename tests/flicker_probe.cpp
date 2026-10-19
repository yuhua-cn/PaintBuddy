// ============================================================
//  黑色条状闪烁诊断 v2
//
//  v1 结论（已排除）：行距与声明完全一致、缓冲长度无突变、无全黑帧、
//  alpha 全非 0。但 v1 只检测「整行横跨全宽都黑」，竖条/局部暗区漏检。
//
//  v2 改用「暗像素占比」逐帧分析，能抓任何形式的暗区（横条/竖条/局部块），
//  并额外检测：列方向黑带、帧间重复（闪烁常表现为交替重复帧）、
//  MEDIATYPECHANGED 出现在第几帧、以及两个源文件各自的分辨率。
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Video.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <winrt/base.h>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using winrt::com_ptr;
using namespace pb;

namespace {

struct MfScope {
    bool inited = false;
    MfScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        inited = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE) inited = false;   // 别人已初始化，不归我们注销
        MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    }
    ~MfScope() { MFShutdown(); if (inited) CoUninitialize(); }
};

std::string LeafName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return W2U(i == std::wstring::npos ? p : p.substr(i + 1));
}

// 一帧的暗区分析结果
struct FrameStat {
    long long idx = 0;
    double    darkRatio = 0;      // 暗像素占比（r,g,b 均 <= 24）
    long long darkRows = 0;       // 该行 >=60% 像素为暗 的行数
    long long darkCols = 0;       // 该列 >=60% 像素为暗 的列数
    int       rowMinY = -1, rowMaxY = -1;
    int       colMinX = -1, colMaxX = -1;
};

// 逐帧扫描，返回每帧统计；同时报告缓冲长度突变与媒体类型变化帧
void DeepScan(const std::wstring& path, const char* label) {
    printf("\n==== %s ====\n", label);
    printf("  文件: %s\n", LeafName(path).c_str());

    double sec = 0; int pw = 0, ph = 0;
    if (!ProbeVideo(path, sec, pw, ph)) { printf("  !! 探测失败\n"); return; }
    printf("  时长 %.2f 秒   分辨率 %dx%d   约 %.0f 帧\n", sec, pw, ph, sec * 30.0);

    com_ptr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 4))) { printf("  !! 属性创建失败\n"); return; }
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1);

    com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attrs.get(), reader.put())) || !reader) {
        printf("  !! 打开失败\n"); return;
    }

    const int W = pw & ~1, H = ph & ~1;
    com_ptr<IMFMediaType> outMt;
    if (FAILED(MFCreateMediaType(outMt.put()))) { printf("  !! 类型创建失败\n"); return; }
    outMt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    outMt->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)(UINT32)W << 32) | (UINT32)H);
    outMt->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)W * 4);
    outMt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 1);
    outMt->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)W * (UINT32)H * 4u);
    outMt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outMt.get()))) {
        printf("  !! SetCurrentMediaType 失败\n"); return;
    }

    std::vector<FrameStat> stats;
    std::vector<long long> typeChangedAt, lenChangedAt;
    DWORD prevLen = 0;
    long long frames = 0;
    std::vector<uint8_t> prevFrame;      // 用于检测重复帧（闪烁常见形态）
    std::vector<long long> dupFrames;
    const int stride = W * 4;

    while (frames < 6000) {
        DWORD actualStream = 0, flags = 0; LONGLONG ts = 0;
        com_ptr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                       &actualStream, &flags, &ts, sample.put());
        if (FAILED(hr)) { printf("  ReadSample 失败 0x%08lX（第 %lld 帧）\n", (unsigned long)hr, frames); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) typeChangedAt.push_back(frames);
        if (!sample) continue;

        com_ptr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.put())) || !buf) continue;
        BYTE* data = nullptr; DWORD curLen = 0, maxLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, &curLen)) || !data) { buf = nullptr; continue; }
        if (prevLen && curLen != prevLen) lenChangedAt.push_back(frames);
        prevLen = curLen;

        if ((size_t)curLen >= (size_t)stride * H) {
            FrameStat s; s.idx = frames;
            long long darkPx = 0;
            std::vector<int> rowDark(W ? H : 0, 0), colDark(W, 0);

            for (int y = 0; y < H; ++y) {
                const uint8_t* row = data + (size_t)y * stride;
                int rd = 0;
                for (int x = 0; x < W; ++x) {
                    uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                    if (r <= 24 && g <= 24 && b <= 24) { ++darkPx; ++rd; ++colDark[x]; }
                }
                rowDark[y] = rd;
            }
            s.darkRatio = (double)darkPx / (double)((long long)W * H);

            const int rowThresh = (int)(W * 0.60);
            const int colThresh = (int)(H * 0.60);
            for (int y = 0; y < H; ++y)
                if (rowDark[y] >= rowThresh) {
                    ++s.darkRows;
                    if (s.rowMinY < 0) s.rowMinY = y;
                    s.rowMaxY = y;
                }
            for (int x = 0; x < W; ++x)
                if (colDark[x] >= colThresh) {
                    ++s.darkCols;
                    if (s.colMinX < 0) s.colMinX = x;
                    s.colMaxX = x;
                }

            // 重复帧检测：与上一帧逐字节比较（取样比较，省时间）
            if (!prevFrame.empty() && prevFrame.size() == (size_t)stride * H) {
                bool same = true;
                for (size_t i = 0; i < prevFrame.size(); i += 997) {   // 抽样
                    if (prevFrame[i] != data[i]) { same = false; break; }
                }
                if (same) dupFrames.push_back(frames);
            }
            if (prevFrame.size() != (size_t)stride * H) prevFrame.resize((size_t)stride * H);
            memcpy(prevFrame.data(), data, (size_t)stride * H);

            stats.push_back(s);
        }
        buf->Unlock();
        buf = nullptr; sample = nullptr;
        ++frames;
    }

    printf("  实扫 %lld 帧\n", frames);
    printf("  MEDIATYPECHANGED 出现在: %s\n", typeChangedAt.empty() ? "无" : "");
    for (size_t i = 0; i < typeChangedAt.size() && i < 5; ++i) printf("    第 %lld 帧\n", typeChangedAt[i]);
    printf("  缓冲长度突变: %s\n", lenChangedAt.empty() ? "无" : "");
    for (size_t i = 0; i < lenChangedAt.size() && i < 5; ++i) printf("    第 %lld 帧\n", lenChangedAt[i]);

    if (stats.empty()) return;

    // 暗像素占比最高的 12 帧 —— 这是「黑色条状」最可能现身的地方
    std::vector<FrameStat> top = stats;
    std::sort(top.begin(), top.end(), [](const FrameStat& a, const FrameStat& b) {
        return a.darkRatio > b.darkRatio;
    });
    printf("\n  暗像素占比最高的帧:\n");
    for (size_t i = 0; i < top.size() && i < 12; ++i) {
        const FrameStat& s = top[i];
        printf("    #%4lld  暗 %.1f%%  横黑带 %lld 行(y=%d..%d)  竖黑带 %lld 列(x=%d..%d)\n",
               s.idx, s.darkRatio * 100.0, s.darkRows, s.rowMinY, s.rowMaxY,
               s.darkCols, s.colMinX, s.colMaxX);
    }

    // 有竖黑带的帧（v1 完全漏检的形态）
    long long colFrames = 0;
    for (const auto& s : stats) if (s.darkCols > 0) ++colFrames;
    printf("\n  含竖黑带的帧数: %lld / %zu\n", colFrames, stats.size());
    long long rowFrames = 0;
    for (const auto& s : stats) if (s.darkRows > 0) ++rowFrames;
    printf("  含横黑带的帧数: %lld / %zu\n", rowFrames, stats.size());
    printf("  与上一帧完全相同的帧数: %zu（占比 %.1f%%）\n",
           dupFrames.size(), stats.size() ? 100.0 * dupFrames.size() / stats.size() : 0.0);
    if (!dupFrames.empty()) {
        printf("    重复帧号（前 12 个）: ");
        for (size_t i = 0; i < dupFrames.size() && i < 12; ++i) printf("%lld ", dupFrames[i]);
        printf("\n");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    MfScope mf;
    OverrideDataDir(SampleDataDir());
    if (argc < 2) {
        printf("用法: flicker_probe.exe \"文件1.mp4\" [\"文件2.mp4\" ...]\n");
        return 2;
    }
    printf("==== 黑色条状闪烁诊断 v2（暗像素占比 + 横竖黑带 + 重复帧）====\n");
    for (int i = 1; i < argc; ++i) {
        char tag[32]; snprintf(tag, sizeof(tag), "文件%d", i);
        DeepScan(argv[i], tag);
    }
    printf("\n==== 诊断结束 ====\n");
    return 0;
}
