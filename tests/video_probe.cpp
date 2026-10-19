// ============================================================
//  Video 模块隔离探针：逐步执行并 fflush，把崩溃点钉到具体调用。
//  界面预览以 0xC0000409(fail-fast) 崩溃且不刷新 stdout，无法定位，
//  故用本探针逐步打印，崩溃前最后一行即出问题的那一步。
// ============================================================
#include "Common.h"
#include "Store.h"
#include "Config.h"
#include "Video.h"
#include <cstdio>

using namespace pb;

static void Step(const char* msg) {
    printf("[step] %s\n", msg);
    fflush(stdout);
}

// 取路径的文件名部分（仅用于打印）
static std::wstring BaseNameForProbe(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

int main(int argc, char** argv) {
    Clock::Init();
    // 可选参数：直接指定要探测的视频目录（用于诊断用户真实录屏）
    if (argc > 1) {
        std::wstring dir = U2W(argv[1]);
        OverrideDataDir(W2U(dir));   // 把 videos 目录当作数据根来扫
        printf("== 诊断模式：探测 %s ==\n", argv[1]);
        fflush(stdout);

        // 逐个文件探测，报告可读性与索引状态
        std::vector<DocCard> cards = ScanDocCards();
        printf("卡片数 = %zu\n", cards.size());
        for (const auto& c : cards) {
            printf("\n[卡片] %s  legacy=%d files=%d\n",
                   W2U(c.doc).c_str(), (int)c.isLegacy, c.FileCount());
            fflush(stdout);
            for (const auto& f : c.files) {
                double sec = 0; int w = 0, h = 0;
                bool ok = ProbeVideo(f, sec, w, h);
                int64_t sz = -1;
                {
                    WIN32_FILE_ATTRIBUTE_DATA fad;
                    if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad))
                        sz = ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                }
                printf("  %s\n    -> ProbeVideo ok=%d sec=%.2f %dx%d  size=%lld\n",
                       W2U(BaseNameForProbe(f)).c_str(), (int)ok, sec, w, h, (long long)sz);
                fflush(stdout);
            }
        }
        printf("\n诊断完成\n");
        return 0;
    }

    OverrideDataDir(SampleDataDir());
    CfgReload();

    Step("SampleDataDir 已设置");
    printf("       数据目录 = %s\n", DataDir().c_str());
    printf("       视频目录 = %s\n", VideoDir().c_str());
    fflush(stdout);

    Step("扫描卡片 ScanDocCards()");
    std::vector<DocCard> cards = ScanDocCards();
    printf("       卡片数 = %zu\n", cards.size());
    for (const auto& c : cards) {
        printf("         doc=%s legacy=%d files=%d bytes=%lld sec=%.2f thumb=%s\n",
               W2U(c.doc).c_str(), (int)c.isLegacy, c.FileCount(),
               (long long)c.totalBytes, c.totalSeconds,
               c.thumbPath.empty() ? "(none)" : W2U(c.thumbPath).c_str());
    }
    fflush(stdout);

    if (cards.empty()) { Step("无卡片，结束"); return 0; }

    // 逐个文件探测（伪造文件应优雅失败，不得崩溃）
    for (const auto& c : cards) {
        for (const auto& f : c.files) {
            Step(("ProbeVideo " + W2U(BaseNameForProbe(f))).c_str());
            double sec = 0; int w = 0, h = 0;
            bool ok = ProbeVideo(f, sec, w, h);
            printf("       -> ok=%d sec=%.2f %dx%d\n", (int)ok, sec, w, h);
            fflush(stdout);
        }
    }

    // 缩略图生成（伪造文件应返回 false，不得崩溃）
    for (const auto& c : cards) {
        if (c.files.empty()) continue;
        Step(("MakeThumbnailFromVideo " + W2U(c.doc)).c_str());
        std::wstring out = ThumbPathFor(c);
        {
            size_t p = out.find_last_of(L'\\');
            if (p != std::wstring::npos) CreateDirectoryW(out.substr(0, p).c_str(), nullptr);
        }
        std::string err;
        bool ok = MakeThumbnailFromVideo(c.files[0], out, 320, err);
        printf("       -> ok=%d err=%s\n", (int)ok, err.c_str());
        fflush(stdout);
    }

    Step("PNG 回读 LoadPngToBgra（若上面成功）");
    for (const auto& c : cards) {
        std::wstring tp = ThumbPathFor(c);
        int w = 0, h = 0; std::vector<uint8_t> px;
        bool ok = LoadPngToBgra(tp, w, h, px);
        printf("       %s -> ok=%d %dx%d bytes=%zu\n",
               W2U(c.doc).c_str(), (int)ok, w, h, px.size());
        fflush(stdout);
    }

    Step("全部完成，无崩溃");
    return 0;
}
