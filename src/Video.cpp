#include "Video.h"
#include "Store.h"
#include "Config.h"   // Cfg().videoBitrateKbps：导出码率与录制码率同源

// 与 Recorder.cpp 一致使用 winrt::com_ptr；包含本模块的构建脚本
// （build.bat / build_uipreview.bat）都已把 cppwinrt 头目录加进 INCLUDE。
#include <winrt/base.h>

#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mutex>
#include <map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

namespace pb {

using winrt::com_ptr;

// ============================================================
//  COM / MF 生命周期辅助（调用线程可能来自界面工作线程）
// ============================================================
namespace {

// COM/MF 生命周期守护。
//
// 关键：CoInitializeEx 返回 RPC_E_CHANGED_MODE 表示「本线程已用另一种
// 套间模型初始化过了」。此时 COM 是可用的（STA 下 WIC/MF 仍能工作），
// 但我们并没有增加引用计数，因此析构时绝不能调用 CoUninitialize ——
// 否则引用计数失衡会让 COM 被过早卸载，随后持有的 WIC 位图/工厂指针
// 全部悬空，表现为访问违规崩溃（离屏预览正是这样崩的：界面线程已按
// STA 初始化，这里再请求 MTA 必然拿到 RPC_E_CHANGED_MODE）。
// 故 needCoUninit_ 只在真正成功初始化（S_OK / S_FALSE）时置位。
struct MfScope {
    bool comUsable = false;      // COM 在本线程是否可用（含已初始化）
    bool needCoUninit_ = false;  // 是否由本对象负责注销
    bool mfOk = false;
    MfScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {              // S_OK 首次初始化 / S_FALSE 已初始化
            comUsable = true;
            needCoUninit_ = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            comUsable = true;             // 已由他人以 STA 初始化：可用但不注销
            needCoUninit_ = false;
        } else {
            comUsable = false;
        }
        mfOk = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
    }
    ~MfScope() {
        if (mfOk) MFShutdown();
        if (needCoUninit_) CoUninitialize();
    }
};

// 线程安全的 WIC 工厂（进程内单例）
IWICImagingFactory* GetWic() {
    static std::mutex m;
    static IWICImagingFactory* f = nullptr;
    std::lock_guard<std::mutex> lk(m);
    if (!f) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&f)))) {
            f = nullptr;
        }
    }
    return f;
}

std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// 从旧版散文件名反推文档名：剥掉尾部 "_YYYYMMDD_HHMMSS"、可选 "_partN"/"_N"
// 例：shijian_20261016_231105.mp4 -> shijian
//     shijian_20261016_231105_2.mp4 -> shijian
std::wstring DocNameFromLegacyFile(const std::wstring& fileName) {
    std::wstring s = fileName;
    // 去扩展名
    size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < s.size()) {
        std::wstring ext = s.substr(dot + 1);
        if (ext.size() <= 8 && ext.find(L' ') == std::wstring::npos) s = s.substr(0, dot);
    }
    // 反复剥掉尾部的 "_数字" 段（覆盖 _2、_part2 里的数字部分）
    auto stripNumTail = [](std::wstring& t) {
        size_t us = t.find_last_of(L'_');
        if (us == std::wstring::npos || us == 0 || us + 1 >= t.size()) return false;
        std::wstring tail = t.substr(us + 1);
        bool allDigit = true;
        for (wchar_t c : tail) if (!iswdigit(c)) { allDigit = false; break; }
        if (!allDigit) return false;
        t = t.substr(0, us);
        return true;
    };
    stripNumTail(s);   // 可能是 _2 / _part2 的序号（partN 含字母，剥不掉，下面单独处理）
    // "_partN"：剥掉 "partN" 段
    {
        size_t us = s.find_last_of(L'_');
        if (us != std::wstring::npos && s.size() > us + 4) {
            std::wstring tail = s.substr(us + 1);
            if (tail.size() > 4 && (tail.compare(0, 4, L"part") == 0)) {
                bool allDigit = true;
                for (size_t i = 4; i < tail.size(); ++i) if (!iswdigit(tail[i])) { allDigit = false; break; }
                if (allDigit) s = s.substr(0, us);
            }
        }
    }
    // 剥掉 "_HHMMSS"
    if (stripNumTail(s)) {
        // 再剥掉 "_YYYYMMDD"
        stripNumTail(s);
    }
    if (s.empty()) s = L"录屏";
    return s;
}

std::vector<std::wstring> ListMp4InFolder(const std::wstring& folder) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = folder + L"\\*.mp4";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());   // 文件名含时间戳 → 升序即时间升序
    return out;
}

int64_t FileSizeW(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

bool FileExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDirW(const std::wstring& path) {
    if (DirExistsW(path)) return;
    CreateDirectoryW(path.c_str(), nullptr);
}

} // namespace

// 公开实现：目录是否存在（见 Video.h）。
// 从匿名命名空间移出，供界面选择路径时校验用户输入。
bool DirExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================
//  目录与扫描
// ============================================================
std::wstring DocFolderPath(const std::wstring& docName) {
    std::wstring safe = SanitizeFileName(docName, 60);
    std::wstring folder = VideoDirW() + L"\\" + safe;
    EnsureDirW(folder);
    return folder;
}

std::vector<DocCard> ScanDocCards() {
    std::vector<DocCard> cards;
    std::wstring root = VideoDirW();
    if (!DirExistsW(root)) return cards;

    std::map<std::wstring, DocCard> legacy;   // 未归档散文件按反推文档名聚合

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return cards;
    do {
        std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // FindFirstFile 会返回 "." 与 ".." 两个特殊项，必须跳过：
            // 否则 "."（= videos 根目录自身）会被当成一张文档卡片，
            // 并把根目录下的散文件同时算进这张卡片，造成重复计数。
            if (name == L"." || name == L"..") continue;
            // 保留目录：散文件卡片的缩略图存放处，不是文档卡片
            if (name == L".thumbs") continue;
            // —— 一个子文件夹 = 一张卡片 ——
            DocCard c;
            c.doc = name;
            c.folder = root + L"\\" + name;
            c.files = ListMp4InFolder(c.folder);
            if (c.files.empty()) continue;      // 空文件夹不显示
            c.thumbPath = c.folder + L"\\thumb.png";
            if (!FileExistsW(c.thumbPath)) c.thumbPath.clear();
            for (const auto& f : c.files) {
                c.totalBytes += FileSizeW(f);
                double sec = 0; int w = 0, hh = 0;
                if (ProbeVideo(f, sec, w, hh)) c.totalSeconds += sec;
            }
            cards.push_back(c);
            continue;
        }
        // —— 根目录下的散文件（旧版布局遗留）——
        if (name.size() < 5) continue;
        std::wstring ext = name.substr(name.size() - 4);
        if (_wcsicmp(ext.c_str(), L".mp4") != 0) continue;

        std::wstring doc = DocNameFromLegacyFile(name);
        auto it = legacy.find(doc);
        if (it == legacy.end()) {
            DocCard c;
            c.doc = doc;
            c.folder = root;
            c.isLegacy = true;
            legacy[doc] = c;
            it = legacy.find(doc);
        }
        it->second.files.push_back(root + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (auto& kv : legacy) {
        DocCard& c = kv.second;
        std::sort(c.files.begin(), c.files.end());
        for (const auto& f : c.files) {
            c.totalBytes += FileSizeW(f);
            double sec = 0; int w = 0, hh = 0;
            if (ProbeVideo(f, sec, w, hh)) c.totalSeconds += sec;
        }
        c.thumbPath.clear();     // 散文件没有缩略图
        cards.push_back(c);
    }

    // 卡片排序：已归档在前（按文档名），未归档的排最后
    std::sort(cards.begin(), cards.end(), [](const DocCard& a, const DocCard& b) {
        if (a.isLegacy != b.isLegacy) return !a.isLegacy;
        return a.doc < b.doc;
    });
    return cards;
}

// ============================================================
//  PNG 读写（缩略图）
// ============================================================
bool SavePngFromBgra(const std::wstring& path, int w, int h, int stride, const void* bits) {
    if (!bits || w <= 0 || h <= 0 || stride <= 0) return false;
    IWICImagingFactory* wic = GetWic();
    if (!wic) return false;

    IWICStream* stream = nullptr;
    if (FAILED(wic->CreateStream(&stream)) || !stream) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        stream->Release();
        return false;
    }

    bool ok = false;
    IWICBitmapEncoder* enc = nullptr;
    if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc) {
        if (SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* props = nullptr;
            if (SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && frame) {
                if (SUCCEEDED(frame->Initialize(props))) {
                    frame->SetSize((UINT)w, (UINT)h);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                    frame->SetPixelFormat(&fmt);
                    // WIC 要求正 stride、自顶向下；BGRA 每像素 4 字节
                    if (SUCCEEDED(frame->WritePixels(h, (UINT)stride, (UINT)(stride * h),
                                                     (BYTE*)bits))) {
                        ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
                    }
                }
                if (props) props->Release();
                frame->Release();
            }
        }
        enc->Release();
    }
    stream->Release();
    return ok;
}

bool LoadPngToBgra(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out) {
    w = h = 0;
    out.clear();
    IWICImagingFactory* wic = GetWic();
    if (!wic) return false;

    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)) || !dec)
        return false;

    bool ok = false;
    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
        UINT fw = 0, fh = 0;
        if (SUCCEEDED(frame->GetSize(&fw, &fh)) && fw > 0 && fh > 0) {
            // 统一转成 32bppBGRA，绘制时无需再关心源格式
            IWICFormatConverter* conv = nullptr;
            if (SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv) {
                if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom))) {
                    UINT stride = fw * 4;
                    out.assign((size_t)stride * fh, 0);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, stride, (UINT)out.size(), out.data()))) {
                        w = (int)fw; h = (int)fh;
                        ok = true;
                    } else {
                        out.clear();
                    }
                }
                conv->Release();
            }
        }
        frame->Release();
    }
    dec->Release();
    return ok;
}

// ============================================================
//  视频探测（时长 / 分辨率）
// ============================================================
bool ProbeVideo(const std::wstring& path, double& seconds, int& width, int& height) {
    seconds = 0.0; width = height = 0;
    MfScope scope;
    if (!scope.mfOk) return false;

    com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put())) || !reader)
        return false;

    // MF_PD_DURATION 以 PROPVARIANT 返回，类型随容器而变，逐一兼容
    UINT64 dur100ns = 0;
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                        MF_PD_DURATION, &pv))) {
            if (pv.vt == VT_UI8)      dur100ns = pv.uhVal.QuadPart;
            else if (pv.vt == VT_UI4) dur100ns = pv.ulVal;
            else if (pv.vt == VT_R8)  dur100ns = (UINT64)(pv.dblVal * 1e7);
            PropVariantClear(&pv);
        }
    }
    seconds = (double)dur100ns / 1e7;

    com_ptr<IMFMediaType> mt;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, mt.put())) && mt) {
        // MF_MT_FRAME_SIZE 是 UINT64（高 32 位=宽，低 32 位=高）。
        // 必须用 GetUINT64 读取：此前误用 GetUINT32 会因类型不匹配而失败，
        // 导致分辨率恒为 0、导出报「无法确定视频分辨率」。
        UINT64 sz = 0;
        if (SUCCEEDED(mt->GetUINT64(MF_MT_FRAME_SIZE, &sz)) && sz) {
            width = (int)(sz >> 32);
            height = (int)(sz & 0xFFFFFFFFULL);
        }
    }
    return dur100ns > 0 || width > 0;
}

// ============================================================
//  缩略图：解码视频首帧 → 等比缩放 → 写 PNG
// ============================================================
bool MakeThumbnailFromVideo(const std::wstring& videoPath, const std::wstring& outPng,
                            int maxWidth, std::string& err) {
    err.clear();
    if (maxWidth <= 0) maxWidth = 320;

    MfScope scope;
    if (!scope.mfOk) { err = "Media Foundation 初始化失败"; return false; }

    com_ptr<IMFAttributes> rattrs;
    if (FAILED(MFCreateAttributes(rattrs.put(), 2))) { err = "创建解码属性失败"; return false; }
    rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1);

    com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(videoPath.c_str(), rattrs.get(), reader.put())) || !reader) {
        err = "打开视频失败";
        return false;
    }

    // 原生分辨率，用于计算等比缩放（MF_MT_FRAME_SIZE 是 UINT64，见 ProbeVideo）
    int srcW = 0, srcH = 0;
    {
        com_ptr<IMFMediaType> nmt;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nmt.put())) && nmt) {
            UINT64 sz = 0;
            if (SUCCEEDED(nmt->GetUINT64(MF_MT_FRAME_SIZE, &sz)) && sz) {
                srcW = (int)(sz >> 32);
                srcH = (int)(sz & 0xFFFFFFFFULL);
            }
        }
    }
    if (srcW < 2 || srcH < 2) { err = "无法读取视频分辨率"; return false; }

    int thW = srcW, thH = srcH;
    if (srcW > maxWidth) { thW = maxWidth; thH = (int)((double)srcH * (double)maxWidth / (double)srcW); }
    thW &= ~1; thH &= ~1;
    if (thW < 2) thW = 2;
    if (thH < 2) thH = 2;

    // 直接让 SourceReader 输出目标尺寸的 BGRA32（内部自动缩放），省去手动重采样
    com_ptr<IMFMediaType> outMt;
    if (FAILED(MFCreateMediaType(outMt.put()))) { err = "创建输出类型失败"; return false; }
    outMt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outMt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);   // BGRA8
    outMt->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)(UINT32)thW << 32) | (UINT32)thH);
    outMt->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(thW * 4));

    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outMt.get()))) {
        err = "设置解码输出类型失败";
        return false;
    }

    std::vector<uint8_t> pixels;
    bool gotFrame = false;
    for (int attempt = 0; attempt < 60 && !gotFrame; ++attempt) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        com_ptr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                        &stream, &flags, &ts, sample.put());
        if (FAILED(hr)) { err = "解码读取失败"; return false; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;

        com_ptr<IMFMediaBuffer> buf;
        // 首帧可能是压缩缓冲，先转成连续内存缓冲
        if (FAILED(sample->ConvertToContiguousBuffer(buf.put())) || !buf) continue;

        BYTE* data = nullptr; DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len))) continue;
        // BGRA32 连续存放，长度应为 thW*thH*4；不足则说明 stride 有对齐，按 stride 逐行拷
        UINT32 stride = (UINT32)thW * 4u;
        if (len >= stride * (UINT32)thH) {
            pixels.assign((size_t)stride * thH, 0);
            memcpy(pixels.data(), data, (size_t)stride * thH);
            gotFrame = true;
        }
        buf->Unlock();
    }
    if (!gotFrame || pixels.empty()) { err = "没有解码到任何帧"; return false; }

    if (!SavePngFromBgra(outPng, thW, thH, thW * 4, pixels.data())) {
        err = "写缩略图 PNG 失败";
        return false;
    }
    return true;
}

std::wstring ThumbPathFor(const DocCard& card) {
    if (!card.isLegacy) return card.folder + L"\\thumb.png";
    // 未归档散文件：缩略图另存于 videos\.thumbs\<doc>.png，
    // 刻意不移动用户既有文件，保持「清空=进回收站、导出=另存」的最小侵入。
    std::wstring dir = VideoDirW() + L"\\.thumbs";
    return dir + L"\\" + SanitizeFileName(card.doc, 60) + L".png";
}

// 宽高比是否可视为一致（相对差 < 0.5%）。
// 导出拼接时：比例一致 → 直接铺满目标尺寸（等比缩放，不变形、无黑边）；
// 比例不一致 → 等比缩放到能装进目标框的最大尺寸，四周补黑边居中。
// 用相对差而非绝对差：3840x2160 与 1920x1080 严格相等，而 1728x991 这类
// 非标准比例即使差 0.1% 也应判为「基本一致」，避免为几像素强行加黑边。
static bool SameAspect(int w1, int h1, int w2, int h2) {
    if (w1 <= 0 || h1 <= 0 || w2 <= 0 || h2 <= 0) return false;
    double r1 = (double)w1 / (double)h1;
    double r2 = (double)w2 / (double)h2;
    double avg = (r1 + r2) * 0.5;
    if (avg <= 0.0) return false;
    return fabs(r1 - r2) / avg < 0.005;
}

// ============================================================
//  拼接导出（解码 → 按最大面积统一尺寸 → 比例一致铺满/不一致补黑边 → 重编码）
// ============================================================
bool ConcatExport(const DocCard& card, const std::wstring& outPath, std::string& err,
                  const std::function<bool(float)>& progress) {
    err.clear();
    if (card.files.empty()) { err = "没有可导出的录屏文件"; return false; }

    MfScope scope;
    if (!scope.mfOk) { err = "Media Foundation 初始化失败"; return false; }

    // —— 目标尺寸：取所有文件中「面积最大」的那个（H264 要求偶数）——
    // 为什么用最大面积：同一文档的多次录制可能因窗口尺寸变化产生不同分辨率的
    // 片段（窗口模式下尤其常见）。以最大的为最终画布，小片段等比缩放后居中，
    // 比例一致则铺满、比例不一致则补黑边 —— 画面不会被拉伸变形。
    // 逐个 ProbeVideo：残缺文件（只有文件头、无索引）解析失败会被跳过，
    // 不影响其余正常文件；全部失败才报错。
    int tgtW = 0, tgtH = 0;
    long long bestArea = -1;
    std::vector<std::pair<int,int>> fileDims(card.files.size(), std::make_pair(0, 0));
    for (size_t i = 0; i < card.files.size(); ++i) {
        double s = 0; int w = 0, hh = 0;
        if (ProbeVideo(card.files[i], s, w, hh) && w >= 16 && hh >= 16) {
            fileDims[i] = std::make_pair(w, hh);
            long long area = (long long)w * (long long)hh;
            if (area > bestArea) { bestArea = area; tgtW = w; tgtH = hh; }
        }
    }
    tgtW &= ~1; tgtH &= ~1;
    if (tgtW < 16 || tgtH < 16) {
        err = "无法确定视频分辨率（该卡片的录屏文件可能都已损坏或为空）";
        return false;
    }
    LogF("[export] 目标画布 = %dx%d（最大面积）", tgtW, tgtH);

    // 导出码率与录制码率同源（设置页的「码率（kbps）」），夹取下限与 Recorder.cpp 一致：
    // 低于 200kbps 的 H264 在整屏分辨率下编码器易出异常，回落到 200000bps 保底。
    const int fps = 30;
    const LONGLONG frameDur = 10000000LL / fps;
    UINT bitrate = (UINT)Cfg().videoBitrateKbps * 1000u;
    if (bitrate < 200000u) bitrate = 200000u;

    // —— 建写入器（与 Recorder 相同的非压缩输入属性要求）——
    com_ptr<IMFSinkWriter> writer;
    {
        com_ptr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.put(), 2))) { err = "MFCreateAttributes 失败"; return false; }
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);
        if (FAILED(MFCreateSinkWriterFromURL(outPath.c_str(), nullptr, attrs.get(), writer.put())) || !writer) {
            err = "创建输出文件失败（路径不可写？）";
            return false;
        }

        UINT64 frameSize = ((UINT64)(UINT32)tgtW << 32) | (UINT32)tgtH;
        UINT64 frameRate = ((UINT64)(UINT32)fps << 32) | 1;

        com_ptr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(outType.put()))) { err = "MFCreateMediaType(out) 失败"; return false; }
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        outType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        outType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);

        DWORD idx = 0;
        if (FAILED(writer->AddStream(outType.get(), &idx))) { err = "AddStream(H264) 失败"; return false; }

        com_ptr<IMFMediaType> inType;
        if (FAILED(MFCreateMediaType(inType.put()))) { err = "MFCreateMediaType(in) 失败"; return false; }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
        inType->SetUINT64(MF_MT_FRAME_RATE, frameRate);
        inType->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
        // 非压缩输入必需属性，缺失会导致 WriteSample 每帧 E_INVALIDARG
        inType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 1);
        inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
        inType->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)tgtW * (UINT32)tgtH * 4u);
        inType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(tgtW * 4));

        if (FAILED(writer->SetInputMediaType(idx, inType.get(), nullptr))) {
            err = "SetInputMediaType(RGB32) 失败";
            return false;
        }
        if (FAILED(writer->BeginWriting())) { err = "BeginWriting 失败"; return false; }
    }

    // —— 逐个输入文件解码并写入 ——
    long long totalFrames = 0;
    bool cancelled = false;
    double totalDur = card.totalSeconds;
    if (totalDur <= 0.0) {
        for (const auto& f : card.files) {
            double s = 0; int w = 0, h2 = 0;
            if (ProbeVideo(f, s, w, h2)) totalDur += s;
        }
        if (totalDur <= 0.0) totalDur = 1.0;
    }
    double consumed = 0.0;

    // 黑边合成用的目标画布尺寸（RGB32, tgtW x tgtH）。每帧新建一个 MFMediaBuffer
    // 交给写入器（写入器要取得所有权），这里只预算字节数与行距。
    const size_t canvasBytes = (size_t)tgtW * (size_t)tgtH * 4u;
    const UINT32 canvasStride = (UINT32)tgtW * 4u;

    for (size_t fi = 0; fi < card.files.size() && !cancelled; ++fi) {
        const std::wstring& in = card.files[fi];
        double fileDur = 0; int w0 = 0, h0 = 0;
        ProbeVideo(in, fileDur, w0, h0);

        // 源分辨率（probe 失败则退回目标尺寸 = 直接铺满，等同旧行为）
        int srcW = (w0 >= 16 && h0 >= 16) ? w0 : tgtW;
        int srcH = (w0 >= 16 && h0 >= 16) ? h0 : tgtH;

        // 等比适配尺寸与居中偏移：
        //   · 比例一致 → 铺满 tgtW x tgtH，无需合成（走直接写入快路径）
        //   · 比例不一致 → 等比缩放到能装进目标框的最大尺寸，居中，四周补黑边
        int outW, outH, offX, offY;
        bool needPad;
        if (SameAspect(srcW, srcH, tgtW, tgtH)) {
            outW = tgtW; outH = tgtH; offX = 0; offY = 0; needPad = false;
        } else {
            double scale = std::min((double)tgtW / (double)srcW, (double)tgtH / (double)srcH);
            outW = (int)((double)srcW * scale + 0.5) & ~1;
            outH = (int)((double)srcH * scale + 0.5) & ~1;
            if (outW < 16) outW = 16;
            if (outH < 16) outH = 16;
            if (outW > tgtW) outW = tgtW & ~1;
            if (outH > tgtH) outH = tgtH & ~1;
            offX = (tgtW - outW) / 2;
            offY = (tgtH - outH) / 2;
            needPad = true;
        }

        com_ptr<IMFAttributes> rattrs;
        if (FAILED(MFCreateAttributes(rattrs.put(), 2))) { err = "创建解码属性失败"; break; }
        // 开启高级视频处理，使 SourceReader 能自动做格式转换与缩放
        rattrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 1);

        com_ptr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(in.c_str(), rattrs.get(), reader.put())) || !reader) {
            LogF("[export] 打开失败，跳过：%s", W2U(in).c_str());
            continue;   // 单个文件打不开不致命，跳过继续
        }

        // 输出类型：RGB32 @ 适配尺寸（outW x outH）。SourceReader 把源等比缩放到此，
        // 再由下面按 needPad 决定是否合成到 tgtW x tgtH 的黑底画布。
        com_ptr<IMFMediaType> outMt;
        if (FAILED(MFCreateMediaType(outMt.put()))) { err = "创建输出类型失败"; break; }
        outMt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outMt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        outMt->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)(UINT32)outW << 32) | (UINT32)outH);
        UINT32 stride = (UINT32)outW * 4;
        outMt->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
        outMt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, 1);
        outMt->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)outW * (UINT32)outH * 4u);
        outMt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);

        HRESULT hrSet = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outMt.get());
        if (FAILED(hrSet)) {
            LogF("[export] SetCurrentMediaType 失败 0x%08lX，跳过：%s",
                 (unsigned long)hrSet, W2U(in).c_str());
            continue;
        }

        long long framesInThisFile = 0;
        while (!cancelled) {
            DWORD actualStream = 0, flags = 0;
            LONGLONG ts = 0;
            com_ptr<IMFSample> sample;
            HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                            &actualStream, &flags, &ts, sample.put());
            if (FAILED(hr)) { err = "解码读取失败"; break; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample) continue;   // 被丢弃的帧（流切换等）

            HRESULT hrW;
            if (!needPad) {
                // 比例一致：适配尺寸 == 目标尺寸，直接写入，无合成开销。
                // 重新按「全局帧序号」连续排时间戳：各文件时间戳从 0 开始，
                // 直接沿用会在拼接处重叠或回退。
                sample->SetSampleTime(totalFrames * frameDur);
                sample->SetSampleDuration(frameDur);
                hrW = writer->WriteSample(0, sample.get());
            } else {
                // 比例不一致：把 outW x outH 帧居中合成到 tgtW x tgtH 黑底画布。
                com_ptr<IMFMediaBuffer> srcBuf;
                if (FAILED(sample->ConvertToContiguousBuffer(srcBuf.put())) || !srcBuf) {
                    err = "ConvertToContiguousBuffer 失败"; break;
                }
                BYTE* srcData = nullptr; DWORD srcMax = 0, srcLen = 0;
                if (FAILED(srcBuf->Lock(&srcData, &srcMax, &srcLen)) || !srcData) {
                    err = "锁定源缓冲失败"; break;
                }
                // 源行距：优先按总长反推（可能含对齐填充），退回 outW*4。
                UINT32 srcStride = (outH > 0 && srcLen >= (DWORD)outW * (DWORD)outH * 4u)
                                   ? (UINT32)(srcLen / (DWORD)outH) : (UINT32)outW * 4u;
                if (srcStride < (UINT32)outW * 4u) srcStride = (UINT32)outW * 4u;

                com_ptr<IMFMediaBuffer> dstBuf;
                if (FAILED(MFCreateMemoryBuffer((DWORD)canvasBytes, dstBuf.put())) || !dstBuf) {
                    srcBuf->Unlock();
                    err = "MFCreateMemoryBuffer 失败"; break;
                }
                BYTE* dstData = nullptr; DWORD dstMax = 0, dstCur = 0;
                if (FAILED(dstBuf->Lock(&dstData, &dstMax, &dstCur)) || !dstData || dstMax < canvasBytes) {
                    srcBuf->Unlock();
                    err = "锁定目标缓冲失败"; break;
                }
                memset(dstData, 0, canvasBytes);   // 黑底（含四周黑边）
                const size_t rowBytes = (size_t)outW * 4u;
                for (int y = 0; y < outH; ++y) {
                    const uint8_t* srow = (const uint8_t*)srcData + (size_t)y * srcStride;
                    uint8_t* drow = dstData + (size_t)(offY + y) * canvasStride + (size_t)offX * 4u;
                    memcpy(drow, srow, rowBytes);
                }
                dstBuf->Unlock();
                srcBuf->Unlock();
                dstBuf->SetCurrentLength((DWORD)canvasBytes);

                com_ptr<IMFSample> outSample;
                if (FAILED(MFCreateSample(outSample.put())) || !outSample) {
                    err = "MFCreateSample 失败"; break;
                }
                if (FAILED(outSample->AddBuffer(dstBuf.get()))) { err = "AddBuffer 失败"; break; }
                outSample->SetSampleTime(totalFrames * frameDur);
                outSample->SetSampleDuration(frameDur);
                hrW = writer->WriteSample(0, outSample.get());
            }

            if (FAILED(hrW)) {
                char b[128];
                snprintf(b, sizeof(b), "WriteSample 失败 0x%08lX", (unsigned long)hrW);
                err = b;
                break;
            }
            ++totalFrames;
            ++framesInThisFile;

            if (progress) {
                double done = consumed + (fileDur > 0.0
                            ? (double)framesInThisFile * frameDur / 1e7
                            : 0.0);
                float p = (float)(done / totalDur);
                if (p < 0.f) p = 0.f;
                if (p > 0.999f) p = 0.999f;
                if (!progress(p)) { cancelled = true; break; }
            }
        }
        consumed += (fileDur > 0.0) ? fileDur : (double)framesInThisFile * frameDur / 1e7;
        LogF("[export] %s  src=%dx%d -> out=%dx%d @(%d,%d) %s -> %lld 帧",
             W2U(BaseName(in)).c_str(), srcW, srcH, outW, outH, offX, offY,
             needPad ? "[pad]" : "[fill]", framesInThisFile);
    }

    if (cancelled) {
        // 取消：不 Finalize，删掉半成品
        writer = nullptr;
        DeleteFileW(outPath.c_str());
        err = "已取消";
        return false;
    }
    if (!err.empty()) {
        writer = nullptr;
        DeleteFileW(outPath.c_str());
        return false;
    }
    if (totalFrames == 0) {
        writer = nullptr;
        DeleteFileW(outPath.c_str());
        err = "没有解码到任何帧（源文件可能已损坏）";
        return false;
    }

    HRESULT hrF = writer->Finalize();
    writer = nullptr;
    if (FAILED(hrF)) {
        char b[128];
        snprintf(b, sizeof(b), "Finalize 失败 0x%08lX", (unsigned long)hrF);
        err = b;
        DeleteFileW(outPath.c_str());
        return false;
    }
    if (progress) progress(1.0f);
    LogF("[export] 完成：%s（%lld 帧，%.1f 秒）",
         W2U(BaseName(outPath)).c_str(), totalFrames, (double)totalFrames * frameDur / 1e7);
    return true;
}

// ============================================================
//  回收站删除（可恢复）
// ============================================================
bool RecyclePath(const std::wstring& path, std::string& err) {
    err.clear();
    if (path.empty()) { err = "路径为空"; return false; }
    // SHFileOperationW 要求 pFrom 双 null 结尾
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = nullptr;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int rc = SHFileOperationW(&op);
    if (rc != 0 || op.fAnyOperationsAborted) {
        char b[128];
        snprintf(b, sizeof(b), "移入回收站失败（SHFileOperation 返回 %d）", rc);
        err = b;
        return false;
    }
    return !DirExistsW(path) && !FileExistsW(path);
}

} // namespace pb
