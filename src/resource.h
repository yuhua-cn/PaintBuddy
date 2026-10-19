#ifndef PB_RESOURCE_H
#define PB_RESOURCE_H

// ============================================================
//  Resource IDs shared by src\app.rc and the C++ sources.
//
//  KEEP THIS FILE ASCII-ONLY  --  this is a hard requirement,
//  not a style preference.
//
//  rc.exe preprocesses this header with the system ANSI codepage
//  (936/GBK on this machine). UTF-8 Chinese bytes misread as GBK
//  can end in 0x5C (backslash), which becomes a line-continuation
//  and silently swallows the following #define. That is exactly what
//  happened first time round: rc emitted
//      Writing GROUP_ICON:IDI_APP
//  instead of
//      Writing GROUP_ICON:101
//  so the resource got a STRING name, FindResourceW(101) failed with
//  error 1813, and the running app fell back to MakeAppIcon().
//  Explorer still showed the right icon (it takes the first
//  RT_GROUP_ICON regardless of name), so the bug is invisible on the
//  desktop but the titlebar/tray/taskbar button keep the old art.
//
//  Chinese explanations belong in the .cpp sources, which cl.exe
//  compiles with /utf-8 and handles correctly.
//
//  Also: traditional include guard instead of "#pragma once", and no
//  "#include <windows.h>" -- rc's preprocessor is a subset of cl's and
//  neither needs nor handles them well here.
// ============================================================

// Application icon. MUST stay the lowest-numbered icon resource in the
// exe: Explorer takes the first RT_GROUP_ICON as the file icon, which is
// what the desktop shortcut, Start menu entry, taskbar pin and the
// file-properties dialog all display.
//
// Master artwork: res\app.ico, ten sizes (16/20/24/32/40/48/64/96/128/256),
// 256 stored as PNG. See the icon spec document, sections 2 and 10.
#define IDI_APP 101

#endif // PB_RESOURCE_H
