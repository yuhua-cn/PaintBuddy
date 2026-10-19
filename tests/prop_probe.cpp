// ============================================================
//  指针设备属性探针：dump GetPointerDeviceProperties 的真实内容
//
//  目的：本机 5 个数位设备的坐标映射 score 全部高达 1.5~1.7，
//  官方指针映射（mapSrc=1）从未成功过，全部退化到虚拟桌面兜底。
//  怀疑是拿 GetPointerDeviceRects 的 device rect 与 HID 逻辑范围比对口径不符。
//
//  本探针同时打印两套数据做对照：
//    A) GetPointerDeviceRects 的 device / display 矩形
//    B) GetPointerDeviceProperties 的 logicalMin/Max + usagePageId/usageId
//  以及 HID 侧的 X/Y 逻辑范围，用于确认哪一套能与 HID 精确对上。
// ============================================================
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")

static std::string W2U_(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static const char* PtrTypeName(int t) {
    switch (t) {
    case POINTER_DEVICE_TYPE_INTEGRATED_PEN: return "INTEGRATED_PEN(数位屏)";
    case POINTER_DEVICE_TYPE_EXTERNAL_PEN:   return "EXTERNAL_PEN(数位板)";
    case POINTER_DEVICE_TYPE_TOUCH:          return "TOUCH(触摸屏)";
    case POINTER_DEVICE_TYPE_TOUCH_PAD:      return "TOUCH_PAD(触控板)";
    default: return "UNKNOWN";
    }
}

static const char* UsageName(USHORT page, USHORT usage) {
    if (page == 0x01 && usage == 0x30) return "Generic:X";
    if (page == 0x01 && usage == 0x31) return "Generic:Y";
    if (page == 0x0D && usage == 0x30) return "Digitizer:TipPressure";
    if (page == 0x0D && usage == 0x32) return "Digitizer:InRange";
    if (page == 0x0D && usage == 0x42) return "Digitizer:TipSwitch";
    static char b[48];
    snprintf(b, sizeof(b), "%02X:%02X", page, usage);
    return b;
}

int main() {
    printf("==== A) GetPointerDevices 枚举 ====\n");
    UINT32 count = 0;
    if (!GetPointerDevices(&count, nullptr) || count == 0) {
        printf("  未发现任何指针设备 (err=%lu)\n", GetLastError());
        return 1;
    }
    if (count > 64) count = 64;
    std::vector<POINTER_DEVICE_INFO> infos(count);
    if (!GetPointerDevices(&count, infos.data())) {
        printf("  GetPointerDevices 失败 err=%lu\n", GetLastError());
        return 1;
    }
    printf("  共 %u 个指针设备\n\n", count);

    for (UINT32 i = 0; i < count; ++i) {
        printf("---- [%u] type=%d %s ----\n", i, (int)infos[i].pointerDeviceType,
               PtrTypeName((int)infos[i].pointerDeviceType));
        printf("  productString : %s\n", W2U_(infos[i].productString).c_str());
        printf("  monitor       : %p  displayOrientation=%lu  maxActiveContacts=%u\n",
               (void*)infos[i].monitor, (unsigned long)infos[i].displayOrientation,
               (unsigned)infos[i].maxActiveContacts);

        // A) GetPointerDeviceRects
        RECT disp{}, dev{};
        if (GetPointerDeviceRects(infos[i].device, &disp, &dev)) {
            printf("  Rects.device  : [%ld,%ld - %ld,%ld]  宽高=%ldx%ld\n",
                   dev.left, dev.top, dev.right, dev.bottom,
                   dev.right - dev.left, dev.bottom - dev.top);
            printf("  Rects.display : [%ld,%ld - %ld,%ld]  宽高=%ldx%ld\n",
                   disp.left, disp.top, disp.right, disp.bottom,
                   disp.right - disp.left, disp.bottom - disp.top);
        } else {
            printf("  Rects         : 获取失败 err=%lu\n", GetLastError());
        }

        // B) GetPointerDeviceProperties
        UINT32 pc = 0;
        if (GetPointerDeviceProperties(infos[i].device, &pc, nullptr) && pc > 0) {
            if (pc > 32) pc = 32;
            std::vector<POINTER_DEVICE_PROPERTY> props(pc);
            if (GetPointerDeviceProperties(infos[i].device, &pc, props.data())) {
                printf("  Properties    : %u 条\n", pc);
                for (UINT32 k = 0; k < pc; ++k) {
                    printf("    [%u] %-22s logical=[%ld..%ld] physical=[%ld..%ld] "
                           "unit=%lu exp=%ld\n",
                           k, UsageName(props[k].usagePageId, props[k].usageId),
                           (long)props[k].logicalMin, (long)props[k].logicalMax,
                           (long)props[k].physicalMin, (long)props[k].physicalMax,
                           (unsigned long)props[k].unit, (long)props[k].unitExponent);
                }
            } else {
                printf("  Properties    : 读取失败 err=%lu\n", GetLastError());
            }
        } else {
            printf("  Properties    : 无 (count=%u err=%lu)\n", pc, GetLastError());
        }

        // C) GetPointerDeviceCursors
        UINT32 cc = 0;
        if (GetPointerDeviceCursors(infos[i].device, &cc, nullptr) && cc > 0) {
            if (cc > 8) cc = 8;
            std::vector<POINTER_DEVICE_CURSOR_INFO> curs(cc);
            if (GetPointerDeviceCursors(infos[i].device, &cc, curs.data())) {
                printf("  Cursors       : %u 个 ->", cc);
                for (UINT32 k = 0; k < cc; ++k) {
                    const char* nm =
                        curs[k].cursor == POINTER_DEVICE_CURSOR_TYPE_TIP    ? "TIP" :
                        curs[k].cursor == POINTER_DEVICE_CURSOR_TYPE_ERASER ? "ERASER" : "?";
                    printf(" %s(id=%u)", nm, curs[k].cursorId);
                }
                printf("\n");
            }
        }
        printf("\n");
    }

    // ---- HID 侧逻辑范围对照 ----
    printf("==== B) HID Raw Input 侧 X/Y 逻辑范围（对照用）====\n");
    UINT nd = 0;
    GetRawInputDeviceList(nullptr, &nd, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(nd);
    if (nd) GetRawInputDeviceList(list.data(), &nd, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < nd; ++i) {
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
        UINT sz = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == (UINT)-1) continue;
        if (info.dwType != RIM_TYPEHID) continue;
        if (info.hid.usUsagePage != 0x0D) continue;

        UINT cb = 0;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_PREPARSEDDATA, nullptr, &cb) == (UINT)-1 || cb == 0)
            continue;
        std::vector<BYTE> blob(cb);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_PREPARSEDDATA, blob.data(), &cb) == (UINT)-1)
            continue;
        HIDP_CAPS caps{};
        if (HidP_GetCaps((PHIDP_PREPARSED_DATA)blob.data(), &caps) != HIDP_STATUS_SUCCESS) continue;

        printf("---- HID usage=%02X:%02X VID=%04lX PID=%04lX ----\n",
               info.hid.usUsagePage, info.hid.usUsage,
               info.hid.dwVendorId, info.hid.dwProductId);
        printf("  InputValueCaps : %u 条\n", caps.NumberInputValueCaps);
        if (caps.NumberInputValueCaps > 0 && caps.NumberInputValueCaps < 64) {
            std::vector<HIDP_VALUE_CAPS> vc(caps.NumberInputValueCaps);
            USHORT n = caps.NumberInputValueCaps;
            if (HidP_GetValueCaps(HidP_Input, vc.data(), &n, (PHIDP_PREPARSED_DATA)blob.data()) == HIDP_STATUS_SUCCESS) {
                vc.resize(n);
                for (USHORT k = 0; k < n; ++k) {
                    USAGE u = vc[k].IsRange ? vc[k].Range.UsageMin : vc[k].NotRange.Usage;
                    printf("    [%u] %-22s reportId=%u link=%u logical=[%ld..%ld] physical=[%ld..%ld] bitSize=%u\n",
                           k, UsageName(vc[k].UsagePage, u),
                           (unsigned)vc[k].ReportID, (unsigned)vc[k].LinkCollection,
                           (long)vc[k].LogicalMin, (long)vc[k].LogicalMax,
                           (long)vc[k].PhysicalMin, (long)vc[k].PhysicalMax,
                           (unsigned)vc[k].BitSize);
                }
            }
        }
        printf("\n");
    }
    return 0;
}
