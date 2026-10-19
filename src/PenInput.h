#pragma once
// ============================================================
//  数位笔输入：Raw Input + HID 解析
//
//  设计要点
//  1) 动态枚举 Usage Page 0x0D（Digitizer）下的**全部顶级集合**并解析能力，
//     因此天然覆盖 External Pen / Integrated Pen / Touch Screen 及任何数位板、
//     数位屏，无需品牌白名单。注册用 RIDEV_PAGEONLY（见 Register 的说明）。
//  2) 与鼠标并行工作：Raw Input 独立于系统鼠标消息流。
//  3) 坐标映射用 GetPointerDeviceProperties 的**逻辑量程**与 HID 侧同源比对
//     （GetPointerDeviceRects 的 device 是屏幕像素、display 是物理毫米，
//     都不能与逻辑量程直接比），命中后用 device 的像素矩形建立映射；
//     多候选且矩形不一致时拒绝猜测，退化为虚拟桌面线性映射；
//     仍可用鼠标事件反解出的样本做标定修正。
//  4) 相对模式（数位板设为鼠标模式）自动累加增量。
//  5) 描述符不规范或映射歧义无法自动消解时，可用 pen_overrides.json 人工覆写
//     （见 PenOverrides.h）。
// ============================================================
#include "Common.h"
#include "PenInputDev.h"
#include "PenOverrides.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <string>
#include <vector>
#include <functional>
#include <map>

#pragma comment(lib, "hid.lib")

namespace pb {

// 一次笔采样（已映射为屏幕坐标）
struct PenSample {
    int64_t tUs     = 0;
    int32_t x = 0, y = 0;   // 屏幕坐标
    float   speed   = 0.f;  // 像素/秒
    float   pressure= 0.f;  // 0..1
    bool    tipDown = false;   // 笔尖开关 TipSwitch(0x42)——真正的「笔接触」
    bool    touch   = false;   // 手指触摸 Touch(0x33)——触控屏/带触控数位屏的手指
    bool    inRange = false;
    bool    eraser  = false;
    bool    barrel  = false;
    uint8_t device  = PEN_UNKNOWN;
    uint32_t devSeq = 0;       // 设备序号（分桶用，见 Dev::devSeq）
    int32_t logicalX = 0, logicalY = 0; // 原始逻辑坐标（标定用）
};

struct PenDeviceInfo {
    std::string name;
    uint8_t     type = PEN_UNKNOWN;
    bool        hasPressure = false;
    int         pressureMax = 0;
    int         mappingSource = 0; // 0无 1指针API 2无候选(虚拟桌面兜底) 3标定 4候选歧义(兜底) 5覆写指定
    uint64_t    devReports = 0;    // 本设备收到的报文条数
    uint64_t    devParsed  = 0;    // 本设备成功解析的采样数（0 且有报文 = 解析坏了）
};

class PenInput {
public:
    // pageOnly=true：用 RIDEV_PAGEONLY 注册整个 Usage Page 0x0D 下的所有
    // 顶级集合（TLC），一次性覆盖任何品牌/任何 usage 的数位设备，杜绝
    // 「笔落在未注册 usage 上 → 完全无数据且日志不报错」。
    // pageOnly=false：回退到旧的显式 4-usage 清单（01/02/04/0C），
    // 仅用于排查放开注册是否引入问题时做 A/B 对比。
    bool Register(HWND hwnd, bool pageOnly = true);

    void Unregister();
    bool IsRegistered() const { return registered_; }

    void OnRawInput(HRAWINPUT hRaw, int64_t nowUs);

    std::function<void(const PenSample&)>       onSample;
    std::function<void(const PenDeviceInfo&)>   onDeviceFound;

    std::vector<PenDeviceInfo> Devices() const;
    bool InRange() const { return anyInRange_; }
    bool TipDown()   const { return anyTip_; }

    // 诊断：收到的原始报文数 / 成功解析的采样数
    void Stats(uint64_t& rawReports, uint64_t& parsedSamples) const {
        rawReports = rawReports_;
        parsedSamples = parsedSamples_;
    }

    // 用鼠标事件反解出的 (逻辑, 屏幕) 对改进映射。
    // devSeq 精确指定要标定哪台设备（见 Dev::devSeq）；传 0 表示未知，
    // 此时退回按 type 匹配（兼容旧调用，但多同类设备时可能互相污染）。
    void AddCalibrationSample(uint32_t devSeq, uint8_t deviceType,
                              int32_t lx, int32_t ly, int32_t sx, int32_t sy);

    static const char* DeviceTypeName(uint8_t t);

    // Ink 回退路径用：当前识别到的笔设备类型（数位屏 / 外接数位板）。
    // 回退路径处理的是系统注入的伪鼠标事件，没有对应的 HID 设备句柄，
    // 无法像原始路径那样直接从 Dev::type 取值——此前只能硬编码 PEN_EXTERNAL，
    // 导致数位屏在 Ink 开启时被错记成「外接数位板」。
    // 这里复用 BuildMapping 已判定好的类型：取「有压感的笔设备」的类型；
    // 若存在多个且类型不一致（无法判定），返回 PEN_UNKNOWN 让调用方退回默认值。
    uint8_t DominantPenType() const;

    // 是否存在**任何**「有压感的笔设备」（数位板 / 数位屏）。
    // 与 DominantPenType 的区别：后者在数位屏与数位板并存、类型冲突时按设计
    // 返回 PEN_UNKNOWN（它要回答「回退笔画该记成哪种设备」，不能猜）；而本函数
    // 只回答「机器上到底有没有笔」，冲突时依然为 true。最后一层回退的门控必须
    // 用这个 —— 报告 20261018_155346 同时接了数位屏与数位板，用 DominantPenType
    // 会得到 UNKNOWN 从而误关回退。
    bool HasPressurePenDevice() const;

    // ========================================================
    //  离线测试接缝
    //
    //  为什么需要它：PHIDP_PREPARSED_DATA 无法离线构造 —— Windows SDK
    //  里没有任何接受「原始 report descriptor 字节数组」的函数，它只能
    //  从真实设备句柄取得（HidD_GetPreparsedData / RIDI_PREPARSEDDATA）。
    //  而开发者只有 Wacom，无法为 Huion / XP-Pen / Gaomon 等品牌造真机
    //  测试。因此把「唯一直接接触 HidP_* 的这一步」抽成可替换的函数，
    //  测试时注入按参数直填 X/Y/压感/Usage 列表的 mock，就能在没有对应
    //  硬件的情况下驱动 ExtractSample → 坐标映射 → 压感归一 全链路。
    //
    //  品牌差异真正体现的地方不是字节偏移（HidP 已按描述符处理，且
    //  HIDP_VALUE_CAPS 根本没有 BitOffset 字段），而是「解析出的逻辑值
    //  如何被解释」——这些全都可以离线验证。
    // ========================================================

    // 从一条 HID 报文取出 Usage 列表与 X/Y/压力逻辑值。
    // linkCollection = 笔所在集合索引，0 = 不指定（取第一个同 Usage 字段）。
    // 返回 false 表示该报文形态不匹配（调用方继续尝试下一个 Report ID）。
    using HidFetchFn = std::function<bool(PHIDP_PREPARSED_DATA prep,
                                          const BYTE* buf, ULONG len,
                                          USHORT linkCollection,
                                          USAGE* usages, ULONG& nUsages,
                                          LONG& vx, LONG& vy, LONG& vp,
                                          bool& gx, bool& gy, bool& gp)>;

    // 生产环境的真实实现（直接调 HidP_GetUsages / HidP_GetUsageValue）
    static bool RealHidFetch(PHIDP_PREPARSED_DATA prep, const BYTE* buf, ULONG len,
                             USHORT linkCollection,
                             USAGE* usages, ULONG& nUsages,
                             LONG& vx, LONG& vy, LONG& vp,
                             bool& gx, bool& gy, bool& gp);

    // 仅供测试：替换 HID 取值实现；传空则恢复真实实现
    void SetHidFetchForTest(HidFetchFn fn) {
        if (fn) hidFetch_ = std::move(fn);
        else    hidFetch_ = HidFetchFn(&PenInput::RealHidFetch);
    }

    // 以下两个解析入口对测试开放（刻意不用 friend：侵入更强，且项目
    // 已有 AddCalibrationSample 这类操作内部 Dev 状态的公开方法）
    bool ExtractSample(Dev& d, const BYTE* rep, ULONG size, int64_t nowUs, PenSample& out);
    bool TryParseForms(Dev& d, const BYTE* rep, ULONG size,
                       USAGE usages[64], ULONG& nUsages,
                       LONG& vx, LONG& vy, LONG& vp,
                       bool& gx, bool& gy, bool& gp);

    // 指针设备候选消歧（BuildMapping 用）。对测试开放：多屏映射无法在单屏
    // 开发机上真机验证，把判定抽成纯函数才能离线覆盖各种候选组合。
    struct PtrCand {
        RECT   rect{};          // 屏幕像素矩形（GetPointerDeviceRects 的 device）
        bool   hasMonitor = false;   // 是否绑定到具体显示器
        int    type = 0;        // POINTER_DEVICE_TYPE_*
    };
    // 返回选中候选的下标；ambiguous 置位表示拒绝猜测。
    // virtualRect 仅供测试注入（nullptr = 用 GetSystemMetrics 取真实虚拟桌面），
    // 使消歧规则不依赖开发机的多屏配置即可离线验证。
    static int PickPointerCandidate(const PtrCand* c, size_t n, bool& ambiguous,
                                    const RECT* virtualRect = nullptr);

    // 设备类型判定（映射与类型分离）。对测试开放。
    // types[i] = POINTER_DEVICE_TYPE_*，hasMon[i] = 该候选是否绑定显示器（0/1）。
    // 用 uint8_t 而非 bool：std::vector&lt;bool&gt; 是代理引用的特化容器、没有 data()。
    // 规则：优先用「绑定了显示器」的候选的类型（硬件形态真相），
    // 无绑定候选时用选中者 bestIdx 的类型。
    // 返回值：PEN_EXTERNAL / PEN_INTEGRATED / PEN_TOUCH_SCREEN / PEN_UNKNOWN。
    static uint8_t ResolvePenType(const int* types, const uint8_t* hasMon,
                                  size_t n, size_t bestIdx);

    // 标定拟合 + 退化解校验（AddCalibrationSample 用）。对测试开放。
    //
    // 为什么需要校验：真实用户报告（diagnostics_20261017_233953.txt）实证，
    // 最小二乘在「笔大幅移动但光标几乎不动」的退化样本上会拟合出
    // sy≈0.0033（正常≈0.044=屏幕高/逻辑量程）的病态解，且 oy≈1889 离谱。
    // 旧代码只校验 ay>1e-6，退化解照样采纳 → mapSrc=3 后笔坐标严重错位、
    // 污染后续数据。这里在采纳前做统计质量校验，拦住退化解。
    struct CalibResult {
        bool   accept = false;              // 是否采纳此次标定
        double sx = 1, ox = 0, sy = 1, oy = 0;
        double rx2 = 0, ry2 = 0;            // X/Y 方向拟合优度 R²（诊断用）
        const char* reason = "";            // 拒绝原因（诊断用，指向静态字面量）
    };
    // lx/sx = 逻辑X/屏幕X 样本对；ly/sy = 逻辑Y/屏幕Y 样本对；n = 样本数。
    // logRangeX/Y = 设备逻辑满量程（lxMax-lxMin 等），用于斜率合理性校验：
    //   拟合斜率 × 满量程 = 该设备整个活动区映射到的屏幕像素跨度，必须是
    //   合理的显示器尺寸（既拦退化解的过小斜率，也拦异常放大的斜率）。
    // 拒绝条件（任一）：样本不足、逻辑跨度过小、屏幕跨度过小（光标几乎没动 →
    // 斜率不可靠）、拟合优度 R² 过低（非线性/噪声）、斜率非正、斜率不合理。
    static CalibResult EvaluateCalibration(const int32_t* lx, const int32_t* sx,
                                           const int32_t* ly, const int32_t* sy,
                                           size_t n, double logRangeX, double logRangeY);

    // 设备类型初判（LoadDeviceInfo 用）。对测试开放：真实 HID preparsed 无法离线
    // 构造，把「topUsage + hasPressure → 类型」的判定抽成纯函数，才能覆盖触控板
    // (0D:05) 等各分支——此前触控板落入 else 被误标为触摸屏并参与笔映射。
    struct DevTypeResult { uint8_t type = PEN_UNKNOWN; bool needPointerType = false; };
    // topUsage 传 HID Digitizer 顶级 usage 裸值（0x02 笔 / 0x04 触摸屏 /
    // 0x05 触控板 / 0x0C 多点）；needPointerType 表示是否待 BuildMapping 用指针类型细分。
    static DevTypeResult ClassifyDeviceType(USAGE topUsage, bool hasPressure);

    // 报文诊断日志决策（OnRawInput 用）。对测试开放：真实报告里 XP-Pen 前 5 条
    // 全悬空、落笔数据用尽额度后不再记录，是排查盲点。改为「前 3 条基线 + 关键
    // 状态跳变各记一次」。抽成纯函数才能在无对应硬件下覆盖各种报文序列。
    // 返回：log=是否记录这条；tag=记录原因（静态字面量）；并按需更新 st。
    // st 传入传出：函数内部推进状态机（baseline 计数、跳变标志、lastTip/lastPressure）。
    static bool DecideReportLog(ReportLogState& st, bool tip, float pressure,
                                bool eraser, const char*& tag);

    // pb.log 诊断扫描（ExportDiagnostics 用）。对测试开放。
    //
    // 为什么需要：诊断报告的「采集状态」计数是**当前会话**累计，而下方
    // 「pb.log 摘录」是**跨会话历史**。报告1（diagnostics_20261017_233222）里
    // 提示「从未收到任何数位设备报文」，紧接着日志就有上一会话的 OK 行——
    // 自相矛盾，误导排查。扫描日志区分二者，才能给出准确提示。
    struct PenLogScan {
        int  sessions = 0;        // 出现的会话数（以 "registered digitizer usages" 为界）
        bool everGotReport = false;  // 历史中是否出现过成功解析的笔报文（OK 行）
        bool everParsedDev = false;  // 历史中是否出现过设备解析（report from device / OK）
        int  okLines = 0;         // OK 行总数
    };
    // 扫描日志全文（UTF-8 文本），统计会话数与历史笔活动，供导出报告判断措辞。
    static PenLogScan ScanPenLog(const std::string& logText);

    // 覆写规则（逃生舱口）。SetOverrides 供测试注入，生产由 Register 时加载。
    void SetOverrides(std::vector<PenOverride> ovs) { overrides_ = std::move(ovs); }
    const std::vector<PenOverride>& Overrides() const { return overrides_; }
    // 对测试开放：覆写的价值在于「真的生效」，只测解析等于没测。
    // 生产由 LoadDeviceInfo 之后、BuildMapping 之前调用。
    void ApplyOverride(Dev& d);

private:
    HidFetchFn hidFetch_ = &PenInput::RealHidFetch;

    Dev* FindOrCreate(HANDLE h);
    bool LoadDeviceInfo(Dev& d);
    void BuildMapping(Dev& d);

    std::vector<PenOverride> overrides_;   // 启动时从 pen_overrides.json 加载

    std::map<HANDLE, Dev> devs_;
    uint32_t nextDevSeq_ = 0;    // 设备序号分配计数器（从 1 开始，0 保留给未知）
    std::vector<BYTE> rawBuf_;   // OnRawInput 复用的读取缓冲，避免每条报文堆分配
    bool registered_ = false;
    bool pageOnly_ = true;      // 本次注册是否用了 RIDEV_PAGEONLY（注销需一致）
    HWND hwnd_ = nullptr;
    bool anyInRange_ = false;
    bool anyTip_ = false;
    uint64_t rawReports_ = 0;
    uint64_t parsedSamples_ = 0;
    // 报文入口各丢弃分支的「只记一次」标记
    bool dropGet_ = false;
    bool dropType_ = false;
    bool dropDev_ = false;
};

} // namespace pb
