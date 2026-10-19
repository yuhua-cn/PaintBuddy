# ============================================================
#  绘画伴侣 · macOS 端构建工程
#
#  设计原则：src/ 下的 Windows 端源码**零修改**参与编译。
#  靠 mac/compat 的头文件垫片拦截 <windows.h> / <d2d1.h> /
#  <dwrite.h> / <wincodec.h>，把类型与函数映射到 Darwin API。
#
#  Windows 端构建走根目录的 build*.bat，与本文件完全无关。
# ============================================================
cmake_minimum_required(VERSION 3.20)

project(PaintingBuddyMac
        VERSION 0.1.0
        DESCRIPTION "绘画伴侣 macOS 端"
        LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# 最低 macOS 13.0：
#   · CSP Mac 版要求 13+
#   · ScreenCaptureKit 要求 12.3+
#   取交集。见 docs/PORTING.md。
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "最低系统版本")

if(NOT APPLE)
    message(FATAL_ERROR
        "本工程只能在 macOS 上构建。Windows 端请使用根目录的 build.bat。")
endif()

# ------------------------------------------------------------
#  路径
# ------------------------------------------------------------
set(PB_ROOT   "${CMAKE_CURRENT_SOURCE_DIR}/..")       # 项目根（绘画伴侣/）
set(PB_SRC    "${PB_ROOT}/src")                        # Windows 端源码（只读复用）
set(PB_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/compat")    # 头文件垫片
set(PB_PLATFORM "${CMAKE_CURRENT_SOURCE_DIR}/platform")# macOS 原生实现
set(PB_APPKIT "${CMAKE_CURRENT_SOURCE_DIR}/appkit")    # AppKit 宿主
set(PB_TOOLS  "${CMAKE_CURRENT_SOURCE_DIR}/tools")     # 无头验证工具

# ⚠️ compat 必须排在 src 之前：Common.h 的 #include <windows.h>
#    与 Ui.h 的 #include <d2d1.h> 靠这个顺序命中垫片而非真实 SDK 头。
set(PB_INCLUDE_DIRS
    "${PB_COMPAT}"
    "${PB_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}")

# ------------------------------------------------------------
#  编译选项
# ------------------------------------------------------------
# -fms-extensions：compat/windows.h 的 LARGE_INTEGER / ULARGE_INTEGER 用
#   「匿名 struct 套在 union 里」复刻 Win32 布局（Common.h 既访问 .QuadPart
#   又访问 .LowPart/.HighPart）。这是 MSVC/GCC 扩展，标准 C++ 不允许。
# -Wno-nullability-completeness：ObjC 头的 nullability 注解不全时不必刷屏。
set(PB_COMMON_FLAGS
    -fms-extensions
    -Wno-nullability-completeness
    -Wno-unknown-pragmas)   # Ui.h 的 #pragma comment(lib, ...) 在 clang 下无效但无害

# Windows 端专属、macOS 不参与编译的文件（由 platform/*.mm 重写替代）：
#   Store.cpp      → Store_mac.mm      文件 I/O 与注册表（Mac 改 LaunchAgent）
#   Config.cpp     → 保留原文件编译，Mac 目标软件映射见 Config_mac.mm
#   Ui.cpp         → Ui_mac.mm         D2D/DWrite → Core Graphics/Core Text
#   Capture.cpp    → Capture_mac.mm    低级钩子 → CGEventTap
#   PenInput.cpp   → PenInput_mac.mm   Raw Input+HID 报文 → CGEvent tablet
#   MainWindow.cpp → appkit/*.mm       Win32 消息循环 → AppKit
#   Recorder.cpp   → Recorder_mac.mm   WGC → ScreenCaptureKit
#   Video.cpp      → Video_mac.mm      Media Foundation → AVAssetWriter
#   main.cpp       → appkit/main.mm    DPI 感知/COM 初始化 → NSApplication
# 完全不移植：PenInject.* / PenOverrides.* / PenInputDev.h / app.rc / resource.h
#   （Mac 端不注入事件、不解析 HID 报文、不用 rc 资源）

# ------------------------------------------------------------
#  原样复用的 Windows 端源码
#
#  经逐文件核实（检索 Win32 / D2D / 文件 I/O 符号）：
#    Analyzer.cpp  只 include Analyzer.h + <algorithm>/<cmath>/<numeric>
#    Json.cpp      只 include Json.h + <cmath>
#    Charts.cpp    只经 Painter 抽象与 Ui.h 交互，无 D2D 直接调用
#    Config.cpp    include Store.h，调用 ReadWholeFile / WriteWholeFileAtomic / LogF
#    Store.cpp     用 Win32 文件 API + 注册表，但由 compat 垫片全部覆盖
#  故五者均可**零修改编译**。
#
#  ⭐ 为什么 Store.cpp 也要原样复用（这是本方案最关键的决定）：
#    SerializeEvent / ParseEventLine / LoadDay / DescribeKey 这几个
#    **数据格式核心函数**都在 Store.cpp 里。它们决定了 jsonl 的字段名、
#    浮点精度（%.9g）、布尔字段的省略规则、pts 数组的元素顺序。
#    若在 macOS 端另写一份实现，任何一处细节漂移都会导致
#    「Mac 录的数据 Windows 读不出」—— 恰好摧毁整个移植的核心价值。
#    因此改为 shim 掉它用到的 Win32 文件与注册表 API，让这一份代码
#    在两端同时生效，格式一致性由「同一份源码」保证，而非靠人工对齐。
#
#    Store.cpp 的 Win32 依赖集中在三块，全部已由垫片覆盖：
#      · 文件读写  CreateFileW/ReadFile/WriteFile/GetFileSizeEx/
#                  MoveFileExW/DeleteFileW/GetFileAttributesExW
#                  → shims/FileShim.mm
#      · 目录遍历  FindFirstFileW/FindNextFileW/CreateDirectoryW
#                  → shims/FileShim.mm
#      · 开机自启  RegOpenKeyExW/RegSetValueExW/RegQueryValueExW/
#                  RegDeleteValueW（HKCU Run 键）
#                  → shims/RegShim.mm（映射为 LaunchAgent plist）
# ------------------------------------------------------------
set(PB_REUSED_SOURCES
    "${PB_SRC}/Analyzer.cpp"
    "${PB_SRC}/Charts.cpp"
    "${PB_SRC}/Json.cpp"
    "${PB_SRC}/Config.cpp"
    "${PB_SRC}/Store.cpp")

# ------------------------------------------------------------
#  macOS 端实现：按阶段逐步落地
#
#  compat/shims/*.mm 是**编译 Store.cpp 等复用源码的前提**，不是可选项：
#  它们提供 CreateFileW / RegOpenKeyExW / _wfsopen 等符号的实现，
#  缺任何一个都会在链接期报 undefined symbols。
#
#  platform/*.mm 与 appkit/*.mm 用 EXISTS 守卫，使 M0 阶段
#  （这些文件还没写）也能构建成功，后续阶段把文件放进对应目录
#  即自动纳入，无需再改本文件。
# ------------------------------------------------------------
set(PB_COMPAT_SOURCES
    "${PB_COMPAT}/shims/Win32Shim.mm"     # 时钟 / 编码转换 / 进程名 / DPI
    "${PB_COMPAT}/shims/FileShim.mm"      # 文件 / 目录 / 路径 / SHGetFolderPathW
    "${PB_COMPAT}/shims/RegShim.mm")      # 注册表 → LaunchAgent

set(PB_PLATFORM_SOURCES ${PB_COMPAT_SOURCES})

foreach(_f
        Config_mac.mm
        Ui_mac.mm
        Capture_mac.mm
        PenInput_mac.mm
        FrontApp_mac.mm
        Permission_mac.mm
        Recorder_mac.mm
        Video_mac.mm)
    if(EXISTS "${PB_PLATFORM}/${_f}")
        list(APPEND PB_PLATFORM_SOURCES "${PB_PLATFORM}/${_f}")
    endif()
endforeach()

foreach(_f
        MainController.mm
        StatusItem.mm
        DisplayLink.mm
        main.mm)
    if(EXISTS "${PB_APPKIT}/${_f}")
        list(APPEND PB_PLATFORM_SOURCES "${PB_APPKIT}/${_f}")
    endif()
endforeach()

# ------------------------------------------------------------
#  系统框架
#
#  对应 Windows 端 build.bat 链接的 21 个库（user32/gdi32/d2d1/dwrite/
#  windowscodecs/mf/mfplat/mfreadwrite/hid/…）在 macOS 上的等价物。
#  即使某些框架要等 M4/M5 才用到，这里一次列全，避免后续反复改。
#
#  ⚠️ 必须用 find_library 把框架名解析成完整路径，不能把裸框架名直接
#     传给 target_link_libraries：那样 CMake 生成 -lCocoa，ld 在库搜索
#     路径里找 libCocoa.* 找不到，报 "ld: library 'Cocoa' not found"。
#     （第八轮 CI 实测：pbcore 静态库编译+链接全部通过，只在链接
#      pbcheck_data 可执行文件时因框架链接方式错误而失败。）
#     find_library 定位到 /System/Library/Frameworks/Cocoa.framework，
#     CMake 链接时正确转成 -framework Cocoa。
# ------------------------------------------------------------
set(PB_FRAMEWORK_NAMES
    Cocoa               # Foundation + AppKit 的 umbrella 框架
    CoreGraphics        # CGEventTap、tablet 事件、2D 绘制
    CoreText            # 文字排版（替代 DirectWrite）
    ImageIO             # PNG 导出（替代 WIC）
    ApplicationServices # AXIsProcessTrusted 等
    IOKit               # 设备信息
    AVFoundation        # AVAssetWriter（替代 Media Foundation）
    ScreenCaptureKit    # SCStream（替代 Windows.Graphics.Capture）
    CoreMedia
    CoreVideo
    UniformTypeIdentifiers)

set(PB_FRAMEWORKS "")
foreach(_fw ${PB_FRAMEWORK_NAMES})
    find_library(_fwpath_${_fw} ${_fw})
    if(_fwpath_${_fw})
        list(APPEND PB_FRAMEWORKS "${_fwpath_${_fw}}")
    else()
        message(WARNING "框架未找到: ${_fw}（链接可能失败）")
    endif()
endforeach()

# ------------------------------------------------------------
#  pbcore：核心静态库（M0 的验收产物）
#
#  M0 验收标准：Analyzer.cpp / Charts.cpp / Json.cpp / Config.cpp /
#  Common.h **未经任何修改**即编译通过。
# ------------------------------------------------------------
add_library(pbcore STATIC
    ${PB_REUSED_SOURCES}
    ${PB_PLATFORM_SOURCES})

target_include_directories(pbcore PUBLIC ${PB_INCLUDE_DIRS})
target_compile_options(pbcore PRIVATE ${PB_COMMON_FLAGS})

# .mm 需要 ARC；纯 .cpp 不受影响。
# 注意：Windows 端源码是 .cpp，用 ObjC++ 的 ARC 语义编译它们是安全的
# （不含 ObjC 代码），但为避免歧义，对复用的 .cpp 显式关闭 ARC 相关选项。
set_source_files_properties(${PB_REUSED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fno-objc-arc")

# ------------------------------------------------------------
#  无头验证工具（CI 可跑，不需要 TCC 权限）
#
#  pbcheck_data   ：读 jsonl → 输出 Analyzer 结果 JSON（M1 验收）
#  pbcheck_render ：离屏渲染甘特图/热力图 → PNG（M2 验收）
#  两者都必须调用 OverrideDataDir() 把数据目录重定向到隔离位置，
#  绝不能碰真实数据目录（见 Common.h:351-356 的说明）。
# ------------------------------------------------------------
if(EXISTS "${PB_TOOLS}/pbcheck_data.cpp")
    add_executable(pbcheck_data "${PB_TOOLS}/pbcheck_data.cpp")
    target_link_libraries(pbcheck_data PRIVATE pbcore ${PB_FRAMEWORKS})
    target_compile_options(pbcheck_data PRIVATE ${PB_COMMON_FLAGS})
endif()

if(EXISTS "${PB_TOOLS}/pbcheck_render.cpp")
    add_executable(pbcheck_render "${PB_TOOLS}/pbcheck_render.cpp")
    target_link_libraries(pbcheck_render PRIVATE pbcore ${PB_FRAMEWORKS})
    target_compile_options(pbcheck_render PRIVATE ${PB_COMMON_FLAGS})
endif()

# ------------------------------------------------------------
#  主程序 .app（M3 起才有内容）
# ------------------------------------------------------------
if(EXISTS "${PB_APPKIT}/main.mm")
    add_executable(PaintingBuddy MACOSX_BUNDLE ${PB_PLATFORM_SOURCES})
    target_include_directories(PaintingBuddy PRIVATE ${PB_INCLUDE_DIRS})
    target_compile_options(PaintingBuddy PRIVATE ${PB_COMMON_FLAGS})
    target_link_libraries(PaintingBuddy PRIVATE pbcore ${PB_FRAMEWORKS})

    # Info.plist：含三项 TCC 用途描述文案。
    # 未签名或未授权时 CGEventTap 会「创建成功但收不到任何事件」，
    # 症状与代码 bug 完全一致 —— 这是 Mac 端最易误判的陷阱。
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
        set_target_properties(PaintingBuddy PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
    endif()
endif()

# ------------------------------------------------------------
#  构建摘要：便于在 CI 日志里一眼确认哪些阶段已落地
# ------------------------------------------------------------
message(STATUS "========== 绘画伴侣 macOS 端 ==========")
message(STATUS "项目根目录   : ${PB_ROOT}")
message(STATUS "复用源码     : ${PB_REUSED_SOURCES}")
message(STATUS "平台实现     : ${PB_PLATFORM_SOURCES}")
message(STATUS "最低系统版本 : ${CMAKE_OSX_DEPLOYMENT_TARGET}")
message(STATUS "======================================")
