#pragma once
// ============================================================
//  垫片内部：带类型标签的句柄
//
//  为什么需要它：Win32 的 HANDLE 是个统一的不透明指针，
//  CloseHandle 可以关闭进程句柄、文件句柄、查找句柄。
//  本垫片用同一个 HANDLE 类型承载三种完全不同的东西：
//    · OpenProcess        → 进程（内部存 pid，用 proc_pidpath 查路径）
//    · CreateFileW        → 打开的文件（内部存 POSIX fd）
//    · FindFirstFileW     → 目录遍历（内部存 DIR*）
//  若不打标签，CloseHandle 无法判断该 close(fd) 还是 closedir(DIR*)
//  还是 delete 进程对象 —— 错误的分支会导致 fd 泄漏或直接崩溃。
//
//  这是垫片内部头，不被 src/ 下任何文件 include。
// ============================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/types.h>

namespace pbshim {

enum class HandleKind : uint8_t {
    Invalid = 0,
    Process,      // pid
    File,         // POSIX fd
    Find,         // 目录遍历游标
};

struct Handle {
    HandleKind kind = HandleKind::Invalid;

    // Process
    pid_t      pid = 0;

    // File
    int        fd = -1;

    // Find
    void*      dir   = nullptr;   // DIR*
    // 遍历的目录路径。FindNextFileW 每读到一个条目都要拼
    // dirPath + "/" + name 再 stat 取属性，故必须存下来。
    std::string dirPath;
    bool       pendingOK = false;  // pending* 是否为待消费的有效条目
    char       pendingName[512] = {0};
    bool       pendingIsDir = false;
    long long  pendingSize  = 0;
};

// GetDC 返回的哨兵不是 Handle（Common.h 的 WindowDpi 用 ReleaseDC
// 而非 CloseHandle 释放它），故 CloseHandle 遇到非 Handle 指针时
// 必须能识别并安全忽略。用一个全局静态对象充当哨兵，
// 指针相等性判断即可区分。
extern Handle g_dcSentinelHandle;

// 按句柄类型释放（close(fd) / closedir(DIR*) / delete 进程对象）。
// 实现在 FileShim.mm —— 它同时掌握三种句柄的内部结构。
// Win32Shim.mm 的 CloseHandle 只需转调它，避免两处各写一遍释放逻辑
// 而漏掉某种句柄类型（漏掉就是 fd 泄漏或崩溃）。
bool ReleaseHandle(Handle* h);

} // namespace pbshim
