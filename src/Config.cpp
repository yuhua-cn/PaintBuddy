#include "Config.h"
#include "Json.h"
#include "Store.h"
#include <mutex>

namespace pb {

// 覆盖 Photoshop / Clip Studio Paint / 优动漫 PAINT / SAI2 的常见进程名。
//
// 关于优动漫 PAINT：它是 CSP 的中国版（厦门联合优创发行），但属于**独立打包
// 发行**的产品，可执行文件名与 CSP 不同。这里曾写着「可执行文件同名，故一并
// 覆盖」——那个假设是错的，导致优动漫用户的作画完全不被识别、静默不记录。
//
// 匹配方式：IsTarget 对进程名做**全小写精确相等**比较（Capture 侧已 ToLowerA），
// 所以这里必须逐个小写全名列出，不能靠前缀或包含匹配。
static const char* const kDefaultTargets[] = {
    "photoshop.exe",            // Adobe Photoshop
    "clipstudiopaint.exe",      // Clip Studio Paint（本机 5.0.4 实测 CLIPStudioPaint.exe）
    "clipstudio.exe",           // CSP 启动器（本机实测 CLIPStudio.exe）
    "udmpaintpro.exe",          // 优动漫 PAINT（用户实测 UDMPaintPRO.exe）
    "sai2.exe",                 // PaintTool SAI 2
    "sai.exe",                  // PaintTool SAI 1（顺带兼容）
};

const char* const* DefaultTargets(size_t& count) {
    count = sizeof(kDefaultTargets) / sizeof(kDefaultTargets[0]);
    return kDefaultTargets;
}

static Config g_cfg;
static std::mutex g_cfgMtx;

Config& Cfg() { return g_cfg; }

void Config::LoadDefaults() {
    targets.clear();
    size_t n = 0;
    const char* const* d = DefaultTargets(n);
    for (size_t i = 0; i < n; ++i) targets.push_back(ToLowerA(d[i]));
    enabled = true;
    keyMergeMs = 800;
    captureMouseWhenIdle = false;
    mouseAsPenFallback = true;
    recordVideo = false;
    captureMode = 0;
    videoBitrateKbps = 5000;
    videoFps = 30;
    minClipMs = 400;
    recordAudio = false;
    videoCacheDir.clear();   // 空 = 用系统默认（%LOCALAPPDATA%\PaintingBuddy\videos）
    exportDir.clear();       // 空 = 用系统「视频」库
    n1Sec = 0.0;
    n2Sec = 0.0;
    depthGapSec = 0.0;
    darkMode = true;    // 默认深色
    lastTab = 0;
    autoStart = true;   // 默认开机自启
    ver = 5;
}

bool Config::IsTarget(const std::string& exeLower) const {
    for (const auto& t : targets) if (t == exeLower) return true;
    return false;
}

bool Config::Load(const std::string& path) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;
    JVal root; std::string err;
    if (!JParse(text, root, err)) { LogF("config parse failed: %s", err.c_str()); return false; }

    if (const JVal* v = root.find("enabled"))               enabled = v->asBool();
    if (const JVal* v = root.find("keyMergeMs"))            keyMergeMs = v->asI32();
    if (const JVal* v = root.find("captureMouseWhenIdle"))  captureMouseWhenIdle = v->asBool();
    if (const JVal* v = root.find("mouseAsPenFallback"))    mouseAsPenFallback = v->asBool();
    if (const JVal* v = root.find("recordVideo"))           recordVideo = v->asBool();
    if (const JVal* v = root.find("captureMode"))           captureMode = v->asI32();
    if (const JVal* v = root.find("videoBitrateKbps"))      videoBitrateKbps = v->asI32();
    if (const JVal* v = root.find("videoFps"))              videoFps = v->asI32();
    if (const JVal* v = root.find("minClipMs"))             minClipMs = v->asI32();
    if (const JVal* v = root.find("recordAudio"))           recordAudio = v->asBool();
    if (const JVal* v = root.find("videoCacheDir"))         videoCacheDir = v->asStr();
    if (const JVal* v = root.find("exportDir"))             exportDir = v->asStr();
    if (const JVal* v = root.find("n1Sec"))                 n1Sec = v->asNum();
    if (const JVal* v = root.find("n2Sec"))                 n2Sec = v->asNum();
    if (const JVal* v = root.find("depthGapSec"))           depthGapSec = v->asNum();
    if (const JVal* v = root.find("darkMode"))              darkMode = v->asBool();
    if (const JVal* v = root.find("lastTab"))               lastTab = v->asI32();
    if (const JVal* v = root.find("autoStart"))             autoStart = v->asBool();
    if (const JVal* v = root.find("ver"))                   ver = v->asI32();

    // 迁移：v1（无 ver 字段）配置把深色强制为新默认值
    if (ver < 2) {
        ver = 2;
        darkMode = true;
        Save(path);
    }
    // 迁移：v2 → v3。n1Sec 的语义从「普通间隔上限」（秒级）改为
    // 「暂停绘画下限」（几十秒级）。旧配置里手动设过的小值（如 3~5s）
    // 在新语义下等于「3 秒就算暂停」，会让暂停次数重新爆表。
    // 因此把落在旧语义量级内的手动值重置为自动推导。
    if (ver < 3) {
        ver = 3;
        bool migrated = false;
        if (n1Sec > 0.0 && n1Sec < 30.0) { n1Sec = 0.0; migrated = true; }
        Save(path);
        if (migrated) LogF("config migrated v2->v3: n1Sec reset to auto (old semantics)");
    }
    // 迁移：v3 → v4。新增 captureMode（录制模式）。旧配置无此字段时
    // Load 不会赋值，保持 LoadDefaults 设的 0（整屏），行为与升级前一致。
    if (ver < 4) {
        ver = 4;
        Save(path);
        LogF("config migrated v3->v4: added captureMode (default 0 = full-screen)");
    }
    // 迁移：v4 → v5。新增 autoStart（开机自动启动，默认开启）。
    // 旧配置无此字段，Load 不赋值，保持 LoadDefaults 设的 true；
    // 这里显式固化并落盘，让升级后的用户直接获得「开机自启」默认行为。
    if (ver < 5) {
        ver = 5;
        autoStart = true;
        Save(path);
        LogF("config migrated v4->v5: added autoStart (default on)");
    }
    if (const JVal* ta = root.arr("targets")) {
        targets.clear();
        for (const JVal& e : ta->a) if (e.t == JVal::Str) targets.push_back(ToLowerA(e.s));
    }

    // 合法性夹取，防止用户写坏配置导致除零/死循环
    if (keyMergeMs < 0) keyMergeMs = 0;
    if (keyMergeMs > 10000) keyMergeMs = 10000;
    if (videoFps < 1) videoFps = 1;
    if (videoFps > 120) videoFps = 120;
    if (videoBitrateKbps < 200) videoBitrateKbps = 200;
    if (videoBitrateKbps > 200000) videoBitrateKbps = 200000;
    if (minClipMs < 0) minClipMs = 0;
    if (captureMode < 0) captureMode = 0;
    if (captureMode > 1) captureMode = 1;
    if (targets.empty()) { size_t n = 0; const char* const* d = DefaultTargets(n);
        for (size_t i = 0; i < n; ++i) targets.push_back(ToLowerA(d[i])); }
    return true;
}

bool Config::Save(const std::string& path) const {
    JW w;
    w.ob();
    w.kv("enabled", enabled);
    w.k("targets").ab();
    for (const auto& t : targets) w.v(t);
    w.ae();
    w.kv("keyMergeMs", keyMergeMs);
    w.kv("captureMouseWhenIdle", captureMouseWhenIdle);
    w.kv("mouseAsPenFallback", mouseAsPenFallback);
    w.kv("recordVideo", recordVideo);
    w.kv("captureMode", captureMode);
    w.kv("videoBitrateKbps", videoBitrateKbps);
    w.kv("videoFps", videoFps);
    w.kv("minClipMs", minClipMs);
    w.kv("recordAudio", recordAudio);
    w.kv("videoCacheDir", videoCacheDir);
    w.kv("exportDir", exportDir);
    w.kv("n1Sec", n1Sec);
    w.kv("n2Sec", n2Sec);
    w.kv("depthGapSec", depthGapSec);
    w.kv("darkMode", darkMode);
    w.kv("lastTab", lastTab);
    w.kv("autoStart", autoStart);
    w.kv("ver", ver);
    w.oe();
    return WriteWholeFileAtomic(path, w.str());
}

void CfgReload() {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    g_cfg.LoadDefaults();
    std::string p = ConfigPath();
    if (!g_cfg.Load(p)) {
        // 首次运行或配置损坏 → 写入默认值
        g_cfg.Save(p);
    }
    // 配置里可能指定了自定义录屏缓存目录；加载后应用它。
    // 注意顺序：EnsureDirs 已先建好默认目录并写好 g_videoDir，
    // 这里只在配置非空且目录可用时覆盖。
    ApplyVideoDirOverride(g_cfg.videoCacheDir);
}

void CfgSave() {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    g_cfg.Save(ConfigPath());
}

// 用户配置的导出目录（为空表示用系统默认，由调用方回退到「视频」库）。
// 放在这里而非 Store.cpp：读取的是 Config，避免 Store 反向依赖 Config。
std::wstring ConfiguredExportDir() {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    return U2W(g_cfg.exportDir);
}

} // namespace pb
