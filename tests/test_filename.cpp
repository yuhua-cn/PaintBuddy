// ============================================================
//  录屏文件名提取测试：验证从窗口标题提取「文档名」的各种形态
//  仅测试纯字符串逻辑，不需要真实录屏，可在沙箱内确定性验证。
// ============================================================
#include "Recorder.h"
#include "Store.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace pb;

static int g_pass = 0, g_fail = 0;

static void Expect(const wchar_t* title, const std::wstring& want) {
    std::wstring got = Recorder::DocNameFromTitle(title);
    if (got == want) {
        ++g_pass;
        printf("  [PASS] \"%s\" -> \"%s\"\n", W2U(title).c_str(), W2U(got).c_str());
    } else {
        ++g_fail;
        printf("  [FAIL] \"%s\" -> \"%s\"  期望 \"%s\"\n",
               W2U(title).c_str(), W2U(got).c_str(), W2U(want).c_str());
    }
}

// 校验文档名是否「可用于建卡」（IsPlausibleDocName）。wantValid=true 期望放行。
static void ExpectValid(const wchar_t* doc, bool wantValid) {
    bool got = Recorder::IsPlausibleDocName(doc);
    if (got == wantValid) {
        ++g_pass;
        printf("  [PASS] \"%s\" -> %s\n", W2U(doc).c_str(), got ? "可用" : "拒绝");
    } else {
        ++g_fail;
        printf("  [FAIL] \"%s\" -> %s  期望 %s\n",
               W2U(doc).c_str(), got ? "可用" : "拒绝", wantValid ? "可用" : "拒绝");
    }
}

int main() {
    printf("==== 录屏文件名提取测试 ====\n\n");

    printf("[1] 真实标题（来自用户日志）\n");
    // 用户实际日志里的 PS 标题，正是本轮要修的场景
    Expect(L"shijian.psd @ 17.9% (图层 199, RGB/8) *", L"shijian");
    Expect(L"shijian.psd @ 11.1% (图层 199, RGB/8)", L"shijian");
    Expect(L"shijian.psd @ 11.1% (图层 199, RGB_8)_20261016_223244", L"shijian");

    printf("\n[2] 各绘画软件的标题格式\n");
    Expect(L"作品_线稿.csp - Clip Studio Paint", L"作品_线稿");
    Expect(L"涂鸦_01.sai2 - PaintTool SAI", L"涂鸦_01");
    Expect(L"untitled-1.psd - Adobe Photoshop", L"untitled-1");
    // CSP 中文本地化（优动漫）
    Expect(L"插画.psd - 优动漫PAINT", L"插画");

    printf("\n[3] 无分隔符 / 无扩展名\n");
    Expect(L"sketch.psd", L"sketch");
    Expect(L"未命名文档", L"未命名文档");
    Expect(L"painting.png", L"painting");

    printf("\n[4] 边界与异常输入\n");
    Expect(L"", L"绘画");
    Expect(L"   ", L"绘画");
    // "*.psd"：去扩展名后只剩 '*'（Windows 非法文件名字符），
    // 剥离后为空 → 回退默认名，这是更安全的行为
    Expect(L"*.psd @ 100%", L"绘画");
    Expect(L".psd", L".psd");                 // 点号在首位，不当扩展名处理
    Expect(L"a.b.c.psd - PS", L"a.b.c");      // 多级点号，只去最后一个扩展名
    Expect(L"文件名很长很长很长很长.psd @ 50%", L"文件名很长很长很长很长");

    printf("\n[5] PS 未保存标记 '*' 应被剥离\n");
    // 部分 PS 版本把未保存标记紧跟在文档名后（无 " @ " 缩放段）
    Expect(L"shijian.psd * - Adobe Photoshop", L"shijian");
    Expect(L"草稿.psd*", L"草稿");

    printf("\n[6] 不应误删正文中的点号\n");
    // "v1.2 草稿.psd" 的扩展名是 .psd，去掉后应保留 "v1.2 草稿"
    Expect(L"v1.2 草稿.psd - Adobe Photoshop", L"v1.2 草稿");

    printf("\n[7] IsPlausibleDocName：畸形文档名应被拒绝（安全网）\n");
    // 液化残片：DocNameFromTitle 在 " @ " 处切断，留下不配对的左括号
    ExpectValid(L"液化 (shijian", false);
    // 浮窗 / 面板名（主窗口锁定失效时的兜底）
    ExpectValid(L"色轮", false);
    ExpectValid(L"工具属性", false);
    ExpectValid(L"环境设置", false);
    // 进程名泄漏（空标题退化）
    ExpectValid(L"photoshop.exe", false);
    ExpectValid(L"clipstudiopaint.exe", false);
    // 空 / 纯空白
    ExpectValid(L"", false);
    ExpectValid(L"   ", false);

    printf("\n[8] IsPlausibleDocName：正常文档名应放行\n");
    ExpectValid(L"shijian", true);
    ExpectValid(L"作品_线稿", true);
    ExpectValid(L"未命名文档", true);
    ExpectValid(L"CLIP STUDIO PAINT", true);   // CSP 主窗口标题，必须放行
    ExpectValid(L"液化 (shijian.psd)", true);  // 括号配对的合法文件名
    ExpectValid(L"绘画", true);                // 空标题兜底默认名

    printf("\n[9] 端到端：液化/浮窗标题经解析后被安全网拦下\n");
    // 液化：解析出畸形名 → 拒绝 → 录屏冻结，不建「液化 (shijian」垃圾卡
    {
        std::wstring d = Recorder::DocNameFromTitle(L"液化 (shijian.psd @ 26.4%)");
        bool rejected = (d == L"液化 (shijian") && !Recorder::IsPlausibleDocName(d);
        if (rejected) { ++g_pass; printf("  [PASS] 液化标题 -> \"%s\" 被拒绝\n", W2U(d).c_str()); }
        else { ++g_fail; printf("  [FAIL] 液化标题 -> \"%s\" valid=%d\n",
                                W2U(d).c_str(), (int)Recorder::IsPlausibleDocName(d)); }
    }
    // PS 主窗口标题：解析出 shijian → 放行
    {
        std::wstring d = Recorder::DocNameFromTitle(L"shijian.psd @ 13.6% (图层 199, RGB/8) *");
        bool ok = (d == L"shijian") && Recorder::IsPlausibleDocName(d);
        if (ok) { ++g_pass; printf("  [PASS] PS 主标题 -> \"%s\" 放行\n", W2U(d).c_str()); }
        else { ++g_fail; printf("  [FAIL] PS 主标题 -> \"%s\" valid=%d\n",
                                W2U(d).c_str(), (int)Recorder::IsPlausibleDocName(d)); }
    }

    printf("\n==== 结果：%d 通过，%d 失败 ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
