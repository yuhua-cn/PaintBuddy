# ============================================================
#  绘画伴侣 · macOS 移植记录
#
#  本文件记录移植的架构决策与「为什么这样做」，不是构建教程
#  （构建见 BUILD.md，权限见 PERMISSIONS.md）。
# ============================================================

## 零、硬约束

**`src/` 下的 Windows 端源码一律不修改。** macOS 端全部代码位于 `mac/`，
自包含。根目录唯一新增的 `.github/workflows/` 是 GitHub Actions 的强制
位置要求（无法放进 `mac/`）。

Windows 端构建走根目录的 `build*.bat`，完全不涉及 `mac/` 目录，两者互不干扰。

---

## 一、为什么能零修改复用：compat 垫片

`Common.h` 直接 `#include <windows.h>`，`Ui.h` 直接 `#include <d2d1.h>` /
`<dwrite.h>` / `<wincodec.h>`。CMake 把 `mac/compat` 放在 include 搜索路径
**最前面**，让这些 include 命中垫片而非真实 Windows SDK 头。

垫片的边界（刻意保守）：

- **只定义「原样复用的那 4 个 .cpp + 3 个 .h」实际用到的符号**，不做 Win32 仿真。
- `CreateFileW` / `RegisterRawInputDevices` / `SetWindowsHookExW` / `RegSetValueExW`
  等 API **一律不提供**。提供了也无人调用，只会掩盖「某个本该重写的文件被误编进来」
  这类错误——让它在链接期报 undefined symbol 反而更容易发现问题。

### 类型形状必须与 Windows SDK 一致

`Charts.cpp` 直接访问 `D2D1_COLOR_F` 的 `.r/.g/.b/.a` 与 `D2D1_RECT_F` 的
`.left/.top/.right/.bottom`，`Charts.h` 的公开接口签名（`GanttView::Draw` /
`ZoomAt` / `OnWheel` …）也以这两个类型为参数。字段名或顺序错一个就编译失败。

`LARGE_INTEGER` / `ULARGE_INTEGER` 用「匿名 struct 套在 union 里」复刻 Win32
布局——`Common.h::Clock` 访问 `.QuadPart`，而 `Common.h::FileTimeToUs` 访问
`.LowPart`/`.HighPart`，三种用法并存。这是 MSVC/GCC 扩展，需 `-fms-extensions`。

---

## 二、三个决定方案的关键发现

### 发现 1：图表层已天然解耦（最大的一笔复用）

`Charts.cpp`(1102 行) 与 `Analyzer.cpp`(886 行) **零 Win32 调用**（已逐文件
检索核实：`Analyzer.cpp` 只 include `Analyzer.h` + `<algorithm>/<cmath>/<numeric>`；
`Json.cpp` 只 include `Json.h` + `<cmath>`）。所有绘制都经 `Painter`
(`Ui.h:155-314`) 这一抽象层，`ui_preview.cpp` 已能离屏渲染出 PNG，
证明绘制与窗口无关。

因此甘特图的 sqrt 压缩列宽、深浅暂停三行、打断通栏、刻度碰撞检测、
0.28s 滞回淡入，以及习惯页的 5×5 转移矩阵 / 提升度热力图 / 二阶条件转移
——**算法一行都不用改**，只需把 `Painter` 的实现从 D2D 换成 Core Graphics。

### 发现 2：Mac 上笔输入路线完全不同，且**更简单**

数位板在 Mac 上被 Wacom 驱动独占，`IOHIDManager` 枚举不到设备
（`IOHIDManagerOpen` 返回 `kIOReturnNotPermitted`）。正确路线是 Core
Graphics 的 tablet 事件——系统已把驱动数据翻译成结构化字段：

| 需求 | CGEventField |
|---|---|
| 压感 | `kCGEventTabletEventPointPressure`（已是 0..1） |
| 绝对坐标 | `kCGEventTabletEventPointX/Y` |
| 悬停 | `kCGEventTabletProximity` + `...InProximity` |
| 笔尖/橡皮/桶键 | `kCGEventTabletEventPointButtons`（bit 位） |
| 倾角/旋转/切向压力 | `kCGEventTabletEventTiltX/Y`、`Rotation`、`TangentialPressure` |
| 设备区分 | `kCGEventTabletEventDeviceID`、`VendorID`、`SystemTabletID` |

**含义**：Windows 端「Raw Input 注册 + HID 报文解析 + 设备描述符覆盖」
那一整套（`PenInputDev.h` 154 行、`PenOverrides.*` 430 行、`PenInject.*`
254 行）**全部无需移植**，共省下 838 行，也顺带绕开了 Windows 端踩过的
「漏注册 HID Usage 0x01」「解析成功但字段错位导致压感恒 0」这类坑。

### 发现 3：数据格式天然跨平台兼容

已核实真实样本 `build/sampledata/logs/20261017.jsonl`：时间戳是
**Unix epoch 微秒 int64**（不是 FILETIME），全链路 UTF-8，`exe` 仅存
进程名小写、`title` 纯文本，**无窗口句柄/PID 入数据**。

Mac 版可直接读取 Windows 录制的 jsonl 与 config.json。需适配的仅两处：
路径分隔符拼接、日切分用的本地时区逻辑。

### 发现 4：`Store.cpp` 也应原样复用（本方案最关键的决定）

原计划把 `Store.cpp`(790 行) 列为「必须重写」，**这是错的**。逐行核实后：
`SerializeEvent` / `ParseEventLine` / `LoadDay` / `DescribeKey` 这几个
**数据格式核心函数**都是纯字符串处理，Win32 依赖只集中在三块：

| 区块 | Win32 API | 垫片位置 |
|---|---|---|
| 文件读写 | `CreateFileW`/`ReadFile`/`WriteFile`/`GetFileSizeEx`/`MoveFileExW`/`DeleteFileW`/`GetFileAttributesExW` | `shims/FileShim.mm` |
| 目录遍历 | `FindFirstFileW`/`FindNextFileW`/`CreateDirectoryW` | `shims/FileShim.mm` |
| 开机自启 | `RegOpenKeyExW`/`RegSetValueExW`/`RegQueryValueExW`/`RegDeleteValueW` | `shims/RegShim.mm` |

**为什么必须复用而非重写**：这些函数决定了 jsonl 的字段名、浮点精度
（`Json.cpp:43` 的 `%.9g`）、布尔字段的省略规则、`pts` 数组的元素顺序。
若 macOS 端另写一份实现，任何一处细节漂移都会导致「Mac 录的数据 Windows
读不出」——**恰好摧毁整个移植的核心价值**。改为 shim 后，格式一致性由
「同一份源码」保证，而不是靠人工对齐两份实现。

代价是垫片要多覆盖约 15 个 Win32 符号。这个交换非常划算。

### ⚠️ 隐藏陷阱：路径分隔符必须归一化

`Store.cpp` 用 `L"\\"` 硬编码拼路径（`:76`、`:100`、`:380`），而 macOS 上
反斜杠**不是**分隔符，是合法的普通文件名字符。若原样传给 POSIX API，
`.../PaintingBuddy\logs` 会被当成「一个名字里含反斜杠的单个文件」，
于是目录永远建不出来、日志永远写不进，**而且完全不报错** —— 症状与
Windows 端那个「UTF-8 被按 GBK 解释致统计全空、磁盘零证据」的历史故障
一模一样。

故 `FileShim.mm` 的 `NormPath()` 在**每一个**接受宽字符路径的入口做
`'\' → '/'` 归一化。新增任何文件 API 到垫片时，这一条不可遗漏。

### ⚠️ 往返测试的判据不能是「字节相等」

曾把 CI 判据写成「序列化后与原始行逐字节相等」，**这是错的**：
`SerializeRecord:551` 对 `type='p'` **无条件**写 `did` 字段，而样本数据里的
旧记录没有 `did`（该字段是后来加的，`ParseRecord:583` 注释明确写了
「旧日志无 did 字段 → 默认 0」）。字节比对会在实现完全正确的情况下判红。

正确判据是**语义往返**：`原始行 → RawRecord A → 序列化 → RawRecord B`，
比对 A 与 B 的**每一个字段**（含 `pts`/`ppts` 轨迹数组的每个元素）。
这既是正确判据，也比字节比对更强 —— 它直接验证「数据经过一个完整循环后
无损」，而这正是跨端互通的充分条件。字节差异仍会统计并打印，作为信息项。

---

## 三、目标软件白名单：不改 schema 的做法

**已核实的发行情况**：

| 软件 | Mac 状态 |
|---|---|
| CSP / 优动漫 PAINT | ✅ 原生版（`CSP_515m_app.pkg`，需 macOS 13+） |
| Photoshop | ✅ 原生版 |
| **SAI2** | ❌ **官方无 Mac 版**，只能靠 CrossOver / Whisky 等兼容层 |

**决策：不新增 `targetsMac` 配置字段。** 原计划曾考虑新增字段，但核实
`Config::IsTarget` 只是纯字符串比对后，改用更简单的方案：Mac 端启动时把
内置的「exe 名 → bundle id」别名映射**追加**进 `Cfg().targets`。

好处：
- 配置 schema 零变更，`config.json` 在两端完全互通
- Windows 端遇到 bundle id 字符串自然匹配不上，直接忽略，无副作用
- 用户手动往 `targets` 里加条目在两端都生效

映射表放在 `platform/Config_mac.mm`，需真机核实各 bundle id
（CSP 与 Photoshop 的实际 bundle id 以真机 `osascript` 查询为准，
不要凭记忆填写）。

---

## 四、DPI / Retina 口径（项目历史踩过坑）

Windows 端曾因「把物理像素当 DIP 用于布局」导致 125%/150% 缩放下右边界
与下边界溢出被裁掉（「换台机器右侧就没了」，2026-10-17 已修，见
`Ui.h:174-186`）。现有约定：

- `W()/H()` → 逻辑 DIP（布局与命中测试**唯一**可用口径）
- `PixelW()/PixelH()` → 物理像素（仅 Resize / PNG 导出用）
- 鼠标物理坐标须经 `ToDipX/Y()` 换算后才做命中测试

macOS 对应关系：`NSView` 坐标天然是逻辑点，`backingScaleFactor` 等价于
`DpiScale()`，**口径天然吻合、风险低于 Windows**。

垫片的实现方式（不改 `Common.h` 一行）：`Common.h::WindowDpi` 的逻辑是
先 `GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")`，
拿到函数指针才用它。垫片让 `GetModuleHandleW` 返回 `nullptr`、
`GetProcAddress` 返回 `nullptr`，于是自然落到 `GetDC(nullptr)` +
`GetDeviceCaps(LOGPIXELSX)` 分支，后者返回 `96 * backingScaleFactor`。

**为什么是 96 基准而非 macOS 的 72pt/inch**：`Painter::DpiScale() = dpi/96`
(`Ui.h:173`)，整个布局体系以 96 为「1 倍」。若返回 72，Retina 屏上会得到
0.75 倍缩放，界面整体缩小。

---

## 五、⚠️ 最高风险：wchar_t 宽度不同

Windows 的 `wchar_t` 是 16 位 UTF-16，macOS 是 32 位 UTF-32。而现有代码的
文本接口**全部**以 `wchar_t*` / `std::wstring` 为参数：`Painter::Text`、
`TextWrap`、`MeasureW`、`MeasureH`、`SanitizeFileName`、`ExePathW`、
`Tooltip::lines`、`DateTick::label`、`OpMark::note`……

转换错误的表现是**不崩溃、只产生乱码文件名或读不到历史数据**，属于最难
排查的一类问题。应对措施：

1. `Win32Shim.mm` 的 `W2U`/`U2W` 按 UTF-32 实现，严格保留 Win32 三条语义
   （源长度 < 0 表示以 null 结尾且返回长度含 null；缓冲为空进探测模式；
   容量不足返回 0）。
2. 无效码点（代理区 D800-DFFF、超过 10FFFF）统一替换为 U+FFFD，保证输出
   永远是合法 UTF-8——否则写进 jsonl 会让 Windows 端的 JSON 解析器读到
   非法序列。
3. 拒绝过长编码（overlong）。
4. M2 的渲染 PNG 比对是主要检出手段（乱码在图上直接可见）。

---

## 六、开机自启：注册表 → LaunchAgent

`Store.h:43-48` 的 `AutoStartApply` / `AutoStartEnabled` / `AutoStartSync`
在 Windows 端是写 `HKCU\...\Run` 键（带 `--tray` 参数，登录后直接进托盘）。

Mac 端改为 `~/Library/LaunchAgents/*.plist`，`RunAtLoad=true`，`KeepAlive=false`。
注意 Windows 端「每次启动按 autoStart 值同步注册表，故 exe 换位置后从新
位置启动一次即可自动纠正」这个行为，Mac 端应同样实现（每次启动重写 plist
里的程序路径），否则移动 .app 位置后自启会失效。

---

## 七、字体

`Ui.h:8-9` 说明用「Microsoft YaHei UI」是为了中英混排不出豆腐块。Mac 上
不存在该字体，换 **PingFang SC**（同样中英混排完整，且是 macOS 系统字体）。

副作用：`MeasureW`/`MeasureH` 结果会变，可能触发布局截断。M2 的 PNG 比对
是主要检出手段。

---

## 八、文件对照表

| Windows 端 | 行数 | macOS 端 | 说明 |
|---|---|---|---|
| `Analyzer.cpp/h` | 1134 | **原样复用** | 零 Win32 调用 |
| `Charts.cpp/h` | 1334 | **原样复用** | 只经 `Painter` 抽象 |
| `Json.cpp/h` | 348 | **原样复用** | 纯标准库 |
| `Config.cpp/h` | 271 | 原样编译 + `Config_mac.mm` | 追加 bundle id 别名映射 |
| `Common.h` | 407 | **原样复用** | 靠垫片满足 inline 函数 |
| `Ui.h` | 331 | **原样复用** | 类型由垫片提供 |
| `Store.h` | 99 | **原样复用** | `DWORD lastError` 由垫片覆盖 |
| `Store.cpp` | 790 | **原样复用**（垫片） | ⭐ 见下方「发现 4」，最关键的决定 |
| `Ui.cpp` | 867 | `Ui_mac.mm` | D2D/DWrite → Core Graphics/Core Text |
| `Capture.cpp/h` | 1796 | `Capture_mac.mm` | 低级钩子 → CGEventTap |
| `PenInput.cpp/h` | 1686 | `PenInput_mac.mm` | Raw Input+HID → tablet 事件 |
| `MainWindow.cpp/h` | 3332 | `appkit/*.mm` | 消息循环 → AppKit |
| `Recorder.cpp/h` | 1389 | `Recorder_mac.mm` | WGC → ScreenCaptureKit |
| `Video.cpp/h` | 896 | `Video_mac.mm` | Media Foundation → AVAssetWriter |
| `main.cpp` | 223 | `appkit/main.mm` | DPI 感知/COM → NSApplication |
| `PenInject.*` | 254 | **不移植** | Mac 端不注入事件 |
| `PenOverrides.*` | 430 | **不移植** | Mac 端不解析 HID 报文 |
| `PenInputDev.h` | 154 | **不移植** | 同上 |
| `app.rc`/`resource.h` | 75 | **不移植** | Mac 端不用 rc 资源 |

合计：现有约 13370 行中，约 3500 行原样复用、约 800 行半复用，
不移植约 900 行。Mac 端新写预估 3000-4000 行。

---

## 九、验证盲区（务必知情）

**CI 能证明**：编译通过；Windows 录的 jsonl 在 Mac 端读得出且往返一致；
图表离屏渲染布局正确。

**CI 不能证明**：真实数位笔输入与压感、真实录屏产物、TCC 权限流程。
GitHub Actions 的 runner **没有图形会话**，`CGEventTap` 与 `SCStream`
都无法运行。M3-M6 必须真机验证，这是本移植最主要的验证盲区。
