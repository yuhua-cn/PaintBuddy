// ============================================================
//  分片 MP4「中断安全性」验证
//
//  核心命题：录制过程中被强杀（不 Finalize）后，文件仍应可打开、
//  仍能读出分辨率与时长，且已写入的画面不丢失。
//  普通 MP4 做不到（索引在 Finalize 时才写到文件尾）。
//
//  做法：用与 Recorder 相同的分片 sink 参数写一段合成视频，
//  然后分两组对比 —— A 组正常 Finalize，B 组直接释放 writer 不 Finalize。
//  分别探测两个文件，B 组若仍可读即证明中断安全。
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Video.h"
#include <winrt/base.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace pb;
using winrt::com_ptr;

static int g_pass = 0, g_fail = 0;
static void Check(bool cond, const char* msg) {
    if (cond) { ++g_pass; printf("  [PASS] %s\n", msg); }
    else      { ++g_fail; printf("  [FAIL] %s\n", msg); }
    fflush(stdout);
}

static const int W = 320, H = 240, FPS = 30, FRAMES = 150;   // 5 秒视频（与生产同为 1s 分片）

// 生成一帧合成的 BGRA 画面（渐变 + 移动方块，便于肉眼确认内容非空）
static void MakeFrame(std::vector<uint8_t>& buf, int frameIdx) {
    buf.assign((size_t)W * H * 4, 0);
    int bx = (frameIdx * 4) % (W - 40);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &buf[((size_t)y * W + x) * 4];
            bool inBox = (x >= bx && x < bx + 40 && y >= 90 && y < 130);
            p[0] = inBox ? 0x20 : (uint8_t)(y % 256);      // B
            p[1] = inBox ? 0x80 : (uint8_t)(x % 256);      // G
            p[2] = inBox ? 0xFF : 0x40;                    // R
            p[3] = 0xFF;                                   // A
        }
    }
}

// 写一段分片 MP4。finalizeIt=false 时直接释放 writer，模拟进程被强杀。
static bool WriteFrag(const std::wstring& path, bool finalizeIt, std::string& err) {
    err.clear();
    const int w = W, h = H;
    UINT64 frameSize = ((UINT64)(UINT32)w << 32) | (UINT32)h;
    UINT64 frameRate = ((UINT64)(UINT32)FPS << 32) | 1;
    const LONGLONG frameDur = 10000000LL / FPS;

    com_ptr<IMFMediaType> outType;
    if (FAILED(MFCreateMediaType(outType.put()))) { err = "MT(out)"; return false; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
    outType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
    outType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    // 与 Recorder.cpp 一致：关键帧间隔 1 秒，与分片时长对齐。
    // 解码从关键帧起，中断损失的上界由两者中较大者决定。
    outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, 1000);

    // —— 与 Recorder 完全一致的分片 sink 创建路径 ——
    com_ptr<IMFByteStream> bs;
    // 必须用 DELETE_IF_EXIST：实测 MF_OPENMODE_RESET_IF_EXIST 不会创建文件，
    // 文件不存在时直接返回 0x80070002（RESET 只是「已存在则截断」）。
    HRESULT hrBs = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST,
                                MF_FILEFLAGS_NONE, path.c_str(), bs.put());
    if (FAILED(hrBs) || !bs) {
        char b[160]; snprintf(b, sizeof(b), "MFCreateFile 0x%08lX path=%s",
                              (unsigned long)hrBs, W2U(path).c_str());
        err = b; return false;
    }

    com_ptr<IMFAttributes> sinkAttr;
    MFCreateAttributes(sinkAttr.put(), 2);
    if (sinkAttr) {
        sinkAttr->SetUINT32(MF_MPEG4SINK_MOOV_BEFORE_MDAT, TRUE);
        // 与生产代码一致：1 秒分片阈值（见 Recorder.cpp），
        // 5 秒视频会写出多个片段，未收尾时已写片段仍应可读。
        sinkAttr->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION, 10000000ULL);
    }

    com_ptr<IMFMediaSink> fSink;
    HRESULT hrSink = MFCreateFMPEG4MediaSink(bs.get(), outType.get(), nullptr, fSink.put());
    if (FAILED(hrSink) || !fSink) {
        char b[96]; snprintf(b, sizeof(b), "FMPEG4Sink 0x%08lX", (unsigned long)hrSink);
        err = b; return false;
    }

    com_ptr<IMFAttributes> attrs;
    MFCreateAttributes(attrs.put(), 2);
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);

    com_ptr<IMFSinkWriter> writer;
    HRESULT hrW = MFCreateSinkWriterFromMediaSink(fSink.get(), attrs.get(), writer.put());
    if (FAILED(hrW) || !writer) {
        char b[96]; snprintf(b, sizeof(b), "SinkWriterFromMediaSink 0x%08lX", (unsigned long)hrW);
        err = b; return false;
    }

    // MFCreateFMPEG4MediaSink 传入 outType 时已建好视频流，直接用 index 0；
    // 再调 AddStream 会多出一条无样本空流，导致 Finalize 报 HEADERS_NOT_FOUND。
    DWORD idx = 0;

    com_ptr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(inType.put()))) { err = "MT(in)"; return false; }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
    inType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
    inType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    inType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 1);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
    inType->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)w * (UINT32)h * 4u);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));
    if (FAILED(writer->SetInputMediaType(idx, inType.get(), nullptr))) { err = "SetInputMediaType"; return false; }
    if (FAILED(writer->BeginWriting())) { err = "BeginWriting"; return false; }

    std::vector<uint8_t> frame;
    for (int i = 0; i < FRAMES; ++i) {
        MakeFrame(frame, i);
        com_ptr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer((DWORD)frame.size(), buf.put())) || !buf) { err = "MemBuf"; return false; }
        BYTE* data = nullptr;
        if (FAILED(buf->Lock(&data, nullptr, nullptr)) || !data) { err = "Lock"; return false; }
        memcpy(data, frame.data(), frame.size());
        buf->Unlock();
        buf->SetCurrentLength((DWORD)frame.size());

        com_ptr<IMFSample> sample;
        if (FAILED(MFCreateSample(sample.put())) || !sample) { err = "Sample"; return false; }
        sample->AddBuffer(buf.get());
        sample->SetSampleTime((LONGLONG)i * frameDur);
        sample->SetSampleDuration(frameDur);

        HRESULT hrWr = writer->WriteSample(idx, sample.get());
        if (FAILED(hrWr)) {
            char b[96]; snprintf(b, sizeof(b), "WriteSample 0x%08lX", (unsigned long)hrWr);
            err = b; return false;
        }
    }

    if (finalizeIt) {
        HRESULT hrF = writer->Finalize();
        if (FAILED(hrF)) { char b[96]; snprintf(b, sizeof(b), "Finalize 0x%08lX", (unsigned long)hrF); err = b; return false; }
    }
    // 关键：finalizeIt=false 时这里不调 Finalize，直接让 writer 析构，
    // 等价于进程被强杀时文件停在「未收尾」状态。
    writer = nullptr;
    try { fSink->Shutdown(); } catch (...) {}
    fSink = nullptr;
    bs = nullptr;
    return true;
}

// 实际解码一段视频，返回成功解出的帧数（比读 duration 更可靠：
// 分片 MP4 未收尾时 duration 元数据本就缺失，但已写片段仍可解码播放）
static int DecodeFrameCount(const std::wstring& path) {
    com_ptr<IMFAttributes> rattrs;
    if (FAILED(MFCreateAttributes(rattrs.put(), 2))) return -1;
    rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1);

    com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), rattrs.get(), reader.put())) || !reader)
        return -1;

    int frames = 0;
    for (int i = 0; i < 100000; ++i) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        com_ptr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                        &stream, &flags, &ts, sample.put());
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (sample) ++frames;
    }
    return frames;
}

int main() {
    Clock::Init();
    OverrideDataDir(SampleDataDir());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needCoUninit = SUCCEEDED(hr);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    std::wstring dir = U2W(SampleDataDir());
    CreateDirectoryW(dir.c_str(), nullptr);
    // MF 的文件接口只接受绝对路径（相对路径会直接失败），
    // 因此把测试目录解析成绝对路径后再拼接文件名。
    wchar_t absBuf[MAX_PATH] = {0};
    DWORD n = GetFullPathNameW(dir.c_str(), MAX_PATH, absBuf, nullptr);
    if (n > 0 && n < MAX_PATH) dir = absBuf;
    printf("测试目录（绝对路径）: %s\n", W2U(dir).c_str());
    fflush(stdout);
    std::wstring okPath = dir + L"\\fragtest_finalized.mp4";
    std::wstring killPath = dir + L"\\fragtest_killed.mp4";

    printf("==== 分片 MP4 中断安全性验证 ====\n\n");
    printf("[A] 正常收尾（对照组）\n");
    std::string err;
    bool okA = WriteFrag(okPath, true, err);
    printf("    写入 %s err=%s\n", okA ? "成功" : "失败", err.c_str());
    Check(okA, "分片 MP4 正常写入并 Finalize");

    printf("\n[B] 不 Finalize 直接释放（模拟进程被强杀）\n");
    err.clear();
    bool okB = WriteFrag(killPath, false, err);
    printf("    写入 %s err=%s\n", okB ? "成功" : "失败", err.c_str());
    Check(okB, "未收尾的分片 MP4 也能写完全部帧");

    // —— 探测对照组 ——
    double secA = 0; int wA = 0, hA = 0;
    bool probeA = ProbeVideo(okPath, secA, wA, hA);
    printf("\n[A] ProbeVideo -> ok=%d sec=%.2f %dx%d\n", (int)probeA, secA, wA, hA);
    Check(probeA && wA == W && hA == H, "对照组：分辨率正确（验证 GetUINT64 修复）");
    {
        double expectSec = (double)FRAMES / FPS;
        printf("    期望时长 %.2f 秒，实际 %.2f 秒\n", expectSec, secA);
        Check(secA > expectSec - 0.3 && secA < expectSec + 0.3, "对照组：时长与写入帧数一致");
    }

    // —— 探测「被强杀」组：这是本测试的核心 ——
    double secB = 0; int wB = 0, hB = 0;
    bool probeB = ProbeVideo(killPath, secB, wB, hB);
    printf("[B] ProbeVideo -> ok=%d sec=%.2f %dx%d\n", (int)probeB, secB, wB, hB);
    Check(probeB, "★ 未收尾的分片文件仍可被打开解析");
    Check(probeB && wB == W && hB == H, "★ 未收尾的分片文件仍能读出分辨率");
    // 注意：分片 MP4 未收尾时 duration 元数据缺失（sec=0）是正常现象，
    // 播放器靠逐个片段累加时长。因此这里验证「能否真的解码出帧」，
    // 这才是「中断后仍可播放」的真实标准，而不是 duration 是否非零。
    int framesB = DecodeFrameCount(killPath);
    int framesA = DecodeFrameCount(okPath);
    printf("    解码帧数：对照组 %d 帧，被强杀组 %d 帧（共写入 %d 帧）\n",
           framesA, framesB, FRAMES);
    Check(framesA == FRAMES, "对照组：写入的全部帧都能解码");
    Check(framesB > 0, "★ 未收尾的分片文件能实际解码出画面（可播放）");
    // 分片 MP4 的固有属性：进程被强杀时，只有「尚未完成的最后一个分片」丢失，
    // 损失量由分片时长与关键帧间隔共同决定（本测试与生产同为 1 秒）。
    // 这不是缺陷，而是「随时可打开」的代价上界：崩溃时最多丢约 1 秒内容，
    // 换来文件在任何时刻都能被打开播放。
    const int fragFrames = FPS;   // 1s 分片 = 30 帧
    printf("    损失 %d 帧（约 %.2f 秒），分片阈值 1.00 秒\n",
           FRAMES - framesB, (double)(FRAMES - framesB) / FPS);
    // 允许损失略多于一个分片（分片边界不必然与帧边界对齐），但必须远小于整段
    Check(framesB >= FRAMES - fragFrames * 2 && framesB < FRAMES,
          "★ 中断损失被限制在约一个分片时长内（不是整段丢失）");

    // —— 缩略图：同样应能在未收尾文件上成功 ——
    std::wstring thumbB = dir + L"\\fragtest_killed_thumb.png";
    err.clear();
    bool thumbOk = MakeThumbnailFromVideo(killPath, thumbB, 160, err);
    printf("[B] MakeThumbnail -> ok=%d err=%s\n", (int)thumbOk, err.c_str());
    Check(thumbOk, "★ 未收尾的分片文件仍能生成缩略图");

    // —— 文件大小对比：被强杀的文件不应是几十字节的空壳 ——
    WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
    GetFileAttributesExW(okPath.c_str(), GetFileExInfoStandard, &fa);
    GetFileAttributesExW(killPath.c_str(), GetFileExInfoStandard, &fb);
    long long sa = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    long long sb = ((long long)fb.nFileSizeHigh << 32) | fb.nFileSizeLow;
    printf("\n文件大小：对照组 %lld 字节，被强杀组 %lld 字节\n", sa, sb);
    Check(sb > 10000, "★ 被强杀的文件不是空壳（含实际画面数据）");

    // ============================================================
    // [C] 导出拼接：这是用户实际报错的路径（"无法确定视频分辨率"）。
    //     故意把「正常文件 + 未收尾文件」混在一张卡片里，
    //     验证导出仍能成功，且未收尾文件不会拖垮整个导出。
    // ============================================================
    printf("\n[C] 导出拼接（含一个未收尾文件）\n");
    {
        DocCard card;
        card.doc = L"fragtest";
        card.folder = dir;
        card.files.push_back(okPath);
        card.files.push_back(killPath);   // 未收尾文件混在其中
        for (const auto& f : card.files) {
            double s = 0; int w = 0, h = 0;
            if (ProbeVideo(f, s, w, h)) card.totalSeconds += s;
            WIN32_FILE_ATTRIBUTE_DATA fd{};
            if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fd))
                card.totalBytes += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
        printf("    卡片：%d 段，总时长 %.2f 秒，%lld 字节\n",
               card.FileCount(), card.totalSeconds, (long long)card.totalBytes);

        std::wstring outPath = dir + L"\\fragtest_exported.mp4";
        DeleteFileW(outPath.c_str());
        err.clear();
        float lastP = -1.f;
        bool okC = ConcatExport(card, outPath, err, [&](float p) {
            lastP = p;
            return true;   // 不取消
        });
        printf("    ConcatExport -> %s err=%s 进度终值=%.2f\n",
               okC ? "成功" : "失败", err.c_str(), lastP);
        Check(okC, "★ 导出拼接成功（此前会报「无法确定视频分辨率」）");
        Check(okC && lastP >= 0.99f, "★ 进度回调推进到 100%");

        if (okC) {
            double secC = 0; int wC = 0, hC = 0;
            bool probeC = ProbeVideo(outPath, secC, wC, hC);
            printf("    产物 ProbeVideo -> ok=%d sec=%.2f %dx%d\n",
                   (int)probeC, secC, wC, hC);
            Check(probeC && wC == W && hC == H, "★ 导出产物分辨率正确且可打开");

            int framesC = DecodeFrameCount(outPath);
            int expect = framesA + framesB;   // 两段帧数之和
            printf("    产物解码 %d 帧（两段合计应为 %d 帧）\n", framesC, expect);
            Check(framesC > 0, "★ 导出产物能实际解码出画面");
            Check(framesC >= expect - 10 && framesC <= expect + 10,
                  "★ 导出产物帧数约等于两段之和（无丢段、无重复）");

            WIN32_FILE_ATTRIBUTE_DATA fc{};
            GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &fc);
            long long sc = ((int64_t)fc.nFileSizeHigh << 32) | fc.nFileSizeLow;
            printf("    产物大小 %lld 字节\n", sc);
            Check(sc > 20000, "★ 导出产物不是空壳");
            DeleteFileW(outPath.c_str());
        }
    }

    // 清理测试产物
    DeleteFileW(okPath.c_str());
    DeleteFileW(killPath.c_str());
    DeleteFileW(thumbB.c_str());

    printf("\n==== 结果：%d 通过，%d 失败 ====\n", g_pass, g_fail);
    MFShutdown();
    if (needCoUninit) CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
