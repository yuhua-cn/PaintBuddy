#pragma once
// ============================================================
//  <share.h> 垫片
//
//  Store.cpp:6 include 它，为的是 _wfsopen / _fsopen 与 _SH_DENYNO。
//
//  ⚠️ _SH_DENYNO 在 Windows 上不是可有可无的装饰：Store.cpp:383-395
//  的注释记录了实测结论 —— 该 CRT 的 fopen 追加写会独占到「读取端
//  无论请求哪种共享模式都拿到 err=32 共享冲突」，于是程序运行期间
//  当天的 .jsonl 一条也读不到，分析结果恒为空（症状：「怎么操作两个
//  图表都不更新」）。_SH_DENYNO 明确允许他人并发读，才能边写边分析。
//
//  POSIX 的 fopen 默认就允许并发读写，没有独占语义 —— 所以这里
//  把 _SH_DENYNO 定义为 0（接受并忽略）在语义上是**等价的**，
//  不是功能缺失。这条等价关系是整个「录制中实时重算」功能的前提，
//  改动前请先读上面那段注释。
// ============================================================

#include <cstdio>
#include <wchar.h>

// MSVC 的共享模式常量。POSIX 无对应机制，全部接受并忽略。
#define _SH_DENYNO  0x40
#define _SH_DENYRW  0x10
#define _SH_DENYWR  0x20
#define _SH_DENYRD  0x30
#define _SH_COMPAT  0x00

#ifdef __cplusplus
extern "C" {
#endif

// _wfsopen(path, mode, shflag)：宽字符路径 + 共享模式。
// 实现见 shims/FileShim.mm。内部把 UTF-32 路径转 UTF-8、
// 反斜杠归一化为正斜杠，再调 fopen；shflag 忽略。
FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int shflag);

// _wfopen_s(FILE**, path, mode)：MSVC 安全函数族。
// 返回 0 表示成功（errno_t 语义），失败返回非 0 且不修改 *pFile。
// Store.cpp:265 的 LogF 用它写 pb.log。
int _wfopen_s(FILE** pFile, const wchar_t* path, const wchar_t* mode);

#ifdef __cplusplus
}
#endif
