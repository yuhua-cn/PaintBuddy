#pragma once
// ============================================================
//  <io.h> 垫片
//
//  Store.cpp:5 include 它。经检索，Store.cpp 并未直接调用其中的
//  _waccess / _wunlink / _commit 等函数（文件删除走 DeleteFileW，
//  刷盘走 fflush）。这里提供一个空头文件让 include 不报错即可。
//
//  同 direct.h 的理由：只提供被实际调用的符号。
// ============================================================

#include <unistd.h>   // access / unlink 的 POSIX 版本
