#include "PenInput.h"
#include "Store.h"

namespace pb {

// ------------------------------------------------------------
// HID Digitizer 用量常量
// ------------------------------------------------------------
static const USAGE UP_GENERIC   = 0x01;
static const USAGE UP_DIGITIZER = 0x0D;

static const USAGE U_DIG_FINGER        = 0x01;  // HID 规范名为 Finger；本机 Wacom 确实暴露了 0D:01 集合
static const USAGE U_DIG_PEN           = 0x02;
static const USAGE U_DIG_TOUCH_SCREEN  = 0x04;
static const USAGE U_DIG_TOUCH_PAD     = 0x05;
static const USAGE U_DIG_MULTI_POINT   = 0x0C;

// 并行监听的数位设备 Usage 清单。
// 必须同时注册多个：不同厂商/驱动会把笔暴露在 0x01、0x02 或 0x0C 上，
// 只注册其中一个会导致「设备能枚举到、但收不到任何报文」。
// 曾漏掉 0x01，而本机 Wacom 恰好有一个 0D:01 集合（pen_diag 实测输出）。
static const USAGE kDigitizerUsages[] = {
    U_DIG_FINGER, U_DIG_PEN, U_DIG_TOUCH_SCREEN, U_DIG_MULTI_POINT,
};
static const int kDigitizerUsageCount =
    (int)(sizeof(kDigitizerUsages) / sizeof(kDigitizerUsages[0]));

static bool IsDigitizerUsage(USAGE u) {
    for (int i = 0; i < kDigitizerUsageCount; ++i)
        if (kDigitizerUsages[i] == u) return true;
    return false;
}
static const USAGE U_DIG_TIP_PRESSURE  = 0x30;
static const USAGE U_DIG_IN_RANGE      = 0x32;
static const USAGE U_DIG_TOUCH         = 0x33;
static const USAGE U_DIG_TIP_SWITCH    = 0x42;
static const USAGE U_DIG_BARREL_SWITCH = 0x44;
static const USAGE U_DIG_ERASER        = 0x45;
static const USAGE U_GEN_X             = 0x30;
static const USAGE U_GEN_Y             = 0x31;

const char* PenInput::DeviceTypeName(uint8_t t) {
    switch (t) {
    case PEN_EXTERNAL:     return "外接数位板";
    case PEN_INTEGRATED:   return "数位屏";
    case PEN_TOUCH_SCREEN: return "触摸屏";
    case PEN_TOUCH_PAD:    return "触控板";
    default:               return "未知数位设备";
    }
}

// ------------------------------------------------------------
// 系统指针设备快照：官方坐标映射 + 笔类型判定
// ------------------------------------------------------------
struct PointerDevSnap {
    HANDLE handle = nullptr;
    int    type = 0;
    RECT   display{};      // 实测为「物理尺寸，单位 0.01mm」——不是屏幕坐标，勿用于映射
    RECT   device{};       // 实测为「屏幕像素矩形」——这才是坐标映射要用的
    HMONITOR monitor = nullptr;   // 绑定的显示器；NULL = 未绑定到特定显示器
    std::string desc;      // 设备描述 / 名称（界面展示用）

    // 来自 GetPointerDeviceProperties 的精确逻辑量程（与 HID 侧同口径，可直接比对）
    bool   hasLogical = false;
    INT32  lxMin = 0, lxMax = 0, lyMin = 0, lyMax = 0;
};

static std::vector<PointerDevSnap> EnumPointerDevices() {
    std::vector<PointerDevSnap> out;
    UINT32 count = 0;
    if (!GetPointerDevices(&count, nullptr) || count == 0) return out;
    if (count > 64) count = 64;
    std::vector<POINTER_DEVICE_INFO> infos(count);
    if (!GetPointerDevices(&count, infos.data())) return out;
    for (UINT32 i = 0; i < count; ++i) {
        PointerDevSnap s;
        s.handle = infos[i].device;
        s.type   = (int)infos[i].pointerDeviceType;
        s.monitor= infos[i].monitor;
        if (infos[i].productString[0]) s.desc = W2U(infos[i].productString);
        RECT disp{}, dev{};
        if (GetPointerDeviceRects(s.handle, &disp, &dev)) { s.display = disp; s.device = dev; }

        // 取精确逻辑量程。这是能与 HID 侧 LogicalMin/Max 直接比对的唯一同源数据：
        // 实测 GetPointerDeviceRects 的 device 是屏幕像素、display 是 0.01mm 物理尺寸，
        // 两者都不是逻辑单位，拿它们和 HID 逻辑范围比对口径不符（曾导致本机
        // 5 个设备 score 全部高达 1.5~1.7、官方映射从未成功过）。
        UINT32 pc = 0;
        if (GetPointerDeviceProperties(s.handle, &pc, nullptr) && pc > 0) {
            if (pc > 32) pc = 32;
            std::vector<POINTER_DEVICE_PROPERTY> props(pc);
            if (GetPointerDeviceProperties(s.handle, &pc, props.data())) {
                for (UINT32 k = 0; k < pc; ++k) {
                    const auto& pr = props[k];
                    if (pr.usagePageId == UP_GENERIC && pr.usageId == U_GEN_X) {
                        s.lxMin = pr.logicalMin; s.lxMax = pr.logicalMax; s.hasLogical = true;
                    } else if (pr.usagePageId == UP_GENERIC && pr.usageId == U_GEN_Y) {
                        s.lyMin = pr.logicalMin; s.lyMax = pr.logicalMax; s.hasLogical = true;
                    }
                }
            }
        }
        out.push_back(s);
    }
    return out;
}

// ------------------------------------------------------------
// 注册
// ------------------------------------------------------------
bool PenInput::Register(HWND hwnd, bool pageOnly) {
    if (registered_) return true;
    hwnd_ = hwnd;
    pageOnly_ = pageOnly;

    // 加载覆写规则（逃生舱口）。必须在预枚举之前，
    // 否则启动期发现的设备拿不到覆写。文件不存在是正常情况。
    // 测试可用 SetOverrides 预先注入，届时非空即跳过文件加载，
    // 避免测试环境读到用户真实配置。
    if (overrides_.empty()) {
        std::vector<std::string> ovErr;
        overrides_ = LoadOverrides(ovErr);
    }

    // 启动期预枚举现有设备。
    // 此前有 `nDev < 512` 的上限：超限就整段跳过，界面上设备列表为空，
    // 要等用户第一次运笔才由懒加载补上。改为分批读取，不再丢弃任何设备。
    UINT nDev = 0;
    GetRawInputDeviceList(nullptr, &nDev, sizeof(RAWINPUTDEVICELIST));
    if (nDev > 0) {
        const UINT kBatch = 256;
        std::vector<RAWINPUTDEVICELIST> list;
        for (UINT off = 0; off < nDev; off += kBatch) {
            UINT want = std::min(kBatch, nDev - off);
            list.resize(want);
            UINT got = GetRawInputDeviceList(list.data(), &want, sizeof(RAWINPUTDEVICELIST));
            if (got == (UINT)-1) {
                DWORD err = GetLastError();
                // ERROR_INSUFFICIENT_BUFFER：列表在两次调用之间变大，重新取总数
                if (err == ERROR_INSUFFICIENT_BUFFER) {
                    GetRawInputDeviceList(nullptr, &nDev, sizeof(RAWINPUTDEVICELIST));
                    if (nDev > off) continue;      // 重试本批
                }
                LogF("[pen] GetRawInputDeviceList 分批读取失败 err=%lu off=%u", err, off);
                break;
            }
            if (got == 0) break;
            for (UINT i = 0; i < got; ++i) {
                if (list[i].dwType != RIM_TYPEHID) continue;
                RID_DEVICE_INFO info{};
                info.cbSize = sizeof(info);
                UINT sz = sizeof(info);
                if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1)
                    continue;
                if (info.hid.usUsagePage != UP_DIGITIZER) continue;
                // pageOnly 模式下注册的是整个 0x0D 页，枚举过滤也必须放宽到
                // 整页，否则「注册得到、启动期却枚举不到」，只能等懒加载兜底。
                if (!pageOnly && !IsDigitizerUsage(info.hid.usUsage)) continue;
                Dev* d = FindOrCreate(list[i].hDevice);
                if (d && !d->preparsed && LoadDeviceInfo(*d)) {
                    ApplyOverride(*d);   // 必须在 BuildMapping 之前：覆写的量程要参与映射
                    BuildMapping(*d);
                    PenDeviceInfo pdi;
                    pdi.name = d->name; pdi.type = d->type;
                    pdi.hasPressure = d->hasPressure;
                    pdi.pressureMax = (int)d->pressMax;
                    pdi.mappingSource = d->mappingSource;
                    if (onDeviceFound) onDeviceFound(pdi);
                    LogF("[pen] device type=%d(%s) pressure=%d(%ld..%ld) logical=[%ld..%ld,%ld..%ld] mapSrc=%d %s",
                         (int)d->type, DeviceTypeName(d->type), (int)d->hasPressure, d->pressMin, d->pressMax,
                         d->lxMin, d->lxMax, d->lyMin, d->lyMax, d->mappingSource, d->name.c_str());
                }
            }
        }
    }

    // 不用 RIDEV_DEVNOTIFY：它会触发 WM_INPUT_DEVICE_CHANGE，而设备变更处理里
    // 若再 Unregister+Register，就形成「注册→通知→重注册」死循环（曾导致日志
    // 在数十毫秒内刷屏上万行、占满一个 CPU 核）。Raw Input 按 usage 注册
    // 本就自动覆盖此后插入的设备，OnRawInput 里也有懒加载解析兜底。
    //
    // 默认走 RIDEV_PAGEONLY 注册整个 Usage Page 0x0D：
    //   官方语义为「该 usage page 下的所有顶级集合」（要求 usUsage 必须为 0，
    //   且与 RIDEV_INPUTSINK 无互斥、后者要求 hwndTarget 非空）。
    //   这样任何品牌、任何现在与未来的 TLC 划分方式都自动覆盖。
    //
    //   为什么必须这么做：本机实测 Wacom 的笔数据 100% 落在 usage 0x01
    //   （0x02/0x04/0x0C 均为 0 条），即「笔落在哪个集合完全由厂商驱动决定」。
    //   连 OpenTabletDriver 都需要人工指定 WinUsage。显式清单永远只能覆盖
    //   已知踩过的坑，而漏掉的后果是「完全无数据且日志不报任何错」——
    //   Register 只会打印 N device(s) parsed，看起来一切正常。
    //
    //   代价：会同时收到触控板/触摸屏等杂设备的高频报文，故 OnRawInput 里
    //   对无 X/Y 能力的设备做一次性早退（见 rejected_），把每条报文的开销
    //   压到一次 map 查找。
    RAWINPUTDEVICE rid[8];
    int n = 0;
    if (pageOnly) {
        rid[n].usUsagePage = UP_DIGITIZER;
        rid[n].usUsage     = 0;                              // PAGEONLY 要求为 0
        rid[n].dwFlags     = RIDEV_PAGEONLY | RIDEV_INPUTSINK;
        rid[n].hwndTarget  = hwnd;
        ++n;
    } else {
        for (int i = 0; i < kDigitizerUsageCount && n < 8; ++i) {
            rid[n].usUsagePage = UP_DIGITIZER;
            rid[n].usUsage     = kDigitizerUsages[i];
            rid[n].dwFlags     = RIDEV_INPUTSINK;
            rid[n].hwndTarget  = hwnd;
            ++n;
        }
    }

    if (!RegisterRawInputDevices(rid, n, sizeof(RAWINPUTDEVICE))) {
        DWORD err = GetLastError();
        LogF("[pen] RegisterRawInputDevices failed: %lu (pageOnly=%d usages=%d)",
             err, (int)pageOnly, n);
        // PAGEONLY 万一在某些系统上不被接受，自动退回显式清单再试一次，
        // 宁可覆盖面窄也不要笔完全无数据。
        if (pageOnly) {
            LogF("[pen] 回退到显式 usage 清单 (01/02/04/0C) 重试");
            pageOnly_ = false;
            int m = 0;
            for (int i = 0; i < kDigitizerUsageCount && m < 8; ++i) {
                rid[m].usUsagePage = UP_DIGITIZER;
                rid[m].usUsage     = kDigitizerUsages[i];
                rid[m].dwFlags     = RIDEV_INPUTSINK;
                rid[m].hwndTarget  = hwnd;
                ++m;
            }
            if (!RegisterRawInputDevices(rid, m, sizeof(RAWINPUTDEVICE))) {
                LogF("[pen] RegisterRawInputDevices 回退也失败: %lu (usages=%d)",
                     GetLastError(), m);
                return false;
            }
            registered_ = true;
            LogF("[pen] registered %d digitizer usages (01/02/04/0C, 回退模式); %zu device(s) parsed",
                 m, devs_.size());
            return true;
        }
        return false;
    }
    registered_ = true;
    LogF("[pen] registered digitizer usages: %s; %zu device(s) parsed",
         pageOnly_ ? "PAGEONLY 0x0D/* (全部 TLC)" : "01/02/04/0C", devs_.size());

    // 回读系统实际生效的注册表。
    // 为什么不能只信 RegisterRawInputDevices 的返回值：真实用户报告
    // （diagnostics_20261018_144042/_144046）显示注册日志正常打印、
    // penRawReports=0、且日志里既没有 "first WM_INPUT arrived" 也没有任何
    // "[pen] DROP:" 行 —— 说明 OnRawInput 一次都没被调用，故障在投递层
    // 而非解析层。只有回读才能区分「注册没生效」与「注册生效但系统不投递」。
    // 注意：与 GetRawInputDeviceList 不同，本 API 需要第三个参数 cbSize，
    // 且 puiNumDevices 是 in/out —— 传入缓冲区容量，传出实际数量。
    UINT regN = 0;
    GetRegisteredRawInputDevices(nullptr, &regN, sizeof(RAWINPUTDEVICE));
    if (regN > 0 && regN <= 64) {
        std::vector<RAWINPUTDEVICE> regs(regN);
        UINT got = GetRegisteredRawInputDevices(regs.data(), &regN, sizeof(RAWINPUTDEVICE));
        if (got != (UINT)-1) {
            // 防御：got 不应超过缓冲区容量，但两次调用之间注册项可能增多，
            // 钳制上界避免越界读。
            UINT lim = std::min(got, (UINT)regs.size());
            for (UINT i = 0; i < lim; ++i) {
                // 只关心我们注册的 0x0D 页，避免把其他进程的注册项刷进日志
                if (regs[i].usUsagePage != UP_DIGITIZER) continue;
                LogF("[pen] registered-table[%u]: usagePage=0x%02X usage=0x%02X flags=0x%08lX hwnd=%p",
                     (unsigned)i, (unsigned)regs[i].usUsagePage, (unsigned)regs[i].usUsage,
                     (unsigned long)regs[i].dwFlags, (void*)regs[i].hwndTarget);
            }
        }
    }
    // 窗口自身状态：投递依赖它有效且属于当前线程
    LogF("[pen] capture hwnd=%p thread=%lu valid=%d",
         (void*)hwnd_, GetCurrentThreadId(), IsWindow(hwnd_) ? 1 : 0);
    return true;
}

void PenInput::Unregister() {
    if (!registered_) return;
    // 移除方式必须与注册时一致，否则移除不掉
    RAWINPUTDEVICE rid[8];
    int n = 0;
    if (pageOnly_) {
        rid[n].usUsagePage = UP_DIGITIZER; rid[n].usUsage = 0;
        rid[n].dwFlags = RIDEV_PAGEONLY | RIDEV_REMOVE; rid[n].hwndTarget = nullptr; ++n;
    } else {
        for (int i = 0; i < kDigitizerUsageCount && n < 8; ++i) {
            rid[n].usUsagePage = UP_DIGITIZER; rid[n].usUsage = kDigitizerUsages[i];
            rid[n].dwFlags = RIDEV_REMOVE; rid[n].hwndTarget = nullptr; ++n;
        }
    }
    RegisterRawInputDevices(rid, n, sizeof(RAWINPUTDEVICE));
    registered_ = false;
}


Dev* PenInput::FindOrCreate(HANDLE h) {
    auto it = devs_.find(h);
    if (it != devs_.end()) return &it->second;

    Dev d;
    d.handle = h;
    d.devSeq = ++nextDevSeq_;   // 进程内自增，从 1 开始（0 保留给未知）
    wchar_t pathBuf[512] = {0};
    UINT cb = sizeof(pathBuf);
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, pathBuf, &cb) != (UINT)-1) {
        d.path = pathBuf;
    }

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT sz = sizeof(info);
    char vid[96];
    if (GetRawInputDeviceInfoW(h, RIDI_DEVICEINFO, &info, &sz) != (UINT)-1) {
        snprintf(vid, sizeof(vid), "VID_%04lX&PID_%04lX usage=%02X:%02X",
                 info.hid.dwVendorId, info.hid.dwProductId,
                 info.hid.usUsagePage, info.hid.usUsage);
        d.name = vid;
        d.topUsage = info.hid.usUsage;
        d.topUsagePage = info.hid.usUsagePage;
        d.vid = info.hid.dwVendorId;
        d.pid = info.hid.dwProductId;
    } else {
        d.name = "unknown-device";
    }

    devs_[h] = std::move(d);
    return &devs_[h];
}

bool PenInput::LoadDeviceInfo(Dev& d) {
    UINT cb = 0;
    if (GetRawInputDeviceInfoW(d.handle, RIDI_PREPARSEDDATA, nullptr, &cb) == (UINT)-1 || cb == 0)
        return false;
    std::vector<BYTE> blob(cb);
    if (GetRawInputDeviceInfoW(d.handle, RIDI_PREPARSEDDATA, blob.data(), &cb) == (UINT)-1)
        return false;
    if (HidP_GetCaps((PHIDP_PREPARSED_DATA)blob.data(), &d.caps) != HIDP_STATUS_SUCCESS)
        return false;
    d.preparsedBlob = std::move(blob);
    d.preparsed = (PHIDP_PREPARSED_DATA)d.preparsedBlob.data();
    if (HidP_GetCaps(d.preparsed, &d.caps) != HIDP_STATUS_SUCCESS) return false;

    if (d.caps.NumberLinkCollectionNodes > 0 && d.caps.NumberLinkCollectionNodes < 512) {
        d.links.resize(d.caps.NumberLinkCollectionNodes);
        ULONG got = d.caps.NumberLinkCollectionNodes;
        if (HidP_GetLinkCollectionNodes(d.links.data(), &got, d.preparsed) != HIDP_STATUS_SUCCESS)
            d.links.clear();
        else
            d.links.resize((size_t)got);
    }

    // —— 值能力：X / Y / 压力 ——
    if (d.caps.NumberInputValueCaps > 0 && d.caps.NumberInputValueCaps < 512) {
        std::vector<HIDP_VALUE_CAPS> vc(d.caps.NumberInputValueCaps);
        USHORT n = d.caps.NumberInputValueCaps;
        if (HidP_GetValueCaps(HidP_Input, vc.data(), &n, d.preparsed) == HIDP_STATUS_SUCCESS) {
            vc.resize(n);
            for (const auto& c : vc) {
                if (c.ReportID != 0 && c.IsRange == FALSE) {
                    if (std::find(d.reportIDs.begin(), d.reportIDs.end(), (USAGE)c.ReportID) == d.reportIDs.end())
                        d.reportIDs.push_back((USAGE)c.ReportID);
                }
                USAGE u = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
                USAGE u2= c.IsRange ? c.Range.UsageMax : c.NotRange.Usage;
                if (c.UsagePage == UP_GENERIC && u == U_GEN_X && u2 == U_GEN_X && !d.hasX) {
                    d.hasX = true;
                    d.lxMin = c.LogicalMin; d.lxMax = c.LogicalMax;
                    d.xRel = (c.LogicalMin < 0);
                } else if (c.UsagePage == UP_GENERIC && u == U_GEN_Y && u2 == U_GEN_Y && !d.hasY) {
                    d.hasY = true;
                    d.lyMin = c.LogicalMin; d.lyMax = c.LogicalMax;
                    d.yRel = (c.LogicalMin < 0);
                } else if (c.UsagePage == UP_DIGITIZER &&
                           ((u <= U_DIG_TIP_PRESSURE && U_DIG_TIP_PRESSURE <= u2) || u == U_DIG_TIP_PRESSURE) &&
                           !d.hasPressure) {
                    d.hasPressure = true;
                    d.pressMin = c.LogicalMin;
                    d.pressMax = c.LogicalMax > c.LogicalMin ? c.LogicalMax : 1;
                    // 压感字段与笔的 X/Y 必然在同一个 link collection 里，
                    // 所以它的 LinkCollection 就是笔集合索引。这是复合设备上
                    // 区分「笔的 X/Y」与「触摸的 X/Y」的唯一可靠依据。
                    d.penLink = c.LinkCollection;
                }
            }
        }
    }

    // —— 按钮能力 ——
    if (d.caps.NumberInputButtonCaps > 0 && d.caps.NumberInputButtonCaps < 512) {
        std::vector<HIDP_BUTTON_CAPS> bc(d.caps.NumberInputButtonCaps);
        USHORT n = d.caps.NumberInputButtonCaps;
        if (HidP_GetButtonCaps(HidP_Input, bc.data(), &n, d.preparsed) == HIDP_STATUS_SUCCESS) {
            bc.resize(n);
            for (const auto& c : bc) {
                if (c.ReportID != 0) {
                    d.usesReportIDs = true;
                    if (std::find(d.reportIDs.begin(), d.reportIDs.end(), (USAGE)c.ReportID) == d.reportIDs.end())
                        d.reportIDs.push_back((USAGE)c.ReportID);
                }
                if (c.UsagePage != UP_DIGITIZER) continue;
                USAGE lo = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
                USAGE hi = c.IsRange ? c.Range.UsageMax  : c.NotRange.Usage;
                auto hit = [&](USAGE t) { return lo <= t && t <= hi; };
                if (hit(U_DIG_TIP_SWITCH))    d.hasTipSwitch = true;
                if (hit(U_DIG_IN_RANGE))      d.hasInRange   = true;
                if (hit(U_DIG_ERASER))        d.hasEraser    = true;
                if (hit(U_DIG_TOUCH))         d.hasTouch     = true;
                if (hit(U_DIG_BARREL_SWITCH)) d.hasBarrel    = true;
                // TipSwitch(0x42) 只出现在笔集合里（触摸集合用 Touch 0x33），
                // 因此它是「无压感设备」定位笔集合的可靠依据。
                // 仅在压感尚未定位时补记，避免覆盖上面更精确的结果。
                if (hit(U_DIG_TIP_SWITCH) && d.penLink == 0)
                    d.penLink = c.LinkCollection;
            }
        }
    }

    if (d.caps.InputReportByteLength > 0 && d.caps.InputReportByteLength <= 65536)
        d.scratch.resize(d.caps.InputReportByteLength + 8);

    // —— 设备类型（抽成纯函数 ClassifyDeviceType，可离线测试）——
    DevTypeResult dt = ClassifyDeviceType(d.topUsage, d.hasPressure);
    d.type = dt.type;
    d.needPointerType = dt.needPointerType;

    return d.hasX && d.hasY;
}

PenInput::DevTypeResult PenInput::ClassifyDeviceType(USAGE topUsage, bool hasPressure) {
    DevTypeResult r;
    if (topUsage == U_DIG_TOUCH_SCREEN) {          // 0x04
        r.type = PEN_TOUCH_SCREEN;
    } else if (topUsage == U_DIG_TOUCH_PAD) {       // 0x05 笔记本触控板
        // 不是绘图设备。此前落入 else、hasPressure=false 被误标为「触摸屏」，
        // 还参与笔坐标映射（真实报告 mapSrc=2 兜底）。单独标记，OnRawInput 早退。
        r.type = PEN_TOUCH_PAD;
    } else if (topUsage == U_DIG_PEN || topUsage == U_DIG_MULTI_POINT) {  // 0x02 / 0x0C
        r.type = PEN_EXTERNAL;        // 默认外接数位板（绘画用户最常见）
        r.needPointerType = true;     // 待 BuildMapping 用指针类型细分数位屏/数位板
    } else {
        r.type = hasPressure ? PEN_EXTERNAL : PEN_TOUCH_SCREEN;
        r.needPointerType = hasPressure;
    }
    return r;
}

bool PenInput::DecideReportLog(ReportLogState& st, bool tip, float pressure,
                               bool eraser, const char*& tag) {
    // 前 3 条无条件记（基线：证明通道通、看初始 x/y/p 是否合理）
    if (st.baseline < 3) {
        ++st.baseline;
        // 基线也标记已观测到的信号：若采集启动时笔已按下，基线行已含 tip=1，
        // 就不该在之后误报一次「首次落笔 tip0->1」（那并非真正的 0→1 跳变）。
        if (tip) st.sawTipDown = true;
        if (pressure > 0.f) st.sawPressure = true;
        if (eraser) st.sawEraser = true;
        st.lastTip = tip; st.lastPressure = pressure;
        tag = "baseline";
        return true;
    }
    // 关键跳变各记一次——比连续采样更有诊断价值：
    // 落笔(0→1)、首次压感、橡皮、抬笔(1→0)。这些是「设备落笔时到底报了什么」
    // 的答案，正是只记前 N 条会漏掉的（报告1 XP-Pen 前5条全悬空即为例）。
    bool log = false;
    if (tip && !st.sawTipDown) { tag = "首次落笔 tip0->1"; log = true; }
    else if (pressure > 0.f && !st.sawPressure) { tag = "首次压感 p>0"; log = true; }
    else if (eraser && !st.sawEraser) { tag = "橡皮 eraser=1"; log = true; }
    else if (!tip && st.lastTip && !st.sawTipUp) { tag = "首次抬笔 tip1->0"; log = true; }
    // 记录时把当前已满足的信号一并标记：一次「落笔且带压感」只出一行（tag=落笔，
    // 行内已含 p 值），不再拆成「落笔」+「首次压感」两行噪声。
    if (log) {
        if (tip) st.sawTipDown = true;
        if (pressure > 0.f) st.sawPressure = true;
        if (eraser) st.sawEraser = true;
        if (!tip && st.lastTip) st.sawTipUp = true;
    }
    st.lastTip = tip; st.lastPressure = pressure;
    return log;
}

PenInput::PenLogScan PenInput::ScanPenLog(const std::string& logText) {
    PenLogScan sc;
    size_t pos = 0;
    while (pos < logText.size()) {
        size_t nl = logText.find('\n', pos);
        std::string line = logText.substr(pos, nl == std::string::npos
                                                ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? logText.size() : nl + 1;
        if (line.find("] [pen]") == std::string::npos) continue;
        // 会话分界：每次启动注册都会打这一行
        if (line.find("registered digitizer usages") != std::string::npos) {
            ++sc.sessions;
        }
        if (line.find("[pen] OK") != std::string::npos) {
            ++sc.okLines;
            sc.everGotReport = true;
            sc.everParsedDev = true;
        } else if (line.find("report from device") != std::string::npos) {
            sc.everParsedDev = true;
        }
    }
    return sc;
}

// ------------------------------------------------------------
// 应用覆写（逃生舱口）
//
// 时机：LoadDeviceInfo 之后、BuildMapping 之前。
// 顺序很关键 —— 覆写的逻辑量程必须先生效，BuildMapping 才能用正确的
// 量程建立映射；覆写的目标显示器也要先解析好，BuildMapping 才知道该优先用它。
//
// 命中多条规则时，后面的规则覆盖前面的（与「越具体的规则写在越后面」的
// 直觉一致），并对每条生效的字段记一次日志，让用户知道当前不是自动路径。
// ------------------------------------------------------------
void PenInput::ApplyOverride(Dev& d) {
    if (overrides_.empty()) return;

    std::string pathLower = ToLowerA(W2U(d.path));
    for (const PenOverride& o : overrides_) {
        if (!o.Matches(d.vid, d.pid, d.topUsage, pathLower)) continue;

        d.ovApplied = true;
        std::string applied;

        if (o.hasLogical) {
            d.lxMin = o.lxMin; d.lxMax = o.lxMax;
            d.lyMin = o.lyMin; d.lyMax = o.lyMax;
            d.ovLogical = true;
            // 量程变了，相对模式判定要跟着重算（LogicalMin<0 即相对）
            d.xRel = (d.lxMin < 0);
            d.yRel = (d.lyMin < 0);
            applied += "logical ";
        }
        if (o.hasPressureMax) {
            d.pressMax = o.pressureMax;
            if (d.pressMin >= d.pressMax) d.pressMin = 0;   // 防止 span<=0 除零
            d.hasPressure = true;      // 覆写压感上限意味着该设备确实有压感
            d.ovPressure = true;
            applied += "pressureMax ";
        }
        if (o.hasPenLink) {
            d.penLink = o.penLink;
            d.ovPenLink = true;
            applied += "penLink ";
        }
        if (o.hasDeviceType) {
            d.type = o.deviceType;
            d.needPointerType = false;   // 人工指定优先，不再被指针类型覆盖
            d.ovType = true;
            applied += "deviceType ";
        }
        if (o.hasDisplayName || o.hasDisplayIndex) {
            RECT r{};
            if (ResolveDisplayRect(o, r)) {
                d.hasOvDisplay = true;
                d.ovDisplay = r;
                applied += "display ";
            } else {
                LogF("[pen] override WARN: 显示器指定无法解析，"
                     "本设备仍走自动映射 (%s)", d.name.c_str());
            }
        }

        LogF("[pen] override applied -> %s(%s)", applied.c_str(), d.name.c_str());
    }

    // 覆写了显示器却解析失败时，列出可用名称，用户照着改配置即可
    if (d.ovApplied && !d.hasOvDisplay) {
        for (const PenOverride& o : overrides_) {
            if (!o.Matches(d.vid, d.pid, d.topUsage, pathLower)) continue;
            if (!o.hasDisplayName && !o.hasDisplayIndex) continue;
            std::vector<std::string> names = EnumDisplayNames();
            std::string list;
            for (size_t i = 0; i < names.size(); ++i) {
                char b[64];
                snprintf(b, sizeof(b), "%zu=%s ", i, names[i].c_str());
                list += b;
            }
            LogF("[pen] override hint: 可用显示器 -> %s", list.c_str());
            break;
        }
    }
}

// ------------------------------------------------------------
// 指针设备候选消歧
//
// 探针实测（build/prop_probe_out.txt）：同一块 Cintiq Pro 27 会暴露
// 两个 PEN 指针条目——一个 monitor 绑定到具体显示器、另一个 monitor 为
// NULL 且矩形等于整个虚拟桌面。两者逻辑量程完全相同，但屏幕矩形不同：
//   绑定者 [0,0-3840,2160]（仅主屏）   未绑定者 [-3840,0-3840,2160]（跨两屏）
//
// 【标定实证推翻了「绑定优先」的直觉】
// 真机运笔时的鼠标反解标定拟合出 sx=0.23518771 ox=-3861.42，
// 即 sx = 7680/32767、ox = -3840 —— 恰好是「整个虚拟桌面」的映射。
// 这说明 Wacom 驱动的真实映射就是跨屏铺展（用户在数位屏上运笔时光标
// 确实会在两块屏之间移动）。若选绑定主屏的候选，坐标就会被压缩到主屏内，
// 与驱动实际行为不符，只能靠标定事后纠正 —— 而标定需要用户落笔运笔，
// 在那之前数据一直是错的。
//
// 因此规则改为：**跨屏候选（矩形等于虚拟桌面）优先**。这与改动前的
// 虚拟桌面兜底行为完全一致，对现有用户零回归；绑定候选只在没有跨屏
// 候选时才采纳（例如单屏机器，虚拟桌面就等于那块屏，两种候选矩形相同，
// 选谁都一样）。
//
// 规则（从确定到不确定）：
//   1. 只有一个候选 → 直接采纳
//   2. 有「矩形等于虚拟桌面」的跨屏候选 → 优先采纳（与驱动实际映射一致）
//   3. 其余情况优先在「绑定了显示器」的候选里选
//   4. 同一优先级内，若所有候选矩形完全相同 → 采纳（映射结果一致，歧义无害）
//   5. 矩形确实不同且无法按优先级区分 → 判为歧义，拒绝猜测
// ------------------------------------------------------------
static bool RectSame(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static bool IsSpanning(const RECT& r, const RECT& vd) {
    return r.left <= vd.left && r.top <= vd.top && r.right >= vd.right && r.bottom >= vd.bottom;
}

int PenInput::PickPointerCandidate(const PtrCand* c, size_t n, bool& ambiguous,
                                   const RECT* virtualRect) {
    ambiguous = false;
    if (n == 0) return -1;
    if (n == 1) return 0;

    RECT vd;
    if (virtualRect) vd = *virtualRect;
    else {
        vd.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
        vd.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
        vd.right  = vd.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vd.bottom = vd.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    // —— 1) 跨屏候选优先（矩形覆盖整个虚拟桌面）——
    int firstSpan = -1;
    bool spanConsistent = true;
    for (size_t i = 0; i < n; ++i) {
        if (!IsSpanning(c[i].rect, vd)) continue;
        if (firstSpan < 0) firstSpan = (int)i;
        else if (!RectSame(c[(size_t)firstSpan].rect, c[i].rect)) spanConsistent = false;
    }
    if (firstSpan >= 0) {
        if (spanConsistent) return firstSpan;
        ambiguous = true;   // 多个不同的跨屏候选，无法区分
        return -1;
    }

    // —— 2) 退而求其次：绑定显示器的候选 ——
    int firstBound = -1;
    size_t boundCount = 0;
    bool boundConsistent = true;
    for (size_t i = 0; i < n; ++i) {
        if (!c[i].hasMonitor) continue;
        if (firstBound < 0) { firstBound = (int)i; boundCount = 1; }
        else {
            ++boundCount;
            if (!RectSame(c[(size_t)firstBound].rect, c[i].rect)) boundConsistent = false;
        }
    }
    if (firstBound >= 0) {
        if (boundCount == 1) return firstBound;
        if (boundConsistent) return firstBound;   // 矩形全同，映射结果一致
        ambiguous = true;
        return -1;
    }

    // —— 3) 全都没绑定也不是跨屏：只在矩形完全一致时采纳 ——
    bool allSame = true;
    for (size_t i = 1; i < n; ++i)
        if (!RectSame(c[0].rect, c[i].rect)) { allSame = false; break; }
    if (allSame) return 0;
    ambiguous = true;
    return -1;
}

// 设备类型判定：映射与类型回答的是不同问题，故分开取。
//   映射 = 驱动把笔数据铺到哪个屏幕范围（真实行为）→ PickPointerCandidate 选的候选
//   类型 = 这块硬件是数位屏还是数位板（物理形态）→ 绑定显示器的候选
// 原因：Wacom 驱动把跨屏模式注册成 EXTERNAL_PEN（monitor=NULL），
// 把绑定主屏的模式注册成 INTEGRATED_PEN。Cintiq Pro 27 硬件上是数位屏，
// 若类型跟着跨屏候选走，会被错标成「外接数位板」。
// 对单屏/触摸/Ink 虚拟笔等场景该规则与旧行为一致（绑定候选与 best 是同一个）。
uint8_t PenInput::ResolvePenType(const int* types, const uint8_t* hasMon,
                                 size_t n, size_t bestIdx) {
    size_t idx = bestIdx;
    for (size_t i = 0; i < n; ++i) {
        if (hasMon[i]) { idx = i; break; }
    }
    if (idx >= n) return PEN_UNKNOWN;
    switch (types[idx]) {
    case POINTER_DEVICE_TYPE_INTEGRATED_PEN: return PEN_INTEGRATED;
    case POINTER_DEVICE_TYPE_EXTERNAL_PEN:   return PEN_EXTERNAL;
    case POINTER_DEVICE_TYPE_TOUCH:          return PEN_TOUCH_SCREEN;
    default:                                 return PEN_UNKNOWN;
    }
}

PenInput::CalibResult PenInput::EvaluateCalibration(
        const int32_t* lx, const int32_t* sx,
        const int32_t* ly, const int32_t* sy,
        size_t n, double logRangeX, double logRangeY) {
    CalibResult r;
    r.reason = "";
    // 样本不足：线性拟合至少需要若干个点，否则退化风险高
    if (n < 16) { r.reason = "样本不足"; return r; }

    // 最小二乘拟合 sx = a*lx + b，同时算 R²（决定系数）。
    // R² 低 = 逻辑坐标与屏幕坐标不是干净线性关系（噪声/多点/光标被拖动），
    // 此时拟合系数不可信，必须拒绝。映射本是精确线性变换，干净的 R²≈1。
    auto fit = [](const int32_t* X, const int32_t* Y, size_t n,
                  double& a, double& b, double& r2) -> bool {
        double sX = 0, sY = 0, sXX = 0, sXY = 0;
        for (size_t i = 0; i < n; ++i) {
            double x = X[i], y = Y[i];
            sX += x; sY += y; sXX += x * x; sXY += x * y;
        }
        double nd = (double)n;
        double den = nd * sXX - sX * sX;
        if (fabs(den) < 1e-9) return false;   // X 全同 → 无法拟合斜率
        a = (nd * sXY - sX * sY) / den;
        b = (sY - a * sX) / nd;
        // R² = 1 - SSres/SStot
        double meanY = sY / nd, ssTot = 0, ssRes = 0;
        for (size_t i = 0; i < n; ++i) {
            double y = Y[i];
            double pred = a * X[i] + b;
            ssTot += (y - meanY) * (y - meanY);
            ssRes += (y - pred) * (y - pred);
        }
        r2 = (ssTot < 1e-9) ? 0.0 : (1.0 - ssRes / ssTot);
        return true;
    };

    double ax = 1, bx = 0, ay = 1, by = 0, rx2 = 0, ry2 = 0;
    if (!fit(lx, sx, n, ax, bx, rx2) || !fit(ly, sy, n, ay, by, ry2)) {
        r.reason = "拟合奇异(逻辑坐标无变化)"; return r;
    }
    r.rx2 = rx2; r.ry2 = ry2;
    // R² 闸：真实映射是精确线性，退化/噪声样本会显著偏离 1
    if (rx2 < 0.98 || ry2 < 0.98) { r.reason = "拟合优度过低(非线性/噪声)"; return r; }
    // 斜率必须为正
    if (ax <= 1e-9 || ay <= 1e-9) { r.reason = "斜率非正"; return r; }

    // 斜率合理性闸：斜率 × 设备逻辑满量程 = 该设备映射到的屏幕像素跨度。
    // 必须落在真实显示器尺寸范围内。这一闸专拦报告2那种「R² 很高但斜率
    // 病态偏小」的退化解（sy≈0.0033 时跨度仅约 108px，远小于任何显示器高度）。
    //   下限 200px：小于此的显示器不存在，跨度这么小说明笔几乎没移动光标。
    //   上限 40000px：跨再多屏也不会超过，超出说明斜率被异常放大。
    double spanX = ax * logRangeX;
    double spanY = ay * logRangeY;
    if (spanX < 200 || spanX > 40000 || spanY < 200 || spanY > 40000) {
        r.reason = "斜率不合理(映射跨度超出显示器范围)"; return r;
    }

    r.accept = true;
    r.sx = ax; r.ox = bx; r.sy = ay; r.oy = by;
    return r;
}

void PenInput::BuildMapping(Dev& d) {
    d.mappingValid = false;
    d.mappingSource = 0;
    // 触控板不建笔映射（非绘图设备），避免诊断报告出现误导性的 mapSrc=2 兜底行
    if (d.type == PEN_TOUCH_PAD) return;
    std::vector<PointerDevSnap> pds = EnumPointerDevices();

    double lw = (double)(d.lxMax - d.lxMin);
    double lh = (double)(d.lyMax - d.lyMin);
    if (lw <= 0) lw = 1;
    if (lh <= 0) lh = 1;

    // —— 覆写指定的显示器优先，直接跳过自动匹配与歧义判定 ——
    //
    // 这是多屏歧义（两个 PEN 指针条目矩形不一致，消歧规则也无法区分）的
    // 确定性解法：用户明确说了这块板子对应哪个屏幕，就不需要再猜。
    // 只有当覆写的矩形确实有效时才采纳，否则落回自动路径（宁可精度低也不要错屏）。
    if (d.hasOvDisplay) {
        double dw = (double)(d.ovDisplay.right - d.ovDisplay.left);
        double dh = (double)(d.ovDisplay.bottom - d.ovDisplay.top);
        if (dw > 0 && dh > 0) {
            d.mapDisplay  = d.ovDisplay;
            d.mapDevLeft  = d.ovDisplay.left;
            d.mapDevTop   = d.ovDisplay.top;
            d.sx = dw / lw;  d.ox = (double)d.ovDisplay.left - d.lxMin * d.sx;
            d.sy = dh / lh;  d.oy = (double)d.ovDisplay.top  - d.lyMin * d.sy;
            d.mappingValid = true;
            d.mappingSource = 5;       // 5 = 覆写指定
            LogF("[pen] mapping from OVERRIDE display [%ld,%ld-%ld,%ld] "
                 "logical=[%ld..%ld,%ld..%ld] (%s)",
                 d.ovDisplay.left, d.ovDisplay.top, d.ovDisplay.right, d.ovDisplay.bottom,
                 (long)d.lxMin, (long)d.lxMax, (long)d.lyMin, (long)d.lyMax, d.name.c_str());
            return;
        }
        LogF("[pen] override WARN: 指定的显示器矩形无效(宽=%.0f 高=%.0f) -> 走自动映射",
             dw, dh);
    }

    // —— 匹配：只比「逻辑量程」，这是与 HID 侧同源可比的唯一数据 ——
    //
    // 为什么不用 GetPointerDeviceRects：探针实测（build/prop_probe_out.txt）显示
    //   Rects.device  = 屏幕像素矩形（如 [0,0-3840,2160]）
    //   Rects.display = 物理尺寸，单位 0.01mm（如 [0,0-60017,33935]）
    // 两者都不是逻辑单位。旧代码拿 HID 逻辑范围（32767）去比 device 的像素宽
    // （7680），误差和恰为 1.6997 —— 与 pb.log 里本机全部设备的 score 完全吻合，
    // 即官方映射从未成功过，一直退化到虚拟桌面兜底。
    //
    // 类型过滤：有压感的 HID 集合是笔，只与 PEN 类指针设备匹配；无压感的是触摸，
    // 只与 TOUCH 类匹配。这一步能消掉大部分歧义（笔不会被匹配到触摸屏上）。
    const bool wantPen = d.hasPressure;
    std::vector<const PointerDevSnap*> cands;
    for (const auto& p : pds) {
        if (!p.hasLogical) continue;
        double pw = (double)(p.lxMax - p.lxMin);
        double ph = (double)(p.lyMax - p.lyMin);
        if (pw <= 0 || ph <= 0) continue;
        // 量程相对误差：要求几乎相同（归一化到 0..32767 的品牌都能精确对上）
        double ex = fabs(pw - lw) / lw;
        double ey = fabs(ph - lh) / lh;
        if (ex + ey > 0.02) continue;
        bool isPenType = (p.type == POINTER_DEVICE_TYPE_INTEGRATED_PEN ||
                          p.type == POINTER_DEVICE_TYPE_EXTERNAL_PEN);
        bool isTouchType = (p.type == POINTER_DEVICE_TYPE_TOUCH);
        if (wantPen && !isPenType) continue;
        if (!wantPen && !isTouchType) continue;
        cands.push_back(&p);
    }

    // —— 消歧：优先选「绑定显示器」的候选；矩形一致视为无害；否则拒绝猜测 ——
    // 详见 PickPointerCandidate 的注释（规则与探针实测依据）。
    std::vector<PtrCand> cs;
    cs.reserve(cands.size());
    for (const auto* p : cands) {
        PtrCand c;
        c.rect = p->device;
        c.hasMonitor = (p->monitor != nullptr);
        c.type = p->type;
        cs.push_back(c);
    }
    bool ambiguous = false;
    int pick = PickPointerCandidate(cs.data(), cs.size(), ambiguous);
    const PointerDevSnap* best = (pick >= 0) ? cands[(size_t)pick] : nullptr;

    if (best && !ambiguous) {
        // 映射：screen = (logical - logicalMin) / 量程 * 像素宽 + 像素左边界
        // 注意用的是 device（屏幕像素），不是 display（物理毫米）——
        // 旧公式误把 display 当屏幕坐标，算出的 sx 是「毫米/像素」的无意义比值；
        // 因为匹配从未成功，这个 bug 一直被掩盖，一旦匹配生效就会产出垃圾坐标。
        double dw = (double)(best->device.right - best->device.left);
        double dh = (double)(best->device.bottom - best->device.top);
        if (dw > 0 && dh > 0) {
            d.mapDisplay  = best->device;         // 存像素矩形（字段名沿用旧名）
            d.mapDevLeft  = best->device.left;
            d.mapDevTop   = best->device.top;
            d.sx = dw / lw;  d.ox = (double)best->device.left - d.lxMin * d.sx;
            d.sy = dh / lh;  d.oy = (double)best->device.top  - d.lyMin * d.sy;
            d.mappingValid = true;
            d.mappingSource = 1;
            if (d.needPointerType) {
                // 类型与映射分离：映射用 best（驱动真实行为），类型用绑定候选
                // （硬件形态真相）。详见 ResolvePenType 的注释。
                std::vector<int> ts; std::vector<uint8_t> hm;
                ts.reserve(cands.size()); hm.reserve(cands.size());
                for (const auto* p : cands) { ts.push_back(p->type); hm.push_back(p->monitor != nullptr ? 1 : 0); }
                uint8_t rt = ResolvePenType(ts.data(), hm.data(), ts.size(), (size_t)pick);
                if (rt != PEN_UNKNOWN) d.type = rt;
            }
            LogF("[pen] pointer match OK: logical=[%ld..%ld,%ld..%ld] -> pixels=[%ld,%ld-%ld,%ld] "
                 "monitor=%p cand=%zu (%s)",
                 (long)d.lxMin, (long)d.lxMax, (long)d.lyMin, (long)d.lyMax,
                 best->device.left, best->device.top, best->device.right, best->device.bottom,
                 (void*)best->monitor, cands.size(), d.name.c_str());
        } else {
            best = nullptr;   // 像素矩形无效，走兜底
        }
    }

    if (!best || ambiguous) {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0 || vh <= 0) {
            vx = vy = 0;
            vw = GetSystemMetrics(SM_CXSCREEN);
            vh = GetSystemMetrics(SM_CYSCREEN);
        }
        d.sx = (double)vw / lw;  d.ox = vx - d.lxMin * d.sx;
        d.sy = (double)vh / lh;  d.oy = vy - d.lyMin * d.sy;
        d.mappingValid = true;
        // 4 = 歧义（找到了多个映射结果不一致的候选，拒绝猜测）
        // 2 = 无候选（没找到量程匹配的同类型指针设备）
        d.mappingSource = ambiguous ? 4 : 2;
        if (ambiguous) {
            LogF("[pen] WARN pointer match AMBIGUOUS: %zu 个候选的屏幕矩形不一致，"
                 "拒绝猜测 -> 虚拟桌面兜底映射（多屏下坐标可能偏移；"
                 "可用覆写文件指定目标显示器）logical=[%ld..%ld,%ld..%ld] (%s)",
                 cands.size(), (long)d.lxMin, (long)d.lxMax, (long)d.lyMin, (long)d.lyMax,
                 d.name.c_str());
        } else {
            LogF("[pen] pointer match: 无同类型量程匹配候选(cand=%zu) -> virtual-screen mapping (%s)",
                 cands.size(), d.name.c_str());
        }
    }
}

void PenInput::AddCalibrationSample(uint32_t devSeq, uint8_t deviceType,
                                    int32_t lx, int32_t ly, int32_t sx, int32_t sy) {
    for (auto& kv : devs_) {
        Dev& d = kv.second;
        // 优先按 devSeq 精确命中；devSeq==0（未知）时退回按 type 匹配。
        // 精确命中避免两块同为 PEN_EXTERNAL 的板子互相污染映射系数。
        if (devSeq != 0) { if (d.devSeq != devSeq) continue; }
        else if (d.type != deviceType) continue;
        if (d.xRel || d.yRel) continue;   // 相对模式无需标定
        d.calLX.push_back(lx); d.calSX.push_back(sx);
        d.calLY.push_back(ly); d.calSY.push_back(sy);
        const size_t MAXS = 64;
        if (d.calLX.size() > MAXS) {
            d.calLX.erase(d.calLX.begin()); d.calSX.erase(d.calSX.begin());
            d.calLY.erase(d.calLY.begin()); d.calSY.erase(d.calSY.begin());
        }
        if (d.calLX.size() < 16) continue;
        int32_t minx = *std::min_element(d.calLX.begin(), d.calLX.end());
        int32_t maxx = *std::max_element(d.calLX.begin(), d.calLX.end());
        int32_t miny = *std::min_element(d.calLY.begin(), d.calLY.end());
        int32_t maxy = *std::max_element(d.calLY.begin(), d.calLY.end());
        if (maxx - minx < 400 || maxy - miny < 400) continue;   // 样本跨度不足

        // 攒够样本后：拟合 + 退化解校验（抽成纯函数 EvaluateCalibration，可离线测试）。
        // 旧代码只查 ax>1e-6，放过了报告2那种「R²很高但斜率病态偏小」的退化解
        // （sy≈0.0033 → 映射跨度仅约108px），采纳后笔坐标严重错位。
        CalibResult cr = EvaluateCalibration(
            d.calLX.data(), d.calSX.data(), d.calLY.data(), d.calSY.data(),
            d.calLX.size(),
            (double)(d.lxMax - d.lxMin), (double)(d.lyMax - d.lyMin));
        if (cr.accept) {
            d.sx = cr.sx; d.ox = cr.ox; d.sy = cr.sy; d.oy = cr.oy;
            d.mappingSource = 3;
            d.calibRejectLogged = false;   // 采纳成功，重置拒绝日志限流
            LogF("[pen] calibration applied sx=%.8f ox=%.2f sy=%.8f oy=%.2f (R2 x/y=%.4f/%.4f)",
                 cr.sx, cr.ox, cr.sy, cr.oy, cr.rx2, cr.ry2);
        } else if (!d.calibRejectLogged) {
            // 退化解：拒绝采纳、保留几何映射（mapSrc 不变），只记一次日志避免刷屏
            d.calibRejectLogged = true;
            LogF("[pen] calibration REJECTED: %s (R2 x/y=%.4f/%.4f) — 保留几何映射",
                 cr.reason, cr.rx2, cr.ry2);
        }
        d.calLX.clear(); d.calSX.clear(); d.calLY.clear(); d.calSY.clear();
    }
}

std::vector<PenDeviceInfo> PenInput::Devices() const {
    std::vector<PenDeviceInfo> out;
    for (const auto& kv : devs_) {
        const Dev& d = kv.second;
        if (!d.hasX || !d.hasY) continue;
        PenDeviceInfo i;
        i.name = d.name;
        i.type = d.type;
        i.hasPressure = d.hasPressure;
        i.pressureMax = (int)d.pressMax;
        i.mappingSource = d.mappingSource;
        i.devReports = d.devReports;
        i.devParsed  = d.devParsed;
        out.push_back(i);
    }
    return out;
}

uint8_t PenInput::DominantPenType() const {
    // 只认「有压感」的设备 —— 触摸屏无压感，不能代表笔。
    // 若找到多个且类型不一致，说明同时接了数位屏与数位板，
    // 此时无法判定 Ink 回退路径的笔属于哪一台，返回 UNKNOWN 让调用方退回默认值。
    uint8_t found = PEN_UNKNOWN;
    for (const auto& kv : devs_) {
        const Dev& d = kv.second;
        if (!d.hasX || !d.hasY) continue;
        if (!d.hasPressure) continue;
        if (d.type != PEN_EXTERNAL && d.type != PEN_INTEGRATED) continue;
        if (found == PEN_UNKNOWN) found = d.type;
        else if (found != d.type) return PEN_UNKNOWN;   // 类型冲突，不猜
    }
    return found;
}

bool PenInput::HasPressurePenDevice() const {
    // 只认「有压感的笔设备」：触摸屏无压感，触控板不是笔，都不能代表笔存在。
    // 与 DominantPenType 不同，这里不做类型一致性判断 —— 数位屏与数位板并存时
    // 依然返回 true，因为问题只是「机器上有没有笔」。
    for (const auto& kv : devs_) {
        const Dev& d = kv.second;
        if (!d.hasX || !d.hasY) continue;
        if (!d.hasPressure) continue;
        if (d.type != PEN_EXTERNAL && d.type != PEN_INTEGRATED) continue;
        return true;
    }
    return false;
}

// ------------------------------------------------------------
// 报文解析（多形态探测）
//
//  Raw Input 送来的 HID 报文形态因驱动而异：
//    形态0  报文本身含 Report ID 前缀，HidP 可直接解析
//    形态1  系统已剥掉前缀，需补回正确的 Report ID 才能解析
//  Wacom 等复合设备（笔/触摸/ExpressKeys 多 Report ID）的笔数据
//  常常不在能力表收集到的第一个 ID 上，因此形态1需要穷举 1..255。
//  首次成功后缓存形态与 ID，后续报文零开销直解。
// ------------------------------------------------------------
// 生产环境的真实实现：唯一直接接触 HidP_* 取值的地方。
// 测试通过 PenInput::SetHidFetchForTest 整体替换它，从而在没有对应品牌
// 硬件的情况下驱动后续解析链路（详见 PenInput.h 的接缝说明）。
bool PenInput::RealHidFetch(PHIDP_PREPARSED_DATA prep, const BYTE* buf, ULONG len,
                            USHORT linkCollection,
                            USAGE* usages, ULONG& nUsages,
                            LONG& vx, LONG& vy, LONG& vp,
                            bool& gx, bool& gy, bool& gp) {
    nUsages = 64;
    NTSTATUS st = HidP_GetUsages(HidP_Input, UP_DIGITIZER, linkCollection, usages, &nUsages, prep,
                                 (PCHAR)buf, len);
    if (st != HIDP_STATUS_SUCCESS && st != HIDP_STATUS_USAGE_NOT_FOUND) return false;

    // HidP_GetUsageValue 的输出参数是 ULONG（无符号），需自行按有符号解释
    ULONG ux = 0, uy = 0, up = 0;
    gx = (HidP_GetUsageValue(HidP_Input, UP_GENERIC, linkCollection, U_GEN_X, &ux, prep, (PCHAR)buf, len) == HIDP_STATUS_SUCCESS);
    gy = (HidP_GetUsageValue(HidP_Input, UP_GENERIC, linkCollection, U_GEN_Y, &uy, prep, (PCHAR)buf, len) == HIDP_STATUS_SUCCESS);
    gp = (HidP_GetUsageValue(HidP_Input, UP_DIGITIZER, linkCollection, U_DIG_TIP_PRESSURE, &up, prep, (PCHAR)buf, len) == HIDP_STATUS_SUCCESS);
    vx = (LONG)ux; vy = (LONG)uy; vp = (LONG)up;
    return gx && gy;
}

// 指定 link collection 取值；若该集合里取不到 X/Y，退回不指定（0）。
//
// 为什么必须带回退：penLink 是从 TipPressure/TipSwitch 所在 cap 反推的，
// 但个别设备把 X/Y 声明在**父集合**而把开关声明在子集合。此时按 penLink
// 精确查会返回 USAGE_NOT_FOUND —— 若不回退，这类设备将完全无法解析。
// 回退到 0 的行为与本次改动前完全一致，因此对单集合设备零影响，
// 只有「确实存在同 Usage 歧义」的复合设备才会真正用上 penLink。
static bool FetchWithFallback(const PenInput::HidFetchFn& fn, PHIDP_PREPARSED_DATA prep,
                              const BYTE* buf, ULONG len, USHORT penLink,
                              USAGE* usages, ULONG& nUsages,
                              LONG& vx, LONG& vy, LONG& vp,
                              bool& gx, bool& gy, bool& gp, USHORT& usedLink) {
    if (penLink != 0 &&
        fn(prep, buf, len, penLink, usages, nUsages, vx, vy, vp, gx, gy, gp)) {
        usedLink = penLink;
        return true;
    }
    if (fn(prep, buf, len, 0, usages, nUsages, vx, vy, vp, gx, gy, gp)) {
        usedLink = 0;
        return true;
    }
    return false;
}

// 逻辑值是否落在设备声明的量程内。
//
// 只用于**穷举 Report ID** 的采纳判据：HidP 按描述符偏移读字节，不校验
// 数值合理性，因此错误的前缀可能读出「看似合法」的 X/Y，命中后被永久缓存，
// 之后所有报文坐标都是错的，而日志形似成功（parse form=1 brute）。
//
// 刻意不用于形态 0 / 1a：那两种形态的 Report ID 来自设备自己声明的能力表，
// 可信度高得多；若对它们也强制校验，描述符有小瑕疵的未知品牌会从
// 「坐标略偏」直接退化成「完全丢数据」——在无法真机验证的前提下，
// 这个代价比保留可疑数据更高。
static bool LogicalInRange(const Dev& d, LONG vx, LONG vy) {
    if (d.xRel) {
        // 相对模式：LogicalMin 为负，量程是对称的增量范围，
        // 用一个宽松的绝对上界判断，避免把正常增量误判为越界
        LONG lim = (LONG)(d.lxMax > 0 ? d.lxMax : -d.lxMin);
        return vx >= -lim && vx <= lim;
    }
    if (vx < d.lxMin || vx > d.lxMax) return false;
    if (d.yRel) {
        LONG lim = (LONG)(d.lyMax > 0 ? d.lyMax : -d.lyMin);
        return vy >= -lim && vy <= lim;
    }
    return vy >= d.lyMin && vy <= d.lyMax;
}

bool PenInput::TryParseForms(Dev& d, const BYTE* rep, ULONG size,
                             USAGE usages[64], ULONG& nUsages,
                             LONG& vx, LONG& vy, LONG& vp,
                             bool& gx, bool& gy, bool& gp) {
    if (size == 0) return false;
    if (d.scratch.size() < (size_t)size + 8) d.scratch.resize((size_t)size + 8);

    // 已缓存形态：直接按缓存解（缓存里存的就是当时采纳的 link collection）
    if (d.parseCachedForm >= 0) {
        if (d.parseCachedForm == 0) {
            if (FetchWithFallback(hidFetch_, d.preparsed, rep, size, d.penLink,
                                  usages, nUsages, vx, vy, vp, gx, gy, gp, d.parseCachedLink))
                return true;
        } else {
            d.scratch[0] = (BYTE)d.parseCachedId;
            memcpy(d.scratch.data() + 1, rep, size);
            // 缓存路径同样优先试 d.penLink（与 form-0 缓存路径、form-1a/1b
            // 探测路径一致），失败再退回 0。此前这里用 d.parseCachedLink 作
            // 优先值，一旦某次回退成 0，复合设备上后续报文就一直取第一个
            // 匹配字段（可能是触摸集合的 X/Y），失去按笔集合精确取值的能力。
            // 旧 Wacom 橡皮/笔尖在不同 tool 上交替上报时尤其会踩到。
            if (FetchWithFallback(hidFetch_, d.preparsed, d.scratch.data(), size + 1, d.penLink,
                                  usages, nUsages, vx, vy, vp, gx, gy, gp, d.parseCachedLink))
                return true;
        }
        d.parseCachedForm = -1;   // 缓存失效（如换了 Report ID 的子报文），重新探测
    }

    // 形态0：原始直读
    {
        USHORT usedLink = 0;
        if (FetchWithFallback(hidFetch_, d.preparsed, rep, size, d.penLink,
                              usages, nUsages, vx, vy, vp, gx, gy, gp, usedLink)) {
            d.parseCachedForm = 0;
            d.parseCachedId = 0;
            d.parseCachedLink = usedLink;
            if (!d.rawLogged) {
                d.rawLogged = true;
                LogF("[pen] parse form=0 direct (link=%u penLink=%u) (%s)",
                     (unsigned)usedLink, (unsigned)d.penLink, d.name.c_str());
            }
            return true;
        }
    }

    // 形态1a：前缀能力表里收集到的 Report ID
    for (USAGE rid : d.reportIDs) {
        if (rid == 0 || rid > 255) continue;
        d.scratch[0] = (BYTE)rid;
        memcpy(d.scratch.data() + 1, rep, size);
        USHORT usedLink = 0;
        if (FetchWithFallback(hidFetch_, d.preparsed, d.scratch.data(), size + 1, d.penLink,
                              usages, nUsages, vx, vy, vp, gx, gy, gp, usedLink)) {
            d.parseCachedForm = 1;
            d.parseCachedId = rid;
            d.parseCachedLink = usedLink;
            LogF("[pen] parse form=1 reportId=%u link=%u (%s)",
                 (unsigned)rid, (unsigned)usedLink, d.name.c_str());
            return true;
        }
    }

    // 形态1b：穷举 1..255（唯一「猜」Report ID 的路径，故必须校验结果）
    //
    // 判据不能只看 gx && gy：HidP 按描述符偏移读字节，一个错误的前缀也可能
    // 让它读出「看似合法」的 X/Y。这里额外要求逻辑值落在设备声明的量程内，
    // 越界即判为形态不匹配、继续试下一个 ID。
    for (int rid = 1; rid <= 255; ++rid) {
        d.scratch[0] = (BYTE)rid;
        memcpy(d.scratch.data() + 1, rep, size);
        USHORT usedLink = 0;
        if (!FetchWithFallback(hidFetch_, d.preparsed, d.scratch.data(), size + 1, d.penLink,
                               usages, nUsages, vx, vy, vp, gx, gy, gp, usedLink))
            continue;

        if (!LogicalInRange(d, vx, vy)) {
            // 命中了 ID 但数值越界：几乎可以断定是错位数据，不采纳
            ++d.bruteRejected;
            if (!d.bruteWarned) {
                d.bruteWarned = true;
                LogF("[pen] WARN brute reportId=%d 命中但 X/Y 越界(x=%ld y=%ld, 量程=[%ld..%ld,%ld..%ld]) "
                     "-> 判为错位数据，继续尝试其他 ID (%s)",
                     rid, (long)vx, (long)vy, (long)d.lxMin, (long)d.lxMax,
                     (long)d.lyMin, (long)d.lyMax, d.name.c_str());
            }
            continue;
        }

        d.parseCachedForm = 1;
        d.parseCachedId = (USAGE)rid;
        d.parseCachedLink = usedLink;
        if (!d.bruteLogged) {
            d.bruteLogged = true;
            LogF("[pen] parse form=1 brute reportId=%d link=%u x=%ld y=%ld p=%ld "
                 "(穷举命中且通过量程校验；rejected=%d) (%s)",
                 rid, (unsigned)usedLink, (long)vx, (long)vy, (long)vp,
                 d.bruteRejected, d.name.c_str());
        }
        return true;
    }
    return false;
}

void PenInput::OnRawInput(HRAWINPUT hRaw, int64_t nowUs) {
    UINT size = 0;
    if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        if (!dropGet_) { dropGet_ = true; LogF("[pen] DROP: GetRawInputData(size) 失败 err=%lu", GetLastError()); }
        return;
    }
    // 复用成员缓冲，避免每条报文都堆分配一次。
    // 放开注册（PAGEONLY）后触摸屏/触控板每秒可达数百条报文，
    // 这里省下的分配是主要的单条开销之一。
    // 安全性：OnRawInput 只在采集线程被调用（单线程），不会重入。
    if (rawBuf_.size() < size) rawBuf_.resize(size);
    BYTE* buf = rawBuf_.data();
    if (GetRawInputData(hRaw, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) {
        if (!dropGet_) { dropGet_ = true; LogF("[pen] DROP: GetRawInputData(data) 失败 err=%lu", GetLastError()); }
        return;
    }

    RAWINPUT* ri = (RAWINPUT*)buf;
    if (ri->header.dwType != RIM_TYPEHID) {
        if (!dropType_) { dropType_ = true; LogF("[pen] DROP: dwType=%lu 非 HID", (unsigned long)ri->header.dwType); }
        return;
    }
    ++rawReports_;
    // 首帧诊断：能证明 WM_INPUT 已送达（若始终无此行，说明报文没到窗口）
    if (rawReports_ == 1) {
        LogF("[pen] first WM_INPUT arrived (hid, dwSizeHid=%lu dwCount=%lu hDevice=%p)",
             (unsigned long)ri->data.hid.dwSizeHid, (unsigned long)ri->data.hid.dwCount,
             (void*)ri->header.hDevice);
    }

    Dev* dp = FindOrCreate(ri->header.hDevice);
    if (!dp) { if (!dropDev_) { dropDev_ = true; LogF("[pen] DROP: FindOrCreate 返回空"); } return; }
    Dev& d = *dp;

    // 已确认非笔类设备：直接早退。
    // 放开注册后杂设备报文量远大于笔，这一步把它们挡在最前面，
    // 使每条报文的开销只剩上面的一次 map 查找。
    if (d.rejected_) return;

    // 首次收到「此设备」的报文：记录设备身份，确认它是不是数位笔
    if (!d.seenRawLogged) {
        d.seenRawLogged = true;
        LogF("[pen] report from device %s preparsed=%d hasX=%d hasY=%d dwSizeHid=%lu",
             d.name.c_str(), (int)(d.preparsed != nullptr), (int)d.hasX, (int)d.hasY,
             (unsigned long)ri->data.hid.dwSizeHid);
    }

    if (!d.preparsed) {
        if (!LoadDeviceInfo(d)) {
            if (!d.failLogged) { d.failLogged = true; LogF("[pen] DROP: LoadDeviceInfo 失败 dev=%s", d.name.c_str()); }
            return;
        }
        ApplyOverride(d);   // 必须在 BuildMapping 之前（热插拔路径同样适用）
        BuildMapping(d);
        PenDeviceInfo pdi;
        pdi.name = d.name; pdi.type = d.type;
        pdi.hasPressure = d.hasPressure; pdi.pressureMax = (int)d.pressMax;
        pdi.mappingSource = d.mappingSource;
        if (onDeviceFound) onDeviceFound(pdi);
        LogF("[pen] hot-plug type=%d pressure=%d hasX=%d hasY=%d penLink=%u %s",
             (int)d.type, (int)d.hasPressure, (int)d.hasX, (int)d.hasY,
             (unsigned)d.penLink, d.name.c_str());
    }
    if (!d.hasX || !d.hasY) {
        // 无 X/Y = 不是能定位的设备（触控板按键、ExpressKeys 等）。
        // 置 rejected_ 后永久早退；日志仍只记一次，不刷屏。
        if (!d.noXYLogged) {
            d.noXYLogged = true;
            LogF("[pen] skip: 设备无 X/Y 能力，后续报文不再处理 dev=%s", d.name.c_str());
        }
        d.rejected_ = true;
        return;
    }
    // 触控板（0D:05）：有 X/Y 但不是绘图设备。手指移动由驱动合成鼠标事件走鼠标
    // 通道，其 HID 报文对本工具无独立价值。永久早退：不计入笔报文、不建映射、不标定。
    // （真实报告 diagnostics_20261017_233953 中 Elan 04F3 触控板曾 mapSrc=2 误参与映射）
    if (d.type == PEN_TOUCH_PAD) {
        if (!d.touchPadLogged) {
            d.touchPadLogged = true;
            LogF("[pen] skip: 触控板非绘图设备，排除出笔通道 dev=%s", d.name.c_str());
        }
        d.rejected_ = true;
        return;
    }
    ++d.devReports;   // 有 X/Y 能力的设备才计数：杂设备不计，避免误报

    ULONG cnt = ri->data.hid.dwCount;
    ULONG hs  = ri->data.hid.dwSizeHid;
    if (cnt == 0 || hs == 0) {
        if (!d.zeroCnt_) { d.zeroCnt_ = true; LogF("[pen] DROP: dwCount=%lu dwSizeHid=%lu dev=%s", (unsigned long)cnt, (unsigned long)hs, d.name.c_str()); }
        return;
    }
    if (cnt > 16) cnt = 16;

    const BYTE* base = ri->data.hid.bRawData;
    for (ULONG r = 0; r < cnt; ++r) {
        const BYTE* rep = base + (size_t)r * hs;
        PenSample s;
        if (ExtractSample(d, rep, hs, nowUs, s)) {
            ++parsedSamples_;
            ++d.devParsed;
            // 成功路径打印原始报文，但不再是「前 5 条」——改为「前 3 条基线 +
            // 关键跳变（落笔/压感/橡皮/抬笔）各记一次」。原因见 DecideReportLog
            // 注释：报告1 XP-Pen 前 5 条全悬空，旧逻辑额度用尽后落笔数据再也不记。
            const char* tag = "";
            if (DecideReportLog(d.repLog, s.tipDown, s.pressure, s.eraser, tag)) {
                std::string hex;
                char b2[8];
                for (ULONG i = 0; i < std::min<ULONG>(hs, 24u); ++i) {
                    snprintf(b2, sizeof(b2), "%02X ", rep[i]);
                    hex += b2;
                }
                LogF("[pen] OK(%s) dev=%s len=%lu raw=[%s] -> x=%d y=%d p=%.3f tip=%d inRange=%d eraser=%d form=%d",
                     tag, d.name.c_str(), (unsigned long)hs, hex.c_str(),
                     s.x, s.y, s.pressure, (int)s.tipDown, (int)s.inRange,
                     (int)s.eraser, d.parseCachedForm);
            }
            if (onSample) onSample(s);
        } else {
            // 首次失败即记录报文原文（不再等 32 次），并每 256 次记一次
            ++d.parseFails;
            if (d.parseFails == 1 || (d.parseFails % 256 == 0)) {
                std::string hex;
                char b2[8];
                for (ULONG i = 0; i < std::min<ULONG>(hs, 24u); ++i) {
                    snprintf(b2, sizeof(b2), "%02X ", base[i]);
                    hex += b2;
                }
                LogF("[pen] parse FAIL #%d dev=%s len=%lu raw=[%s]",
                     d.parseFails, d.name.c_str(), (unsigned long)hs, hex.c_str());
            }
        }
    }
}

bool PenInput::ExtractSample(Dev& d, const BYTE* rep, ULONG size, int64_t nowUs, PenSample& out) {
    if (size == 0 || !d.preparsed) return false;

    USAGE usages[64]; ULONG nUsages = 0;
    LONG vx = 0, vy = 0, vp = 0;
    bool gx = false, gy = false, gp = false;
    if (!TryParseForms(d, rep, size, usages, nUsages, vx, vy, vp, gx, gy, gp))
        return false;

    bool tip = false, inRange = false, eraser = false, barrel = false, touch = false;
    for (ULONG i = 0; i < nUsages; ++i) {
        switch (usages[i]) {
        case U_DIG_TIP_SWITCH:    tip = true;     break;
        case U_DIG_IN_RANGE:      inRange = true; break;
        case U_DIG_ERASER:        eraser = true;  break;
        case U_DIG_BARREL_SWITCH: barrel = true;  break;
        case U_DIG_TOUCH:         touch = true;   break;
        default: break;
        }
    }

    // 触摸数据直接丢弃，不做任何处理。
    // 纯手指触摸（有 Touch 0x33、无 TipSwitch 0x42）不是笔接触：不计入统计、
    // 不更新笔通道状态、不刷新全局活跃标志，直接返回 false。这样带触控的
    // 数位屏上用手指缩放画布、纯触控屏的手指操作都不会被记成笔画。
    //
    // 位置关键：必须在更新 havePrev/prevX（速度追踪）与 anyInRange_/anyTip_
    // （录屏活跃判定）之前丢弃，否则会污染笔的速度计算并让录屏误判为在作画。
    //
    // 对现有 Wacom 笔零影响：笔用 TipSwitch(0x42) 上报，触摸屏才用 Touch(0x33)，
    // 二者在本机是不同 handle 的不同设备，pen 设备的报文不会带 Touch。
    if (touch && !tip) return false;

    LONG lx = vx, ly = vy;
    int32_t sx, sy;
    if (d.xRel || d.yRel) {
        // 相对模式（数位板设为鼠标模式）：累加增量
        if (!d.haveRelBase) {
            POINT cp; GetCursorPos(&cp);
            d.relX = cp.x; d.relY = cp.y;
            d.haveRelBase = true;
        }
        d.relX += lx; d.relY += ly;
        int vxMax = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vyMax = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int vxMin = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vyMin = GetSystemMetrics(SM_YVIRTUALSCREEN);
        if (d.relX < vxMin) d.relX = vxMin;
        if (d.relY < vyMin) d.relY = vyMin;
        if (d.relX > vxMin + vxMax) d.relX = vxMin + vxMax;
        if (d.relY > vyMin + vyMax) d.relY = vyMin + vyMax;
        sx = (int32_t)d.relX; sy = (int32_t)d.relY;
    } else {
        sx = (int32_t)llround((double)lx * d.sx + d.ox);
        sy = (int32_t)llround((double)ly * d.sy + d.oy);
    }

    float pr = 0.f;
    if (gp && d.hasPressure) {
        double span = (double)(d.pressMax - d.pressMin);
        pr = span > 0 ? (float)((vp - d.pressMin) / span) : 0.f;
        if (pr < 0.f) pr = 0.f;
        if (pr > 1.f) pr = 1.f;
    } else {
        pr = tip ? 1.0f : 0.f;   // 无压力传感器时笔尖按下视为 1.0（触摸已在前面丢弃）
    }
    out.pressure = pr;

    float sp = 0.f;
    if (d.havePrev) {
        int64_t dt = nowUs - d.prevT;
        if (dt > 0) {
            double dx = (double)(sx - d.prevX), dy = (double)(sy - d.prevY);
            sp = (float)(sqrt(dx * dx + dy * dy) * 1000000.0 / (double)dt);
        }
    }
    d.havePrev = true; d.prevT = nowUs; d.prevX = sx; d.prevY = sy; d.prevTip = tip;

    out.tUs       = nowUs;
    out.x = sx; out.y = sy;
    out.speed     = sp;
    out.tipDown   = tip;              // 仅笔尖 TipSwitch(0x42)；触摸已早退丢弃
    out.touch     = touch;            // 保留标志供诊断（此路径 touch 恒为 false 或与 tip 并存）
    out.inRange   = inRange || out.tipDown;
    out.eraser    = eraser;
    out.barrel    = barrel;
    out.device    = d.type;
    out.devSeq    = d.devSeq;         // 采集层按此分桶，多设备笔画不交织
    out.logicalX  = lx;
    out.logicalY  = ly;

    d.lastLogicalX = lx; d.lastLogicalY = ly; d.lastLogicalT = nowUs;

    anyInRange_ = out.inRange;
    anyTip_     = out.tipDown;
    return true;
}

} // namespace pb
