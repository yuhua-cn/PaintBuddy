#pragma once
// ============================================================
//  <shlobj.h> 垫片
//
//  Store.cpp:9 include 它，为的是 SHGetFolderPathW 与 CSIDL_LOCAL_APPDATA
//  （定位 %LOCALAPPDATA%，即数据根目录的父目录）。
//
//  macOS 对应位置是 ~/Library/Application Support —— 这是 macOS 用户级
//  应用数据的标准位置（Apple 文件系统编程指南）。
//  Store.cpp:76 会在返回值后拼接 L"\\PaintingBuddy"，故本函数返回的
//  路径**末尾不带分隔符**。
// ============================================================

#include "windows.h"

// CSIDL 常量：只定义 Store.cpp 用到的这一个。
// 其余 CSIDL_* 不提供 —— 用到了再说，避免垫片无谓膨胀。
#define CSIDL_LOCAL_APPDATA 0x001c

// SHGetFolderPathW 的返回值判定用 SUCCEEDED(hr)，
// 该宏在 windows.h 中定义（HRESULT >= 0）。

// 成功返回 S_OK 并把路径写入 pszPath（宽字符，含终止 null）。
// 失败返回负的 HRESULT。pszPath 容量按 Win32 约定为 MAX_PATH 个 wchar_t。
//
// ⚠️ 刻意不用 extern "C"：定义在 shims/FileShim.mm 的全局 C++ 作用域。
//    声明用 C 链接而定义用 C++ 链接会导致链接期 undefined symbol。
HRESULT SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken,
                         DWORD dwFlags, LPWSTR pszPath);
