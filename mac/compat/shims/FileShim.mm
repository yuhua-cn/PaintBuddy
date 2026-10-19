// ============================================================
//  文件 / 目录 / Shell 路径 API 的 macOS 实现
//
//  对应 compat/windows.h、compat/share.h、compat/shlobj.h 中声明的
//  全部文件类符号。目的是让 src/Store.cpp **原样编译**，从而
//  SerializeEvent / ParseEventLine / LoadDay / DescribeKey 这些
//  数据格式核心逻辑与 Windows 端逐字节一致 —— 这是整个移植
//  「历史数据互通」承诺的地基。
//
//  ⚠️ 贯穿本文件的头号陷阱：路径分隔符归一化。
//     Store.cpp 用 L"\\" 硬编码拼路径（:76 / :100 / :380），
//     而 macOS 上反斜杠是合法文件名字符、不是分隔符。
//     每个接受宽字符路径的入口都必须先 NormPath()，否则目录建不出来、
//     日志写不进，且**完全不报错** —— 与 Windows 端那个
//     「UTF-8 被按 GBK 解释致统计全空、磁盘零证据」的历史故障同款症状。
// ============================================================

#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mach-o/dyld.h>
#include <libproc.h>
#include <errno.h>

#include "windows.h"
#include "share.h"
#include "shlobj.h"
#include "shims/ShimHandle.h"

// pb::W2U / U2W / ToLowerA 来自 Common.h（本文件要把宽字符路径转 UTF-8，
// 并把 UTF-8 文件名转回宽字符填进 WIN32_FIND_DATAW）。
#include "Common.h"

using pbshim::ShimHandle;
using pbshim::HandleKind;

// GetDC 的哨兵句柄（extern 声明在 shims/ShimHandle.h；此处给出实体）
namespace pbshim { ShimHandle g_dcSentinelHandle; }

namespace {

// ------------------------------------------------------------
//  路径归一化：宽字符 UTF-32 → UTF-8，同时 '\' → '/'
//
//  复用 Common.h 的 W2U（它经垫片的 WideCharToMultiByte 走 UTF-32），
//  再逐字符替换分隔符。Windows 的盘符（C:）不会出现在 Mac 路径里，
//  故无需处理 "C:\" 这类特例。
// ------------------------------------------------------------
std::string NormPath(const wchar_t* w) {
    if (!w) return std::string();
    std::string s = pb::W2U(std::wstring(w));
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

// 把 UTF-8 路径按 '/' 切成各级，逐级 mkdir（等价 CreateDirectoryW 的
// 「父目录必须已存在」语义之外，这里做成递归创建更稳妥）。
bool MkDirAll(const std::string& path) {
    if (path.empty()) return false;
    std::string acc;
    size_t i = 0;
    if (path[0] == '/') { acc = "/"; i = 1; }
    while (i <= path.size()) {
        size_t next = path.find('/', i);
        if (next == std::string::npos) next = path.size();
        acc = path.substr(0, next);
        if (!acc.empty() && acc != "/") {
            if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (next >= path.size()) break;
        i = next + 1;
    }
    return true;
}

// CreateFileW 的 dwCreationDisposition + dwDesiredAccess → open() flags
int MapOpenFlags(DWORD access, DWORD disposition) {
    int flags = 0;
    bool rd = (access & GENERIC_READ) != 0;
    bool wr = (access & GENERIC_WRITE) != 0;
    if (rd && wr)      flags = O_RDWR;
    else if (wr)       flags = O_WRONLY;
    else               flags = O_RDONLY;

    switch (disposition) {
        case CREATE_NEW:        flags |= O_CREAT | O_EXCL;  break;
        case CREATE_ALWAYS:     flags |= O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING:     /* 无额外标志 */            break;
        case OPEN_ALWAYS:       flags |= O_CREAT;           break;
        case TRUNCATE_EXISTING: flags |= O_TRUNC;           break;
        default: break;
    }
    return flags;
}

ShimHandle* NewHandle(HandleKind k) {
    ShimHandle* h = new (std::nothrow) ShimHandle();
    if (h) h->kind = k;
    return h;
}

// 把 stat 结果映射成 WIN32 属性位。Store.cpp 只用 DIRECTORY 位与
// 「存在与否」（attr != INVALID_FILE_ATTRIBUTES），故只需这两项准确。
DWORD AttrFromStat(const struct stat& st) {
    DWORD a = FILE_ATTRIBUTE_NORMAL;
    if (S_ISDIR(st.st_mode))  a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

void FillFindData(WIN32_FIND_DATAW* fd, const char* nameUtf8,
                  const struct stat& st) {
    memset(fd, 0, sizeof(*fd));
    fd->dwFileAttributes = AttrFromStat(st);
    fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    fd->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
    // UTF-8 文件名 → UTF-32 宽字符，受 MAX_PATH 限制。
    // Store.cpp:338 会用 W2U(fd.cFileName) 转回 UTF-8，故此处必须填
    // 合法的 UTF-32，否则文件名会变成乱码、当天日志永远找不到。
    std::wstring w = pb::U2W(std::string(nameUtf8));
    size_t n = w.size();
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    memcpy(fd->cFileName, w.c_str(), n * sizeof(wchar_t));
    fd->cFileName[n] = L'\0';
}

} // anonymous namespace

// ------------------------------------------------------------
//  CreateFileW / ReadFile / WriteFile / CloseHandle
//
//  CloseHandle 的实现在 Win32Shim.mm（它要同时处理 Process / File / Find
//  三种句柄），但那里不知道 File/Find 的内部结构 —— 故本文件提供
//  一个供 CloseHandle 调用的分类释放函数 pbshim::ReleaseHandle（见下方）。
// ------------------------------------------------------------

// 供 Win32Shim.mm 的 CloseHandle 调用：按句柄类型释放。
// 返回 true 表示「本句柄已被识别并处理」，false 表示「不是本垫片的句柄」。
namespace pbshim {
bool ReleaseHandle(ShimHandle* h) {
    if (!h) return false;
    switch (h->kind) {
        case HandleKind::File:
            if (h->fd >= 0) close(h->fd);
            delete h; return true;
        case HandleKind::Find:
            if (h->dir) closedir((DIR*)h->dir);
            delete h; return true;
        case HandleKind::Process:
            delete h; return true;
        default:
            return false;   // 不是已知句柄，交回调用方处理
    }
}
}

HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD /*dwShareMode*/,
                   void* /*lpSecurityAttributes*/, DWORD dwCreationDisposition,
                   DWORD /*dwFlagsAndAttributes*/, HANDLE /*hTemplateFile*/) {
    if (!lpFileName) { errno = EINVAL; return INVALID_HANDLE_VALUE; }
    std::string path = NormPath(lpFileName);
    if (path.empty()) { errno = EINVAL; return INVALID_HANDLE_VALUE; }

    int flags = MapOpenFlags(dwDesiredAccess, dwCreationDisposition);
    // CREATE_* 需要 mode 参数（新建文件的权限）
    int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) {
        // CREATE_NEW 遇已存在 → 映射成 Win32 的 ERROR_ALREADY_EXISTS 语义
        return INVALID_HANDLE_VALUE;
    }
    ShimHandle* h = NewHandle(HandleKind::File);
    if (!h) { close(fd); return INVALID_HANDLE_VALUE; }
    h->fd = fd;
    return (HANDLE)h;
}

BOOL ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead,
              DWORD* lpNumberOfBytesRead, void* /*lpOverlapped*/) {
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
    ShimHandle* h = (ShimHandle*)hFile;
    if (!h || h->kind != HandleKind::File || !lpBuffer) return FALSE;

    // 循环读满：read() 可能短读（Store.cpp:294 已按 got==0 判 EOF，
    // 但一次 read 未必读满请求量，需累积）。
    size_t total = 0;
    while (total < nNumberOfBytesToRead) {
        ssize_t n = read(h->fd, (char*)lpBuffer + total,
                         nNumberOfBytesToRead - total);
        if (n < 0) { if (errno == EINTR) continue; return FALSE; }
        if (n == 0) break;              // EOF
        total += (size_t)n;
    }
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = (DWORD)total;
    return TRUE;                         // 短读到 EOF 也是成功（Win32 语义）
}

BOOL WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite,
               DWORD* lpNumberOfBytesWritten, void* /*lpOverlapped*/) {
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
    ShimHandle* h = (ShimHandle*)hFile;
    if (!h || h->kind != HandleKind::File || !lpBuffer) return FALSE;

    size_t total = 0;
    while (total < nNumberOfBytesToWrite) {
        ssize_t n = write(h->fd, (const char*)lpBuffer + total,
                          nNumberOfBytesToWrite - total);
        if (n < 0) { if (errno == EINTR) continue; return FALSE; }
        if (n == 0) break;
        total += (size_t)n;
    }
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = (DWORD)total;
    return (total == nNumberOfBytesToWrite) ? TRUE : FALSE;
}

DWORD GetFileAttributesW(LPCWSTR lpFileName) {
    if (!lpFileName) return INVALID_FILE_ATTRIBUTES;
    std::string path = NormPath(lpFileName);
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        // 路径含反斜杠残留时 stat 也会失败；NormPath 已处理，这里只报不存在
        return INVALID_FILE_ATTRIBUTES;
    }
    return AttrFromStat(st);
}

BOOL GetFileSizeEx(HANDLE hFile, LARGE_INTEGER* lpFileSize) {
    ShimHandle* h = (ShimHandle*)hFile;
    if (!h || h->kind != HandleKind::File || !lpFileSize) return FALSE;

    // 优先 fstat（不移动文件偏移量，Store.cpp 随后要从头 ReadFile）。
    // 不能用 lseek(fd,0,SEEK_END) 再 lseek 回去：那样在并发写场景下
    // 取到的大小与后续读取的起点可能不一致。
    struct stat st{};
    if (fstat(h->fd, &st) != 0) return FALSE;
    lpFileSize->QuadPart = (int64_t)st.st_size;
    return TRUE;
}

// Store.cpp:320 的 WriteWholeFileAtomic 调用：写完 .tmp、rename 覆盖前，
// 把数据真正落盘。⚠️ 必须真的 fsync，不能返回 TRUE 了事：
//   它与 MoveFileExW 的 MOVEFILE_WRITE_THROUGH 一起构成 config.json 的
//   原子写。若这里空实现，断电/崩溃时 rename 后的文件可能是 0 字节或半截，
//   下次启动 JSON 解析失败 → 用户全部配置（阈值/白名单/录屏参数）丢失。
BOOL FlushFileBuffers(HANDLE hFile) {
    ShimHandle* h = (ShimHandle*)hFile;
    if (!h || h->kind != HandleKind::File) return FALSE;
    return (fsync(h->fd) == 0) ? TRUE : FALSE;
}

BOOL GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS /*level*/,
                          void* lpFileInformation) {
    if (!lpFileName || !lpFileInformation) return FALSE;
    std::string path = NormPath(lpFileName);
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return FALSE;

    WIN32_FILE_ATTRIBUTE_DATA* fad = (WIN32_FILE_ATTRIBUTE_DATA*)lpFileInformation;
    memset(fad, 0, sizeof(*fad));
    fad->dwFileAttributes = AttrFromStat(st);
    fad->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    fad->nFileSizeLow  = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR lpPathName, void* /*lpSecurityAttributes*/) {
    if (!lpPathName) return FALSE;
    std::string path = NormPath(lpPathName);
    if (path.empty()) return FALSE;
    // Store.cpp:37/186 依赖「已存在也算成功」的幂等语义。
    if (mkdir(path.c_str(), 0755) == 0) return TRUE;
    if (errno == EEXIST) {
        // 已存在：确认它确实是目录
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return TRUE;
        return FALSE;
    }
    // 父目录不存在时递归创建（Windows 的 CreateDirectoryW 不会，但
    // Store.cpp 总是先建 base 再建 base/logs，父目录已存在，故这里
    // 递归创建只是额外稳健，不改变既有调用序列的结果）
    return MkDirAll(path) ? TRUE : FALSE;
}

BOOL DeleteFileW(LPCWSTR lpFileName) {
    if (!lpFileName) return FALSE;
    std::string path = NormPath(lpFileName);
    if (path.empty()) return FALSE;
    if (unlink(path.c_str()) == 0) return TRUE;
    return (errno == ENOENT) ? TRUE : FALSE;   // 删不存在的文件视为幂等成功
}

BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    if (!lpExistingFileName || !lpNewFileName) return FALSE;
    std::string src = NormPath(lpExistingFileName);
    std::string dst = NormPath(lpNewFileName);
    if (src.empty() || dst.empty()) return FALSE;

    if (!(dwFlags & MOVEFILE_REPLACE_EXISTING)) {
        struct stat st{};
        if (stat(dst.c_str(), &st) == 0) { errno = EEXIST; return FALSE; }
    }
    if (rename(src.c_str(), dst.c_str()) != 0) return FALSE;

    // MOVEFILE_WRITE_THROUGH：要求改名操作落盘（原子写的关键，
    // 见 windows.h 中该常量的注释）。POSIX 的 rename 本身是原子的，
    // 但元数据可能还在缓存里；对目标目录 fsync 一次确保断电后可见。
    if (dwFlags & MOVEFILE_WRITE_THROUGH) {
        std::string dir = dst;
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) dir = dir.substr(0, slash);
        int dfd = open(dir.c_str(), O_RDONLY);
        if (dfd >= 0) { fsync(dfd); close(dfd); }
    }
    return TRUE;
}

// ------------------------------------------------------------
//  目录遍历：FindFirstFileW / FindNextFileW / FindClose
//
//  ⚠️ 语义差异：Win32 的 FindFirstFileW **返回时就已定位到第一个条目**
//  （调用方随即读 fd 里的数据），而 POSIX 的 opendir 不返回任何条目。
//  Store.cpp:334-344 的用法是：
//      h = FindFirstFileW(pattern, &fd); if (h==INVALID) return;
//      do { 用 fd } while (FindNextFileW(h, &fd));
//  即「先处理 First 填的 fd，再循环 Next」。
//  故 FindFirstFileW 内部先读一个条目填进 fd；FindNextFileW 从第二个开始，
//  返回 FALSE 表示遍历结束（对应 Win32 的 ERROR_NO_MORE_FILES）。
//
//  pattern 形如 "dir\*"（Store.cpp:333 拼的是 U2W(dir) + L"\\*"）。
//  归一化后取目录部分（去掉末尾通配），忽略通配本身 —— Store.cpp 不做
//  扩展名过滤，ListFiles 的后缀匹配在拿到全名后自己做（Store.cpp:339-342）。
// ------------------------------------------------------------

// 读一个目录条目（跳过 . 与 ..），填进 ShimHandle 的 pending 缓存。
// 返回 true = 读到有效条目，false = 目录已尽或出错。
bool ReadOneEntry(ShimHandle* h) {
    DIR* d = (DIR*)h->dir;
    if (!d) return false;
    for (;;) {
        struct dirent* ent = readdir(d);
        if (!ent) return false;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        // 拼全路径取属性。d_type 在部分文件系统上可能是 DT_UNKNOWN，
        // 故一律 stat 一次，不依赖 d_type。
        std::string full = h->dirPath + "/" + ent->d_name;
        struct stat st{};
        if (stat(full.c_str(), &st) != 0) continue;   // 竞态消失：跳过该条

        strncpy(h->pendingName, ent->d_name, sizeof(h->pendingName) - 1);
        h->pendingName[sizeof(h->pendingName) - 1] = '\0';
        h->pendingIsDir = S_ISDIR(st.st_mode);
        h->pendingSize  = (long long)st.st_size;
        h->pendingOK    = true;
        return true;
    }
}

// 把 ShimHandle 的 pending 缓存写进调用方的 WIN32_FIND_DATAW，并清除 pendingOK。
void EmitPending(ShimHandle* h, WIN32_FIND_DATAW* fd) {
    struct stat st{};
    st.st_size = (off_t)h->pendingSize;
    st.st_mode = h->pendingIsDir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    FillFindData(fd, h->pendingName, st);
    h->pendingOK = false;   // 已消费
}

HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!lpFileName || !lpFindFileData) { errno = EINVAL; return INVALID_HANDLE_VALUE; }

    std::string pattern = NormPath(lpFileName);
    // 去掉末尾通配，得到目录路径
    std::string dir = pattern;
    {
        size_t star = dir.find_last_of('*');
        if (star != std::string::npos) dir = dir.substr(0, star);
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    }
    if (dir.empty()) dir = ".";

    DIR* d = opendir(dir.c_str());
    if (!d) return INVALID_HANDLE_VALUE;

    ShimHandle* h = NewHandle(HandleKind::Find);
    if (!h) { closedir(d); return INVALID_HANDLE_VALUE; }
    h->dir     = d;
    h->dirPath = dir;        // FindNextFileW 要用它 stat 后续条目

    // First 的语义：立即读第一个条目填 fd。
    if (!ReadOneEntry(h)) {
        // 空目录：Win32 返回 ERROR_FILE_NOT_FOUND、句柄无效。
        closedir(d);
        delete h;
        errno = ENOENT;
        return INVALID_HANDLE_VALUE;
    }
    EmitPending(h, lpFindFileData);   // 填 fd 并把 pendingOK 清零
    return (HANDLE)h;
}

BOOL FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    ShimHandle* h = (ShimHandle*)hFindFile;
    if (!h || h->kind != HandleKind::Find || !lpFindFileData) return FALSE;
    if (!ReadOneEntry(h)) return FALSE;   // 目录已尽
    EmitPending(h, lpFindFileData);
    return TRUE;
}

BOOL FindClose(HANDLE hFindFile) {
    ShimHandle* h = (ShimHandle*)hFindFile;
    if (!h || h->kind != HandleKind::Find) return FALSE;
    if (h->dir) closedir((DIR*)h->dir);
    delete h;
    return TRUE;
}

// ------------------------------------------------------------
//  GetModuleFileNameW：当前可执行文件绝对路径
//  Store.cpp:697 用它取 exe 路径（供 AutoStart 写入命令行）。
// ------------------------------------------------------------
DWORD GetModuleFileNameW(HMODULE /*hModule*/, LPWSTR lpFilename, DWORD nSize) {
    if (!lpFilename || nSize == 0) return 0;
    char buf[PROC_PIDPATHINFO_MAXSIZE] = {0};
    uint32_t need = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &need) != 0) {
        std::vector<char> big(need + 1, 0);
        if (_NSGetExecutablePath(big.data(), &need) != 0) return 0;
        memcpy(buf, big.data(), sizeof(buf) < need ? sizeof(buf) : need);
    }
    std::wstring w = pb::U2W(std::string(buf));
    DWORD n = (DWORD)w.size();
    if (n >= nSize) n = nSize - 1;      // Win32 语义：截断并 null 结尾
    memcpy(lpFilename, w.c_str(), n * sizeof(wchar_t));
    lpFilename[n] = L'\0';
    return n;
}

// ------------------------------------------------------------
//  _dupenv_s：读环境变量（Store.cpp:224 用于 PB_SAMPLE_DIR）
//  返回 0 成功。*pBuffer 由调用方 free()（Store.cpp 注释已说明）。
// ------------------------------------------------------------
int _dupenv_s(char** pBuffer, size_t* pNumberOfElements, const char* varName) {
    if (pBuffer) *pBuffer = nullptr;
    if (pNumberOfElements) *pNumberOfElements = 0;
    if (!varName) return EINVAL;
    const char* v = getenv(varName);
    if (!v) return 0;                    // 变量不存在也算成功，*pBuffer 为 null
    size_t len = strlen(v) + 1;
    char* buf = (char*)malloc(len);
    if (!buf) return ENOMEM;
    memcpy(buf, v, len);
    if (pBuffer) *pBuffer = buf;
    if (pNumberOfElements) *pNumberOfElements = len;
    return 0;
}

// ------------------------------------------------------------
//  SHGetFolderPathW：CSIDL_LOCAL_APPDATA → ~/Library/Application Support
//  Store.cpp:75 用 SUCCEEDED() 判定，成功后在返回值尾部拼 L"\\PaintingBuddy"。
//  故这里返回的路径**末尾不带分隔符**，让 Store.cpp 自己拼。
// ------------------------------------------------------------
HRESULT SHGetFolderPathW(HWND /*hwnd*/, int csidl, HANDLE /*hToken*/,
                         DWORD /*dwFlags*/, LPWSTR pszPath) {
    if (!pszPath) return E_INVALIDARG;
    if (csidl != CSIDL_LOCAL_APPDATA) return E_INVALIDARG;   // 只支持这一个

    @autoreleasepool {
        NSString* home = NSHomeDirectory();
        NSString* base = [home stringByAppendingPathComponent:@"Library/Application Support"];
        const char* utf8 = [base UTF8String];
        if (!utf8) return E_FAIL;
        std::wstring w = pb::U2W(std::string(utf8));
        if (w.size() >= MAX_PATH) return E_FAIL;
        memcpy(pszPath, w.c_str(), w.size() * sizeof(wchar_t));
        pszPath[w.size()] = L'\0';
    }
    return S_OK;
}

// ------------------------------------------------------------
//  _wfsopen / _wfopen_s：宽字符路径的 fopen
//  Store.cpp:265 的 LogF 用 _wfopen_s，:397 的 EventWriter 用 _wfsopen。
//  POSIX 的 fopen 默认允许并发读写，故 _SH_DENYNO 语义天然满足（见 share.h）。
// ------------------------------------------------------------
FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int /*shflag*/) {
    if (!path || !mode) { errno = EINVAL; return nullptr; }
    std::string p = NormPath(path);
    // 宽字符 mode → 窄字符（mode 只可能是 "ab"/"rb"/"wb" 这类 ASCII）
    std::string m = pb::W2U(std::wstring(mode));
    if (p.empty() || m.empty()) { errno = EINVAL; return nullptr; }
    return fopen(p.c_str(), m.c_str());
}

int _wfopen_s(FILE** pFile, const wchar_t* path, const wchar_t* mode) {
    if (!pFile) return EINVAL;
    *pFile = _wfsopen(path, mode, _SH_DENYNO);
    return (*pFile) ? 0 : errno;
}
