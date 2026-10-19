#include "Store.h"
#include "Json.h"
#include <direct.h>
#include <sys/stat.h>
#include <io.h>
#include <share.h>      // _fsopen / _SH_DENYNO
#include <cerrno>
#include <cstdlib>      // _dupenv_s / free
#include <shlobj.h>
#include <cstdarg>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")   // RegOpenKeyExW / RegSetValueExW：开机自启

namespace pb {

// ------------------------------------------------------------
// 目录
// ------------------------------------------------------------
static std::string g_dataDir, g_logDir, g_videoDir, g_configPath;

// —— 数据目录健康状态（见 Store.h 的 DataDirHealth 说明）——
static DataDirHealth g_health;
const DataDirHealth& DataDirHealthState() { return g_health; }

// 创建目录并**检查返回值**。
// 原实现忽略 CreateDirectoryW 的结果：目录建不出来时无人知晓，后续所有写入
// 只是静默失败。这与录屏目录的 ApplyVideoDirOverride（下面）形成刺眼的不对称
// —— 那边有递归创建 + 可写性探测 + 失败日志，logs 目录却什么都不检查，
// 于是「录屏正常、统计全空」这种故障在磁盘上留不下任何证据。
// 返回 false 时通过 outErr 给出 GetLastError，供上层汇总。
static bool MkDirW(const std::wstring& p, DWORD* outErr = nullptr) {
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        if (outErr) *outErr = 0;
        return true;   // 已存在即视为成功（幂等）
    }
    if (CreateDirectoryW(p.c_str(), nullptr)) {
        if (outErr) *outErr = 0;
        return true;
    }
    DWORD e = GetLastError();
    // ERROR_ALREADY_EXISTS：并发/竞态下别的线程刚建好，仍是成功
    if (e == ERROR_ALREADY_EXISTS) {
        if (outErr) *outErr = 0;
        return true;
    }
    if (outErr) *outErr = e;
    return false;
}

// 可写性探测：建一个临时文件再删掉。
// 只验证「目录存在」是不够的 —— 只读盘、权限不足、被杀软拦截时，目录看着
// 好好的，写入却要到最后一步才失败，用户完全无法定位。这与
// ApplyVideoDirOverride 里的探测同源，只是那边用于「不可用就保留原值」，
// 这里用于「不可用就如实上报」。
static bool ProbeWritableW(const std::wstring& dir, DWORD* outErr = nullptr) {
    std::wstring probe = dir + L"\\.__pb_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (outErr) *outErr = GetLastError();
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    if (outErr) *outErr = 0;
    return true;
}

void EnsureDirs() {
    g_health = DataDirHealth();   // 复位（重入/测试场景下不残留上一次结果）

    wchar_t appdata[MAX_PATH] = {0};
    std::wstring base;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appdata))) {
        base = std::wstring(appdata) + L"\\PaintingBuddy";
    } else {
        base = std::wstring(L".\\PaintingBuddyData");
    }

    DWORD err = 0;
    g_health.baseOk = MkDirW(base, &err);
    std::wstring logs = base + L"\\logs";
    std::wstring vids = base + L"\\videos";
    if (!g_health.baseOk) g_health.lastError = err;

    // logs 建不出来时不必再探测（探测必然失败），但 videos 仍要试 ——
    // 录屏与统计是两条独立链路，一边坏不该连带放弃另一边。
    bool vidsOk = false;
    DWORD vidsErr = 0;
    if (g_health.baseOk) {
        g_health.logsOk = MkDirW(logs, &err);
        if (!g_health.logsOk) g_health.lastError = err;
        vidsOk = MkDirW(vids, &vidsErr);
    }

    g_dataDir   = W2U(base);
    g_logDir    = W2U(logs);
    g_videoDir  = W2U(vids);
    g_configPath= g_dataDir + "\\config.json";

    // g_dataDir 现在才有值，LogF 从此可用（它开头会判 g_dataDir.empty()）。
    // 这也是本函数里所有日志都必须放在赋值之后的原因 —— videos 的失败结果
    // 同样只能在这里补记，早一行都写不出去。
    if (g_health.baseOk && !vidsOk) {
        LogF("[dir] videos 子目录创建失败 err=%lu（录屏将不可用，不影响统计）", vidsErr);
    }
    g_health.writable = g_health.logsOk && ProbeWritableW(logs, &err);
    if (!g_health.writable && g_health.lastError == 0) g_health.lastError = err;

    if (!g_health.baseOk || !g_health.logsOk || !g_health.writable) {
        g_health.note = "数据目录不可用："
            + std::string(g_health.baseOk ? "" : "根目录创建失败; ")
            + std::string(g_health.logsOk ? "" : "logs 子目录创建失败; ")
            + std::string(g_health.writable ? "" : "logs 目录不可写; ")
            + "GetLastError=" + std::to_string(g_health.lastError)
            + "。总览图与习惯页将没有数据，录屏可能仍正常。";
        // 这条日志本身可能写不进去（同一个目录坏了），所以 note 必须留在内存里，
        // 由诊断导出与界面读取 —— 不能只依赖落盘日志这一条通路。
        LogF("[dir] %s base=%s", g_health.note.c_str(), g_dataDir.c_str());
    } else {
        LogF("[dir] 数据目录 %s（logs 可写探测通过）", g_dataDir.c_str());
    }
}

std::string  DataDir()  { return g_dataDir; }
std::wstring DataDirW() { return U2W(g_dataDir); }
std::string  LogDir()   { return g_logDir; }
std::wstring LogDirW()  { return U2W(g_logDir); }
std::string  VideoDir() { return g_videoDir; }
std::wstring VideoDirW(){ return U2W(g_videoDir); }
std::string  ConfigPath(){ return g_configPath; }

// 开发/测试工具专用：把数据目录重定向到隔离位置（通常是项目内 build\sampledata）。
// 与 EnsureDirs 的区别仅在于根目录来自调用方，而不是 %LOCALAPPDATA%。
void OverrideDataDir(const std::string& baseUtf8) {
    std::wstring base = U2W(baseUtf8);
    DWORD err = 0;
    g_health = DataDirHealth();
    g_health.baseOk = MkDirW(base, &err);
    if (!g_health.baseOk) g_health.lastError = err;
    std::wstring logs = base + L"\\logs";
    std::wstring vids = base + L"\\videos";
    if (g_health.baseOk) {
        g_health.logsOk = MkDirW(logs, &err);
        if (!g_health.logsOk) g_health.lastError = err;
        MkDirW(vids, &err);
    }
    g_dataDir   = baseUtf8;
    g_logDir    = W2U(logs);
    g_videoDir  = W2U(vids);
    g_configPath= g_dataDir + "\\config.json";

    // 必须一并刷新健康状态：否则重定向后诊断导出的仍是 EnsureDirs 时期
    // 针对 %LOCALAPPDATA% 的旧结论，与实际写入目标不符，会造成误报。
    g_health.writable = g_health.logsOk && ProbeWritableW(logs, &err);
    if (!g_health.writable && g_health.lastError == 0) g_health.lastError = err;
}

// ------------------------------------------------------------
// 按用户配置覆盖录屏缓存目录
//
// 与 EnsureDirs 的关系：EnsureDirs 在配置加载**之前**运行（它要先建好
// 数据目录才能找到 config.json），所以配置里的自定义目录只能在加载后
// 通过本函数覆盖。每次用户在界面改路径后也要再调一次。
//
// 用户可能填多级不存在的路径（如 D:\绘画录屏\缓存），因此必须递归创建。
// 创建失败（盘符不存在、无写权限）时**保留原值**并写日志 ——
// 否则录屏会写到一个无效路径上，用户看到的是「录了但找不到文件」。
// ------------------------------------------------------------
static bool MkDirRecursiveW(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD attr = GetFileAttributesW(p.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    // 逐级创建：先把父目录建好，再建自己
    size_t cut = p.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) {
        // 跳过形如 "D:" 的盘符根（cut==2 且 p[1]==':'）
        bool driveRoot = (cut == 2 && p.size() > 2 && p[1] == L':');
        if (!driveRoot) {
            std::wstring parent = p.substr(0, cut);
            if (!MkDirRecursiveW(parent)) return false;
        }
    }
    return CreateDirectoryW(p.c_str(), nullptr) != FALSE ||
           GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ApplyVideoDirOverride(const std::string& dirUtf8) {
    if (dirUtf8.empty()) return;          // 空 = 用默认，不覆盖
    std::wstring want = U2W(dirUtf8);
    // 去掉尾部的斜杠，避免 CreateDirectory 失败
    while (want.size() > 1 && (want.back() == L'\\' || want.back() == L'/'))
        want.pop_back();

    if (!MkDirRecursiveW(want)) {
        LogF("[dir] 录屏缓存目录不可用，保持原值 %s（请求：%s，err=%lu）",
             g_videoDir.c_str(), W2U(want).c_str(), GetLastError());
        return;
    }
    // 可写性验证：建一个临时文件再删掉。只验证「目录存在」是不够的，
    // 只读盘或权限不足时录屏会在写入阶段才失败，用户很难定位。
    std::wstring probe = want + L"\\.__pb_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogF("[dir] 录屏缓存目录不可写，保持原值 %s（请求：%s，err=%lu）",
             g_videoDir.c_str(), W2U(want).c_str(), GetLastError());
        return;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());

    g_videoDir = W2U(want);
    LogF("[dir] 录屏缓存目录 -> %s", g_videoDir.c_str());
}

// 开发工具的合成数据隔离目录（见 Common.h 说明）
std::string SampleDataDir() {
    char* buf = nullptr;
    size_t len = 0;
    std::string dir;
    if (_dupenv_s(&buf, &len, "PB_SAMPLE_DIR") == 0 && buf && *buf) {
        dir = buf;
    }
    if (buf) free(buf);
    if (dir.empty()) dir = "build\\sampledata";
    return dir;
}

void LogF(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (g_dataDir.empty()) return;
    std::string p = g_dataDir + "\\pb.log";

    // 大小上限保护：改用 RIDEV_PAGEONLY 注册整个 0x0D 页后，杂设备或异常
    // 报文可能让日志快速增长；而 LogF 无级别、无轮转，若不加限制会无限膨胀。
    // 超过上限就把当前日志轮转为 pb.log.old（覆盖上一个备份），保留最近上下文，
    // 既能继续诊断又不至于占满磁盘。稳态下各分支都有「只记一次」保护，
    // 这个 stat 的开销可忽略。
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        std::wstring wp = U2W(p);
        if (GetFileAttributesExW(wp.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER sz;
            sz.HighPart = fad.nFileSizeHigh;
            sz.LowPart  = fad.nFileSizeLow;
            const ULONGLONG kMaxLogBytes = 20ULL * 1024 * 1024;   // 20 MB
            if (sz.QuadPart > kMaxLogBytes) {
                std::wstring oldp = wp + L".old";
                MoveFileExW(wp.c_str(), oldp.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
    }

    // 必须用宽字符 API：p 是 UTF-8 字节串，而 fopen_s 会按进程 ACP（简体中文
    // Windows = CP936/GBK）解释它。Windows 用户名含任何非 ASCII 字符时
    // （%LOCALAPPDATA% = C:\Users\<用户名>\AppData\Local），路径就被解成乱码、
    // 指向一个不存在的目录，于是 pb.log 永远建不出来 —— 而它是唯一记录
    // 「当天日志打不开」的地方，故障因此完全静默。
    // 模式串保持 "a"（文本模式）与原实现一致，不改变换行与追加语义。
    FILE* f = nullptr;
    if (_wfopen_s(&f, U2W(p).c_str(), L"a") != 0 || !f) return;
    int64_t now = Clock::NowUs();
    fprintf(f, "[%s.%03d] ", UsToLocalStr(now, "%Y-%m-%d %H:%M:%S").c_str(), (int)((now / 1000) % 1000));
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ------------------------------------------------------------
// 文件工具
// ------------------------------------------------------------
bool ReadWholeFile(const std::string& pathUtf8, std::string& out) {
    std::wstring wp = U2W(pathUtf8);
    // 共享模式必须包含 FILE_SHARE_WRITE。
    // 当天的 .jsonl 正被 EventWriter 以「追加写」持有句柄；若这里只声明
    // FILE_SHARE_READ（= 不允许他人写），CreateFileW 会因共享冲突直接失败，
    // 于是当天数据一条也读不到 —— 表现为图表始终空白、分析「用时 0 ms」。
    HANDLE h = CreateFileW(wp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    out.clear();
    out.resize((size_t)sz.QuadPart);
    DWORD total = 0;
    while (total < sz.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(h, &out[total], (DWORD)(sz.QuadPart - total), &got, nullptr) || got == 0) {
            CloseHandle(h);
            return false;
        }
        total += got;
    }
    CloseHandle(h);
    return true;
}

bool WriteWholeFileAtomic(const std::string& pathUtf8, const std::string& data) {
    std::wstring wp = U2W(pathUtf8);
    std::wstring tmp = wp + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0, total = 0;
    while (total < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - total, 1u << 22);
        if (!WriteFile(h, data.data() + total, chunk, &wrote, nullptr) || wrote == 0) {
            CloseHandle(h);
            DeleteFileW(tmp.c_str());
            return false;
        }
        total += wrote;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    // 覆盖式替换
    if (!MoveFileExW(tmp.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::string> ListFiles(const std::string& dirUtf8, const std::string& suffixLower) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = U2W(dirUtf8) + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = W2U(fd.cFileName);
        if (!suffixLower.empty()) {
            if (name.size() < suffixLower.size()) continue;
            if (_stricmp(name.c_str() + name.size() - suffixLower.size(), suffixLower.c_str()) != 0) continue;
        }
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

int64_t FileSizeBytes(const std::string& pathUtf8) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(U2W(pathUtf8).c_str(), GetFileExInfoStandard, &fad)) return -1;
    return ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// ------------------------------------------------------------
// 事件写入器
// ------------------------------------------------------------
static EventWriter g_events;
EventWriter& Events() { return g_events; }

void EventWriter::EnsureForToday(int64_t nowUs) {
    std::string day = UsToLocalStr(nowUs, "%Y%m%d");
    if (day == curDay_ && fp_) return;

    // —— 重试冷却（相邻缺陷修复）——
    // curDay_ 在打开失败时也会被赋值（下面），于是下一条事件进来时
    // `day == curDay_ && fp_` 因 fp_ 为空而不成立 → 每条事件都重走一遍完整的
    // 路径拼接 + 打开尝试，且必然再次失败；每次失败还要调一次 LogF（它自己也要
    // 开文件），开销被放大。高频运笔时这是纯白耗。
    // 冷却期内直接返回。跨天时 day != curDay_，冷却自然失效 —— 新的一天值得重试，
    // 且用户可能已经修好了目录/权限。
    if (day == curDay_ && !fp_ && lastOpenFailUs_ != 0 &&
        (nowUs - lastOpenFailUs_) < EVENT_OPEN_RETRY_COOLDOWN_US) {
        return;
    }

    if (fp_) { fflush(fp_); fclose(fp_); fp_ = nullptr; }
    curDay_ = day;
    std::string p = LogDir() + "\\" + day + ".jsonl";
    // 必须以二进制模式写入：内容已是 UTF-8 字节，若用 ccs=UTF-8 会被 CRT 二次转码。
    //
    // 必须用 _wfsopen(..., _SH_DENYNO) 而不是 fopen(..., "ab")：
    // 实测（tests/share_probe.cpp）本 CRT 的 fopen 追加写会独占到「读取端无论
    // 请求哪种共享模式都拿到 err=32 共享冲突」。于是程序运行期间当天的 .jsonl
    // 一条也读不到，分析结果恒为空 —— 表现就是「怎么操作两个图表都不更新」。
    // _SH_DENYNO 明确允许他人并发读，配合读取端的 READ|WRITE 共享即可边写边分析。
    //
    // 为什么必须是 _wfsopen 而不是 _fsopen（主因修复）：p 是 UTF-8 字节串，
    // 而 _fsopen 会按进程 ACP（简体中文 Windows = CP936/GBK）解释它。Windows 用户名
    // 含任何非 ASCII 字符时（%LOCALAPPDATA% = C:\Users\<用户名>\AppData\Local），
    // 路径被解成乱码、指向不存在的目录 → 打开失败 → AppendLine 里 fp_ 为空直接
    // return，每条事件被无声丢弃。而唯一记录此失败的 LogF 又写不进同一个目录，
    // 于是「录屏正常、总览图与习惯全空」在磁盘上留不下任何证据。
    // _wfsopen 与 _fsopen 的共享语义完全一致，_SH_DENYNO 原样保留，
    // 上面那条「边写边分析」的并发读能力不受任何影响。
    fp_ = _wfsopen(U2W(p).c_str(), L"ab", _SH_DENYNO);
    if (!fp_) {
        lastOpenFailUs_ = nowUs;
        LogF("cannot open day log %s (errno=%d)", p.c_str(), errno);
    } else {
        lastOpenFailUs_ = 0;
        // 每天一条正向确认：证明写入链路通了、且共享模式正确。
        // 之前只有失败路径留痕，而失败时日志本身又写不出来，等于全无凭据。
        LogF("[store] 打开当天事件日志 %s（_SH_DENYNO 并发读）", p.c_str());
    }
}

std::string EventWriter::CurrentPath() const {
    if (curDay_.empty()) return std::string();
    return LogDir() + "\\" + curDay_ + ".jsonl";
}

uint64_t EventWriter::DroppedEvents() {
    std::lock_guard<std::mutex> lk(mtx_);
    return droppedEvents_;
}

void EventWriter::AppendLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(mtx_);
    EnsureForToday(Clock::NowUs());
    if (!fp_) {
        // 原来是无声 return：写入端打不开文件时，每条事件都被丢掉且不留任何痕迹，
        // 用户只看到图表全空。累计条数使其可被诊断导出量化。
        ++droppedEvents_;
        return;
    }
    fwrite(line.data(), 1, line.size(), fp_);
    fputc('\n', fp_);
    if (++pending_ >= 16) { fflush(fp_); pending_ = 0; }
}

void EventWriter::Flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) { fflush(fp_); pending_ = 0; }
}

void EventWriter::Close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (fp_) { fflush(fp_); fclose(fp_); fp_ = nullptr; }
    // 清除冷却：Close 之后仍可能有追加（测试工具、退出收尾顺序调整），
    // 若沿用上一次失败的时刻，冷却期内会跳过重试并静默丢弃事件。
    lastOpenFailUs_ = 0;
}

// ------------------------------------------------------------
// 键名描述（采集层与分析层共用）
// ------------------------------------------------------------
static std::string VkNameShared(int vk) {
    char buf[16];
    if (vk >= 'A' && vk <= 'Z') { snprintf(buf, sizeof(buf), "%c", (char)vk); return buf; }
    if (vk >= '0' && vk <= '9') { snprintf(buf, sizeof(buf), "%c", (char)vk); return buf; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(buf, sizeof(buf), "小键盘%d", vk - VK_NUMPAD0); return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1); return buf; }
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:      return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU:         return "Alt";
    case VK_LWIN: case VK_RWIN:                         return "Win";
    case VK_ESCAPE:     return "Esc";
    case VK_SPACE:      return "空格";
    case VK_RETURN:     return "回车";
    case VK_TAB:        return "Tab";
    case VK_BACK:       return "退格";
    case VK_DELETE:     return "Delete";
    case VK_INSERT:     return "Insert";
    case VK_LEFT:       return "左";
    case VK_RIGHT:      return "右";
    case VK_UP:         return "上";
    case VK_DOWN:       return "下";
    case VK_HOME:       return "Home";
    case VK_END:        return "End";
    case VK_PRIOR:      return "PageUp";
    case VK_NEXT:       return "PageDown";
    case VK_CAPITAL:    return "CapsLock";
    case VK_OEM_3:      return "`";
    case VK_OEM_MINUS:  return "-";
    case VK_OEM_PLUS:   return "=";
    case VK_OEM_4:      return "[";
    case VK_OEM_6:      return "]";
    case VK_OEM_5:      return "\\";
    case VK_OEM_1:      return ";";
    case VK_OEM_7:      return "'";
    case VK_OEM_COMMA:  return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2:      return "/";
    default: break;
    }
    snprintf(buf, sizeof(buf), "键%02X", vk);
    return buf;
}

std::string DescribeKey(int vk, uint8_t mods) {
    std::string s;
    if (mods & PB_MOD_CTRL)  s += "Ctrl+";
    if (mods & PB_MOD_SHIFT) s += "Shift+";
    if (mods & PB_MOD_ALT)   s += "Alt+";
    if (mods & PB_MOD_WIN)   s += "Win+";
    s += VkNameShared(vk);
    return s;
}

// ------------------------------------------------------------
// 序列化 / 反序列化
// ------------------------------------------------------------
static void SerPathPoints(JW& w, const char* key, const std::vector<PathPoint>& pts) {
    w.k(key).ab();
    for (const auto& p : pts) {
        w.ab();
        w.v(p.dtUs).v(p.x).v(p.y).v((double)p.speed);
        w.ae();
    }
    w.ae();
}
static void SerPenPoints(JW& w, const char* key, const std::vector<PenPoint>& pts) {
    w.k(key).ab();
    for (const auto& p : pts) {
        w.ab();
        w.v(p.dtUs).v(p.x).v(p.y).v((double)p.speed).v((double)p.pressure).v((int)p.flags);
        w.ae();
    }
    w.ae();
}

static std::string SerializeRecord(const RawRecord& r) {
    JW w;
    w.ob();
    w.kv("t", r.tUs);
    w.kv("ty", std::string(1, (char)r.type));
    if (r.kind != OpKind::Normal) w.kv("kind", OpKeyName(r.kind));
    switch (r.type) {
    case 'k':
        w.kv("vk", r.vk).kv("mods", (int)r.mods).kv("count", r.count);
        if (r.endUs && r.endUs != r.tUs) w.kv("e", r.endUs);
        break;
    case 'm':
    case 'M':
        w.kv("e", r.endUs).kv("btn", r.button)
         .kv("len", (double)r.pathLen).kv("sp", (double)r.maxSpeed);
        // 鼠标笔画与移动段的「键盘归属」标记都必须落盘。
        // 此前 'm'/'M' 分支漏写该字段，采集时算出的 kbAttr 全部丢失，
        // 重新加载后无法还原「按下键时的鼠标操作记为键盘操作」这条规则。
        if (r.kbAttr) w.kv("kb", true);
        // 滚轮合并次数（btn=3 时才可能 >1）：原始数据如实记录一次滚动
        // 由几条系统消息合成，与键盘的 count 字段同义。
        if (r.count > 1) w.kv("count", r.count);
        SerPathPoints(w, "pts", r.mpts);
        break;
    case 'p':
        w.kv("e", r.endUs).kv("dev", (int)r.device)
         .kv("did", (int)r.deviceId)
         .kv("era", r.eraser).kv("kb", r.kbAttr).kv("hov", r.hover)
         .kv("maxp", (double)r.maxPressure).kv("avgp", (double)r.avgPressure)
         .kv("maxs", (double)r.maxSpeed).kv("avgs", (double)r.avgSpeed)
         .kv("len", (double)r.pathLen);
        SerPenPoints(w, "pts", r.ppts);
        break;
    case 'f':
        w.kv("exe", r.exe).kv("title", r.title).kv("tgt", r.isTarget);
        break;
    default:
        break;
    }
    w.oe();
    return w.take();
}

std::string SerializeEvent(const RawRecord& r) { return SerializeRecord(r); }

static bool ParseRecord(const JVal& j, RawRecord& r, bool parsePoints = true) {
    r.tUs = j.i64("t");
    if (!r.tUs) return false;
    std::string ty = j.str("ty");
    if (ty.empty()) return false;
    r.type = (uint8_t)ty[0];
    r.kind = OpKindFromName(j.str("kind"));
    r.vk = j.i32("vk");
    r.mods = (uint8_t)j.i32("mods");
    r.count = std::max(1, j.i32("count", 1));
    r.endUs = j.i64("e", r.tUs);
    r.button = j.i32("btn");
    r.device = (uint8_t)j.i32("dev");
    r.deviceId = (uint32_t)j.i32("did");   // 旧日志无 did 字段 → 默认 0（未知）
    r.eraser = j.boo("era");
    r.kbAttr = j.boo("kb");
    r.hover = j.boo("hov");
    r.maxPressure = (float)j.num("maxp");
    r.avgPressure = (float)j.num("avgp");
    r.maxSpeed = (float)j.num("maxs");
    r.avgSpeed = (float)j.num("avgs");
    r.pathLen = (float)j.num("len");
    r.exe = j.str("exe");
    r.title = j.str("title");
    r.isTarget = j.boo("tgt");
    if (parsePoints) {
        if (const JVal* a = j.arr("pts")) {
            for (const JVal& e : a->a) {
                if (e.t != JVal::Arr || e.a.size() < 4) continue;
                if (r.type == 'p') {
                    PenPoint p;
                    p.dtUs = (int32_t)e.a[0].n;
                    p.x = (int32_t)e.a[1].n;
                    p.y = (int32_t)e.a[2].n;
                    p.speed = (float)e.a[3].n;
                    if (e.a.size() > 4) p.pressure = (float)e.a[4].n;
                    if (e.a.size() > 5) p.flags = (uint8_t)e.a[5].n;
                    r.ppts.push_back(p);
                } else {
                    PathPoint p;
                    p.dtUs = (int32_t)e.a[0].n;
                    p.x = (int32_t)e.a[1].n;
                    p.y = (int32_t)e.a[2].n;
                    if (e.a.size() > 3) p.speed = (float)e.a[3].n;
                    r.mpts.push_back(p);
                }
            }
        }
    }
    return true;
}

void AppendEvent(const RawRecord& r) {
    Events().AppendLine(SerializeRecord(r));
}

bool ParseEventLine(const std::string& line, RawRecord& out) {
    JVal j; std::string err;
    if (!JParse(line, j, err)) return false;
    out = RawRecord();
    return ParseRecord(j, out, false);   // 测试只关心分类字段，跳过轨迹
}

bool LoadDay(const std::string& day, DayEvents& out, bool parsePoints) {
    out.day = day;
    out.records.clear();
    std::string text;
    if (!ReadWholeFile(LogDir() + "\\" + day + ".jsonl", text)) return false;
    size_t i = 0;
    while (i < text.size()) {
        size_t nl = text.find('\n', i);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        if (end > i) {
            std::string line = text.substr(i, end - i);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                JVal j; std::string err;
                if (JParse(line, j, err)) {
                    RawRecord r;
                    if (ParseRecord(j, r, parsePoints)) out.records.push_back(std::move(r));
                }
            }
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    std::sort(out.records.begin(), out.records.end(),
              [](const RawRecord& a, const RawRecord& b) { return a.tUs < b.tUs; });
    return true;
}

std::vector<std::string> ListDays() {
    std::vector<std::string> days;
    for (const auto& f : ListFiles(LogDir(), ".jsonl")) {
        std::string d = f.substr(0, f.size() - 6);
        if (d.size() == 8) days.push_back(d);
    }
    std::sort(days.begin(), days.end());
    return days;
}

// ------------------------------------------------------------
// 开机自动启动（HKCU Run 键，当前用户级，无需管理员）
//
// 为什么放 Store.cpp 而不是新建 .cpp：build_uipreview.bat /
// build_test_filename.bat 等无头构建用**显式 obj 列表**，新建源文件会
// 导致它们链接失败、需逐个改 .bat；Store.cpp 每个构建都已包含，塞进来
// 可完全绕开这个坑（见 build_*.bat）。
//
// 注册表值数据形如： "D:\...\PaintingBuddy.exe" --tray
// 路径必须加引号 —— exe 路径含空格或中文时，不加引号会被命令行解析器
// 在空格处截断，导致开机启动的是不存在的目标。--tray 让程序静默进托盘。
// ------------------------------------------------------------
static const wchar_t* const kRunSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// 测试版与正式版**共用同一 Run 子键**，故值名必须区分：
// 若两者都写 "PaintingBuddy"，后启动的那个会覆盖前者的路径，
// 表现为「装了正式版却开机启动了测试版」（或反之），且互相反复覆盖。
#ifdef PB_BETA
static const wchar_t* const kRunValueName = L"PaintingBuddyBeta";
#else
static const wchar_t* const kRunValueName = L"PaintingBuddy";
#endif

// 当前 exe 的绝对路径（宽字符）。取不到返回空串。
std::wstring ExePathW() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf, n);
}

// 写入或删除 Run 值。enable=true 写当前 exe 路径 + --tray；false 删除该值。
// 返回是否成功。
bool AutoStartApply(bool enable) {
    HKEY hk = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0,
                           KEY_SET_VALUE | KEY_QUERY_VALUE, &hk);
    if (r != ERROR_SUCCESS) {
        LogF("[autostart] 打开 Run 键失败 %ld", r);
        return false;
    }
    bool ok = false;
    if (!enable) {
        // 删除不存在的值也算成功（幂等）
        r = RegDeleteValueW(hk, kRunValueName);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    } else {
        std::wstring exe = ExePathW();
        if (exe.empty()) {
            LogF("[autostart] 取不到 exe 路径，跳过写入");
        } else {
            std::wstring cmd = L"\"" + exe + L"\" --tray";
            r = RegSetValueExW(hk, kRunValueName, 0, REG_SZ,
                               (const BYTE*)cmd.c_str(),
                               (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            ok = (r == ERROR_SUCCESS);
            if (!ok) LogF("[autostart] 写入 Run 值失败 %ld", r);
        }
    }
    RegCloseKey(hk);
    return ok;
}

// 查询 Run 值当前是否存在（供 UI 显示真实状态）。
bool AutoStartEnabled() {
    HKEY hk = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_QUERY_VALUE, &hk);
    if (r != ERROR_SUCCESS) return false;
    bool exists = (RegQueryValueExW(hk, kRunValueName, nullptr, nullptr, nullptr, nullptr)
                   == ERROR_SUCCESS);
    RegCloseKey(hk);
    return exists;
}

// 读取 Run 值当前的命令行内容。不存在或失败返回空串。
std::wstring AutoStartReadCommand() {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubKey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[1024] = {0};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(hk, kRunValueName, nullptr, &type, (BYTE*)buf, &cb);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS || type != REG_SZ) return L"";
    return std::wstring(buf);
}

// 自愈式同步：每次启动把注册表对齐到「期望状态 + 当前 exe 路径」。
//
// want 由调用方传（main.cpp 传 Cfg().autoStart），Store 层不反向依赖 Config。
//
// 关键：**不能只比较「值存不存在」**。exe 挪过位置后值依然存在，但指向的是
// 旧路径 —— 那样开机会启动一个不存在的文件，而按存在性判断会认为「已一致」
// 直接跳过，自愈彻底失效。所以 want=true 时还要比对内容是否为当前 exe。
// 仅在确实不一致时才写，避免每次启动都无谓地写注册表
// （也减少对杀毒软件 / SmartScreen 的触发频率）。
void AutoStartSync(bool want) {
    if (!want) {
        if (AutoStartEnabled() && AutoStartApply(false))
            LogF("[autostart] 已移除开机自启");
        return;
    }

    std::wstring exe = ExePathW();
    if (exe.empty()) { LogF("[autostart] 取不到 exe 路径，跳过同步"); return; }
    const std::wstring wantCmd = L"\"" + exe + L"\" --tray";

    std::wstring haveCmd = AutoStartReadCommand();
    if (_wcsicmp(haveCmd.c_str(), wantCmd.c_str()) == 0) return;   // 已一致，无需写

    if (!AutoStartApply(true)) return;
    if (haveCmd.empty())
        LogF("[autostart] 已写入开机自启：%s", W2U(exe).c_str());
    else
        LogF("[autostart] exe 路径已变更，更新开机自启：%s -> %s",
             W2U(haveCmd).c_str(), W2U(wantCmd).c_str());
}

} // namespace pb
