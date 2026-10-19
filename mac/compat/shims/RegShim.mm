// ============================================================
//  注册表 API → LaunchAgent 的 macOS 实现
//
//  Store.cpp:704-757 用注册表实现开机自启（写 HKCU\...\Run 键 + --tray）。
//  macOS 的对应机制是 ~/Library/LaunchAgents/<label>.plist。
//
//  映射模型：把「一个注册表值」映射成「一个 plist 文件」。
//    RegOpenKeyExW(HKEY_CURRENT_USER, Run子键, ...) → 返回哨兵句柄
//      · 请求写权限(KEY_SET_VALUE)：总是成功（真正的落地在 Set/Delete）
//      · 只请求读权限(KEY_QUERY_VALUE)：plist 不存在时返回
//        ERROR_FILE_NOT_FOUND，使 AutoStartEnabled 正确报告「未开启」
//    RegSetValueExW  → 生成 plist 并原子写入（含 --tray 参数）
//    RegQueryValueExW→ 读回 plist 里记录的程序路径，按 REG_SZ 宽字符返回
//    RegDeleteValueW → 删除 plist（幂等）
//
//  ⚠️ Store.cpp:763-767 的注释指出「不能只比较值存不存在」：exe 挪过
//     位置后值依然存在但指向旧路径，那样开机会启动一个不存在的文件，
//     而按存在性判断会认为「已一致」直接跳过，自愈彻底失效。
//     所以 AutoStartSync 会**比对内容是否为当前 exe**。
//     这意味着 RegQueryValueExW 必须如实返回 plist 里记录的路径字符串，
//     不能只回「存在/不存在」—— 否则自愈逻辑失效，.app 移动后自启会
//     指向一个已不存在的路径且永远修不好。
//
//  ⚠️ RegSetValueExW 收到的 lpData 是**宽字符**命令行字符串，形如
//     "…/PaintingBuddy.app/Contents/MacOS/PaintingBuddy" --tray
//     （Store.cpp:722-725 拼的）。这里要把它拆成「程序路径 + 参数」
//     写进 ProgramArguments 数组 —— LaunchAgent 不接受一整条命令行字符串。
// ============================================================

#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "windows.h"
// pb::W2U / U2W：命令行字符串在宽字符与 UTF-8 之间转换
#include "Common.h"

namespace {

const char* const kLaunchAgentLabel = "com.paintingbuddy.autostart";

// RegOpenKeyExW 的哨兵句柄。注册表键在 macOS 上没有对应实体，
// 但 Store.cpp 拿到句柄后会传给 Reg{Set,Query,Delete}ValueExW，
// 那些实现根本不读句柄内容（都只操作同一个固定 plist），
// 故给一个非空常量指针即可 —— 非空是必须的，Store.cpp 用
// `r != ERROR_SUCCESS` 判定失败，句柄为空会被上层误判。
char g_regSentinel = 0;

std::string NsToUtf8(NSString* s) {
    if (!s) return std::string();
    const char* c = [s UTF8String];
    return c ? std::string(c) : std::string();
}

std::string LaunchAgentPath() {
    @autoreleasepool {
        NSString* p = [NSHomeDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"Library/LaunchAgents/%s.plist",
                                       kLaunchAgentLabel]];
        return NsToUtf8(p);
    }
}

bool PlistExists() {
    std::string p = LaunchAgentPath();
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// ------------------------------------------------------------
//  宽字符命令行 → 程序路径 + 参数数组
//
//  输入形如：  "<路径>" --tray      （带引号，Store.cpp:722 拼的）
//  输出：      program = <路径>（去引号）
//              args    = ["<路径>", "--tray"]
//
//  LaunchAgent 的 ProgramArguments[0] 必须是可执行文件本身，
//  后续元素是参数。故 args[0] 要重复一遍程序路径。
// ------------------------------------------------------------
void SplitCommandLine(const std::wstring& wcmd, std::string& program,
                      std::vector<std::string>& args) {
    program.clear();
    args.clear();

    std::string cmd = pb::W2U(wcmd);
    size_t i = 0;
    std::vector<std::string> tokens;

    // 简易分词：支持双引号包裹（路径含空格时 Store.cpp 会加引号）
    while (i < cmd.size()) {
        while (i < cmd.size() && cmd[i] == ' ') ++i;
        if (i >= cmd.size()) break;
        std::string tok;
        if (cmd[i] == '"') {
            ++i;
            while (i < cmd.size() && cmd[i] != '"') tok += cmd[i++];
            if (i < cmd.size()) ++i;           // 跳过收尾引号
        } else {
            while (i < cmd.size() && cmd[i] != ' ') tok += cmd[i++];
        }
        if (!tok.empty()) tokens.push_back(tok);
    }

    if (tokens.empty()) return;
    program = tokens[0];
    args = tokens;                              // args[0] 即程序路径
}

bool WriteLaunchAgentPlist(const std::wstring& wcmd) {
    std::string program;
    std::vector<std::string> args;
    SplitCommandLine(wcmd, program, args);
    if (program.empty()) return false;

    @autoreleasepool {
        NSString* plistPath = [NSString stringWithUTF8String:LaunchAgentPath().c_str()];

        // 父目录 ~/Library/LaunchAgents 可能不存在，先建。
        NSString* dir = [plistPath stringByDeletingLastPathComponent];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil error:nil];

        NSMutableArray<NSString*>* progArgs = [NSMutableArray array];
        for (const auto& a : args)
            [progArgs addObject:[NSString stringWithUTF8String:a.c_str()]];

        NSDictionary* root = @{
            @"Label"           : @(kLaunchAgentLabel),
            @"ProgramArguments": progArgs,
            @"RunAtLoad"       : @YES,   // 登录后立即启动
            @"KeepAlive"       : @NO,    // 崩溃不自动重启（避免刷屏）
            @"ProcessType"     : @"Background",
        };

        NSError* err = nil;
        // 用 NSPropertyListSerialization 而非手拼 XML：program 路径可能含
        // 需要转义的字符（& < > "），手拼极易产出损坏的 plist。
        NSData* d = [NSPropertyListSerialization dataWithPropertyList:root
                                format:NSPropertyListXMLFormat_v1_0
                                options:0 error:&err];
        if (!d) return false;

        // 原子写：先 .tmp 再 rename。plist 若写坏一半，launchd 会拒绝加载，
        // 表现为「设了开机自启但下次登录没启动」，且没有任何提示。
        NSString* tmp = [plistPath stringByAppendingString:@".tmp"];
        if (![d writeToFile:tmp atomically:YES]) return false;

        NSFileManager* fm = [NSFileManager defaultManager];
        [fm removeItemAtPath:plistPath error:nil];      // 幂等：不存在也无妨
        if (![fm moveItemAtPath:tmp toPath:plistPath error:&err]) {
            [fm removeItemAtPath:tmp error:nil];
            return false;
        }
    }
    return true;
}

// 读回 plist 里记录的命令行（还原成 Windows 端的 "<路径>" --tray 形式），
// 供 AutoStartSync 比对是否为当前 exe。读不到返回空串。
std::wstring ReadLaunchAgentCommand() {
    if (!PlistExists()) return std::wstring();

    @autoreleasepool {
        NSString* plistPath = [NSString stringWithUTF8String:LaunchAgentPath().c_str()];
        NSData* d = [NSData dataWithContentsOfFile:plistPath];
        if (!d) return std::wstring();

        NSError* err = nil;
        id obj = [NSPropertyListSerialization propertyListWithData:d
                        options:NSPropertyListImmutable format:nil error:&err];
        if (![obj isKindOfClass:[NSDictionary class]]) return std::wstring();

        NSArray* progArgs = ((NSDictionary*)obj)[@"ProgramArguments"];
        if (![progArgs isKindOfClass:[NSArray class]] || progArgs.count == 0)
            return std::wstring();

        // 还原成 Windows 端的形态：首项加引号，其余空格分隔。
        // 必须与 Store.cpp:722 的拼法一致，否则 AutoStartSync 的内容比对
        // 会永远判定「不一致」，导致每次启动都重写 plist（无害但白耗）。
        NSMutableString* cmd = [NSMutableString string];
        [cmd appendFormat:@"\"%@\"", [progArgs objectAtIndex:0]];
        for (NSUInteger k = 1; k < progArgs.count; ++k) {
            [cmd appendFormat:@" %@", [progArgs objectAtIndex:k]];
        }
        return pb::U2W(NsToUtf8(cmd));
    }
}

} // anonymous namespace

// ------------------------------------------------------------
//  注册表 API
// ------------------------------------------------------------

LONG RegOpenKeyExW(HKEY hKey, LPCWSTR /*lpSubKey*/, DWORD /*ulOptions*/,
                   DWORD samDesired, HKEY* phkResult) {
    if (!phkResult) return ERROR_INVALID_PARAMETER;
    *phkResult = nullptr;
    if (hKey != HKEY_CURRENT_USER) return ERROR_FILE_NOT_FOUND;

    // 只读打开且 plist 不存在 → 如实报「没有」，让 AutoStartEnabled
    // 返回 false。写权限则总是成功（Set/Delete 自己处理不存在的情况）。
    bool wantWrite = (samDesired & (KEY_SET_VALUE | KEY_ALL_ACCESS)) != 0;
    if (!wantWrite && !PlistExists()) return ERROR_FILE_NOT_FOUND;

    *phkResult = (HKEY)&g_regSentinel;
    return ERROR_SUCCESS;
}

LONG RegQueryValueExW(HKEY hKey, LPCWSTR /*lpValueName*/, LPDWORD /*lpReserved*/,
                      LPDWORD lpType, BYTE* lpData, LPDWORD lpcbData) {
    if (hKey != (HKEY)&g_regSentinel) return ERROR_FILE_NOT_FOUND;

    std::wstring val = ReadLaunchAgentCommand();
    if (val.empty()) return ERROR_FILE_NOT_FOUND;

    if (lpType) *lpType = REG_SZ;

    // 需要的字节数：含终止 null（与 Win32 的 REG_SZ 一致）。
    // ⚠️ sizeof(wchar_t) 在 macOS 是 4 而非 2 —— 这里必须用 sizeof(wchar_t)
    //    而不是硬编码 2，否则调用方按 Windows 的宽度假定分配缓冲会溢出。
    DWORD need = (DWORD)((val.size() + 1) * sizeof(wchar_t));

    if (!lpData || !lpcbData || *lpcbData < need) {
        // Win32 语义：缓冲不足时回填所需大小并返回 ERROR_MORE_DATA。
        if (lpcbData) *lpcbData = need;
        return (lpData && lpcbData) ? ERROR_MORE_DATA : ERROR_SUCCESS;
    }

    memcpy(lpData, val.c_str(), need);
    *lpcbData = need;
    return ERROR_SUCCESS;
}

LONG RegSetValueExW(HKEY hKey, LPCWSTR /*lpValueName*/, DWORD /*Reserved*/,
                    DWORD dwType, const BYTE* lpData, DWORD cbData) {
    if (hKey != (HKEY)&g_regSentinel) return ERROR_FILE_NOT_FOUND;
    if (dwType != REG_SZ || !lpData || cbData == 0) return ERROR_INVALID_PARAMETER;

    // lpData 是宽字符串，cbData 是字节数。按 macOS 的 wchar_t 宽度还原
    // 成 std::wstring（不能假定 2 字节/字符）。
    size_t nchars = (size_t)cbData / sizeof(wchar_t);
    const wchar_t* w = (const wchar_t*)lpData;
    if (nchars > 0 && w[nchars - 1] == L'\0') --nchars;   // 去掉终止 null
    std::wstring cmd(w, nchars);

    return WriteLaunchAgentPlist(cmd) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

LONG RegDeleteValueW(HKEY hKey, LPCWSTR /*lpValueName*/) {
    if (hKey != (HKEY)&g_regSentinel) return ERROR_FILE_NOT_FOUND;

    std::string p = LaunchAgentPath();
    if (unlink(p.c_str()) != 0) {
        // 删除不存在的值也算成功（Store.cpp:716 明确依赖此幂等语义：
        // `ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)`）
        return (errno == ENOENT) ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

LONG RegCloseKey(HKEY /*hKey*/) {
    // 哨兵不是资源，无需释放。
    return ERROR_SUCCESS;
}
