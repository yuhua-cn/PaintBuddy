#include "Capture.h"
#include "Config.h"
#include "Store.h"
#include "PenInput.h"
#include "PenInject.h"   // 签名无关的笔注入判定（IMDT_PEN）
#include <set>
#include <map>

// Windows Ink / 触摸注入鼠标事件的签名
#ifndef MOUSEEVENTF_FROMTOUCH
#define MOUSEEVENTF_FROMTOUCH 0xFF515700
#endif
#define PB_INJECT_SIG_MASK 0xFFFFFF00

namespace pb {

// ============================================================
//  RAII 临界区
// ============================================================
struct CsLock {
    CRITICAL_SECTION& cs;
    explicit CsLock(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
    ~CsLock() { LeaveCriticalSection(&cs); }
};

static const UINT WM_PB_START   = WM_APP + 1;
static const UINT WM_PB_STOP    = WM_APP + 2;
static const UINT TID_FLUSH     = 1;   // 周期性收尾落盘
static const UINT TID_FGPOLL    = 2;   // 前台状态兜底轮询
static const int  FLUSH_MS      = 250;
static const int  FGPOLL_MS     = 400;

// ============================================================
//  键名（复用 Store 的公共实现，避免两份定义漂移）
// ============================================================
static std::string VkName(int vk) {
    return DescribeKey(vk, PB_MOD_NONE);
}

static uint8_t NormalizeModVk(int vk) {
    switch (vk) {
    case VK_LSHIFT: case VK_RSHIFT:     return PB_MOD_SHIFT;
    case VK_LCONTROL: case VK_RCONTROL: return PB_MOD_CTRL;
    case VK_LMENU: case VK_RMENU:       return PB_MOD_ALT;
    case VK_LWIN: case VK_RWIN:         return PB_MOD_WIN;
    default: return PB_MOD_NONE;
    }
}
static bool IsModifierVk(int vk) { return NormalizeModVk(vk) != PB_MOD_NONE; }
static int ModIndex(int vk) {
    uint8_t b = NormalizeModVk(vk);
    if (b & PB_MOD_CTRL)  return 0;
    if (b & PB_MOD_SHIFT) return 1;
    if (b & PB_MOD_ALT)   return 2;
    if (b & PB_MOD_WIN)   return 3;
    return -1;
}
static uint8_t CurrentMods() {
    uint8_t m = PB_MOD_NONE;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= PB_MOD_CTRL;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= PB_MOD_SHIFT;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= PB_MOD_ALT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) m |= PB_MOD_WIN;
    return m;
}
static std::string ModPrefix(uint8_t m) {
    std::string s;
    if (m & PB_MOD_CTRL)  s += "Ctrl+";
    if (m & PB_MOD_SHIFT) s += "Shift+";
    if (m & PB_MOD_ALT)   s += "Alt+";
    if (m & PB_MOD_WIN)   s += "Win+";
    return s;
}

// ============================================================
//  实现体
// ============================================================
struct Capture::Impl {
    Capture*      self = nullptr;
    HWND          hwnd = nullptr;
    HHOOK         kbHook = nullptr;
    HHOOK         msHook = nullptr;
    HWINEVENTHOOK fgHook = nullptr;
    PenInput      pen;
    bool          penRegistered = false;
    bool          initOk = false;

    // —— 前台 ——
    ForegroundState fg;
    bool            fgValid = false;

    // —— 键盘 ——
    struct PendingKey {
        bool    active = false;
        int     vk = 0;
        uint8_t mods = 0;
        int64_t firstUs = 0, lastUs = 0;
        int     count = 0;
        OpKind  kind = OpKind::Normal;
        bool    penActive = false;
        void Reset() { *this = PendingKey(); }
    } pending;

    struct ModPending {
        bool    active = false;
        bool    consumed = false;   // 已作为组合键的一部分使用
        int64_t firstUs = 0, lastUs = 0;
        int     count = 0;
        void Reset() { *this = ModPending(); }
    };
    ModPending modPend[4];           // 0=Ctrl 1=Shift 2=Alt 3=Win

    // —— 滚轮待合并（与键盘 pending 同构）——
    // 一次物理滚动在系统层不是一条消息：每 120 delta 就产生一条 WM_MOUSEWHEEL，
    // 自由滚轮鼠标甩一下可达几十条。此前每条都直接落盘并计数，
    // 于是「今日操作」里一次滚动被记成几十次。
    // 处理方式与键盘完全一致：窗口内合并，时间戳取**第一次**，累计次数。
    struct WheelPending {
        bool    active = false;
        int64_t firstUs = 0, lastUs = 0;   // firstUs 作为记录时间戳（只记第一次）
        int     count = 0;                 // 合并进来的滚轮消息数
        void Reset() { *this = WheelPending(); }
    } wheelPend;

    std::set<int> keysDown;

    // —— 鼠标 ——
    struct FreeMove {
        bool    active = false;
        int64_t firstUs = 0, lastUs = 0;
        bool    kbAttr = false;      // 段起始时有键按住 → 归为键盘操作
        std::vector<PathPoint> pts;
        void Reset() { *this = FreeMove(); }
    } freeMove;

    struct ActiveStroke {
        bool    active = false;
        int64_t downUs = 0, lastUs = 0;
        int     button = 0;
        int32_t sx = 0, sy = 0, ex = 0, ey = 0;
        std::vector<PathPoint> pts;
        bool    fromPen = false;
        bool    kbAttr = false;      // 按下瞬间有键按住 → 记为键盘操作
        void Reset() { *this = ActiveStroke(); }
    } mStroke;

    // —— 数位笔 ——
    struct ActivePen {
        bool    inStroke = false;
        bool    inHover = false;
        int64_t downUs = 0, lastUs = 0, lastTUs = 0;
        uint8_t device = PEN_UNKNOWN;
        uint32_t devSeq = 0;      // 本桶对应的设备序号（= map 的 key）
        bool    eraser = false;
        bool    kbAttr = false;
        float   maxP = 0, sumP = 0, maxS = 0, sumS = 0, len = 0;
        int     nPts = 0;
        int32_t lastX = 0, lastY = 0;
        bool    haveLast = false;
        std::vector<PenPoint> pts;
        // 标定
        int32_t calLX = 0, calLY = 0;
        int64_t calT = 0;
        // Reset 保留 devSeq：桶身份不随笔画收尾而丢失，
        // 调用方持有的引用在 Flush 后仍指向同一设备的有效桶。
        void Reset() { uint32_t keep = devSeq; *this = ActivePen(); devSeq = keep; }
    };

    // 按设备序号分桶。
    // 为什么：此前是单一 pStroke，所有数位设备的采样都汇进这一条逻辑笔。
    // 数位屏 + 数位板同时插着、或两块板交替运笔时，采样会交织进同一条笔画
    // —— 轨迹跳变、速度爆表、压感错乱，且落盘后无法事后修复（轨迹点已合并）。
    // 分桶后每台设备各自独立累积、独立收尾。devSeq 见 PenInputDev.h。
    std::map<uint32_t, ActivePen> penStrokes_;

    ActivePen& StrokeFor(uint32_t devSeq) {
        ActivePen& ps = penStrokes_[devSeq];
        ps.devSeq = devSeq;   // 新建的桶默认 devSeq=0，这里补上真实序号
        return ps;
    }
    // 任一设备有落笔笔画进行中
    bool AnyPenStroke() const {
        for (const auto& kv : penStrokes_) if (kv.second.inStroke) return true;
        return false;
    }
    // 任一设备有笔画或悬空段进行中
    bool AnyPenActive() const {
        for (const auto& kv : penStrokes_)
            if (kv.second.inStroke || kv.second.inHover) return true;
        return false;
    }

    int64_t lastPenSampleUs = 0;
    int32_t lastPenX = 0, lastPenY = 0;

    // —— Windows Ink 回退采集 ——
    // 现象：数位笔开启 Windows Ink（Wacom 默认开）后，笔数据被系统墨迹管线
    // 接管，不再向后台的 Raw Input 监听窗口投递原始 HID 报文（日志表现为
    // 从无 "first WM_INPUT arrived"，且出现 VID_0000&PID_0000 的 Ink 虚拟笔设备）。
    // 此时笔会生成「注入的鼠标事件」（dwExtraInfo 带 FROMTOUCH 签名），
    // 低级鼠标钩子是全局的、一定能收到。故：当原始 HID 笔通道静默时，
    // 把这些注入鼠标事件回收为画笔笔画。
    // 压感：Ink 促进事件的 dwExtraInfo 低 8 位携带 pointerId，
    // 用 GetPointerPenInfo 查系统全局笔状态即可拿到真实压力（0..1024），
    // 非侵入、不抢前台焦点；查不到时退回 1.0（不影响轨迹/分类）。
    int64_t lastRealPenSampleUs_ = 0;   // 最近一次真实 HID 笔采样时刻
    // 是否收到过**落笔**采样（tip=1）。与 lastRealPenSampleUs_ 严格区分：
    // CSP 开启 Wintab 后驱动独占笔尖数据，Raw Input 侧只剩悬停报文
    // （报告 20261018_155346 实证：penSamples=759 全为 tip=0、penStrokes=0、
    // 而 mouseStrokes=182、undo=11 说明用户确实在作画）。若按「有无任何采样」
    // 判断笔通道存活，悬停数据会永久关闭注入回退与告警 —— 缺陷⑦。
    bool everPenTipDown_ = false;
    // 注入路径是否已识别出笔（三判据任一命中）。命中说明能拿到压感与橡皮，
    // 语义比最后一层回退准确，此时不得退化到鼠标回退（见 ShouldUseMouseFallback）。
    bool everInjectedPen_ = false;
    bool mouseFallbackLogged_ = false;   // 最后一层回退只记一次日志
    struct InjectedPen {
        bool    inStroke = false;
        int64_t downUs = 0, lastUs = 0, lastTUs = 0;
        int32_t lastX = 0, lastY = 0;
        bool    haveLast = false;
        bool    kbAttr = false;
        float   maxS = 0, sumS = 0, len = 0;
        float   maxP = 0, sumP = 0;      // 压感（来自 GetPointerPenInfo）
        int     nPts = 0;
        int     nPressure = 0;           // 成功取到压感的采样数
        UINT32  pointerId = 0;           // 当前笔画的 pointer id
        bool    sawEraser = false;       // 本次笔画中是否出现过橡皮（PEN_FLAG_ERASER）
        bool    fromMouseFallback = false; // 最后一层回退（无压感、来源为鼠标左键）
        std::vector<PenPoint> pts;
        void Reset() { *this = InjectedPen(); }
    } injPen;
    bool injPenLogged = false;
    bool penPressureLogged = false;
    bool penPressureFailedLogged = false;
    bool penWatchdogLogged_ = false;   // 看门狗只记一次日志，避免刷屏
    bool touchDropLogged = false;      // Ink 回退路径丢弃手指触摸，只记一次
    bool unusualPenSigLogged_ = false; // 非标准签名的笔被识别，只记一次（诊断证据）
    bool unrecInjectedLogged_ = false; // 未识别的注入左键按下，只记一次（诊断证据）

    CaptureCounters counters;

    // ========================================================
    //  会话级状态复位（跨会话必须能再次触发最后一层回退）
    //
    //  不变量：上面这些标志的作用域是**单个采集会话**。用户明确要求「跨会话
    //  可以再次触发回退」—— 暂停/恢复、重启程序后，门控必须从零开始重新判定，
    //  不能因为上一会话曾拿到过落笔数据（everPenTipDown_）或曾识别出注入笔
    //  （everInjectedPen_）就永久关闭回退。
    //
    //  目前该不变量由对象生命周期**隐式**保证：全部标志都是 Impl 成员，
    //  Stop() 里 delete impl（见 ThreadProc 退出清理），Start() 里 new Impl()，
    //  托盘「暂停记录/恢复记录」走的正是 Stop/Start（MainWindow.cpp 托盘菜单）。
    //
    //  为什么还要显式复位：隐式保证很脆 —— 将来若有人为了「跨会话保留诊断计数」
    //  把某个标志挪到 Capture 单例上，回退就会静默变成一次性、且没有任何测试会
    //  失败。这里显式列出必须会话级复位的字段并在会话开始时调用，使该不变量
    //  成为代码里可读、可审查的约定，而不只是内存布局的副作用。
    // ========================================================
    void ResetSessionState() {
        // 门控否决条件（决定回退是否启用）
        everPenTipDown_   = false;
        everInjectedPen_  = false;
        // 「只记一次」的日志闸（决定新会话是否会再记一条证据）
        mouseFallbackLogged_    = false;
        injPenLogged            = false;
        penPressureLogged       = false;
        penPressureFailedLogged = false;
        penWatchdogLogged_      = false;
        touchDropLogged         = false;
        unusualPenSigLogged_    = false;
        unrecInjectedLogged_    = false;
        // 原始笔通道存活信号
        lastRealPenSampleUs_ = 0;
        lastPenSampleUs = 0;
    }

    // ========================================================
    bool TargetActive() const { return fgValid && fg.isTarget; }

    // ---------------- 实时活跃状态（供录屏判断「是否正在操作」） ----------------
    // 关键：必须在「采样级」更新，而不是「笔画级」。
    // 一次连续运笔可持续两三秒甚至更久，期间不产生任何已完成的操作；
    // 若活跃时刻只在笔画收尾时刷新，录屏就会在长笔画中途判为空闲并切断。
    void TouchAct(int64_t nowUs) {
        if (nowUs > self->actUs_.load(std::memory_order_relaxed))
            self->actUs_.store(nowUs, std::memory_order_relaxed);
    }

    // 是否有笔画正在进行（笔尖未抬 / 左键未松）
    bool AnyStrokeActive() const {
        return mStroke.active || AnyPenStroke() || injPen.inStroke || pen.TipDown();
    }

    void SyncStrokeActive() {
        self->strokeAct_.store(AnyStrokeActive() ? 1 : 0, std::memory_order_relaxed);
    }

    // ---------------- 落盘 ----------------
    void Notify(const OpAtom& a) {
        if (self->opCb_) self->opCb_(a);
    }

    void EmitKey(int vk, uint8_t mods, int64_t tUs, int64_t endUs, int count,
                 OpKind kind, bool penActive) {
        RawRecord r;
        r.tUs = tUs;
        r.endUs = endUs ? endUs : tUs;
        r.type = 'k';
        r.vk = vk; r.mods = mods; r.count = count; r.kind = kind;
        r.title = ModPrefix(mods) + VkName(vk);
        r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = tUs; a.endUs = r.endUs;
        a.kind = kind; a.src = 0;
        a.intensity = (float)count;
        a.note = r.title;
        Notify(a);

        counters.keys++;
        if (count > 1) counters.keysMergedIn += (uint64_t)(count - 1);
        if (kind == OpKind::Undo) counters.undoOps++;
        if (kind == OpKind::Redo) counters.redoOps++;
        if (penActive) counters.penKbAttr++;
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, r.endUs);
    }

    static float StrokePathLen(const std::vector<PathPoint>& pts) {
        float len = 0.f;
        for (size_t i = 1; i < pts.size(); ++i) {
            double dx = pts[i].x - pts[i - 1].x, dy = pts[i].y - pts[i - 1].y;
            len += (float)sqrt(dx * dx + dy * dy);
        }
        return len;
    }

    void EmitMouseStroke(int64_t downUs, int64_t upUs, int button, bool kbAttr,
                         std::vector<PathPoint>& pts) {
        if (pts.size() < 2) { pts.clear(); return; }
        RawRecord r;
        r.tUs = downUs; r.endUs = upUs;
        r.type = 'M';
        // 落笔瞬间有键按住 → 整次记为键盘操作（普通操作）
        r.kind = OpKind::Normal;
        r.button = button;
        r.kbAttr = kbAttr;
        r.pathLen = StrokePathLen(pts);
        float mx = 0.f;
        for (const auto& p : pts) mx = std::max(mx, p.speed);
        r.maxSpeed = mx;
        r.mpts = pts;
        r.title = fg.title;
        r.exe = fg.exe;
        r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = downUs; a.endUs = upUs;
        a.kind = OpKind::Normal; a.src = kbAttr ? 0 : 1;
        a.intensity = r.pathLen;
        a.note = kbAttr ? "mouse-stroke-as-keyboard" : "mouse-stroke";
        Notify(a);

        counters.mouseStrokes++;
        counters.mousePoints += pts.size();
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, upUs);
        pts.clear();
    }

    void EmitMouseMove(int64_t firstUs, int64_t lastUs, std::vector<PathPoint>& pts, bool kbAttr) {
        if (pts.size() < 2) { pts.clear(); return; }
        RawRecord r;
        r.tUs = firstUs; r.endUs = lastUs;
        r.type = 'm';
        r.kind = OpKind::Normal;
        r.button = 0;
        r.kbAttr = kbAttr;
        r.pathLen = StrokePathLen(pts);
        float mx = 0.f, sum = 0.f;
        for (const auto& p : pts) { mx = std::max(mx, p.speed); sum += p.speed; }
        r.maxSpeed = mx;
        r.avgSpeed = sum / (float)pts.size();
        r.mpts = pts;
        r.title = fg.title;
        r.exe = fg.exe;
        r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = firstUs; a.endUs = lastUs;
        a.kind = OpKind::Normal; a.src = kbAttr ? 0 : 1;
        a.intensity = r.pathLen;
        a.note = kbAttr ? "mouse-move-as-keyboard" : "mouse-move";
        Notify(a);

        counters.mouseMoves++;
        counters.mousePoints += pts.size();
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, lastUs);
        pts.clear();
    }

    void EmitMouseClick(int64_t tUs, int button) {
        // 前台必须是受监控的绘画软件才记录。
        // 此前唯独这条路径漏了该判断（键盘、鼠标移动、鼠标笔画都有），
        // 导致在任意软件里滚动滚轮都会刷新 lastOpUs —— 于是「非前台时
        // 也会计入图表并让录屏保持活跃」；同时它一个计数器都不加，
        // 所以「前台时操作数也不涨」。两个症状同源，一并修正。
        if (!TargetActive()) return;

        // 滚轮不走这里：见 OnWheel（需要合并窗口，与键盘 pending 同构）
        if (button == 3) { OnWheel(tUs); return; }

        RawRecord r;
        r.tUs = tUs; r.endUs = tUs;
        r.type = 'm'; r.kind = OpKind::Normal; r.button = button;
        r.title = fg.title; r.exe = fg.exe; r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = tUs; a.endUs = tUs;
        a.kind = OpKind::Normal; a.src = 1;
        a.intensity = 0.f;
        a.note = "mouse-click";
        Notify(a);

        counters.mouseClicks++;
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, tUs);
    }

    // ------------------------------------------------------------
    //  滚轮：合并窗口内聚成一个事件（时间戳取第一次，累计次数）
    //
    //  与键盘 pending 同一套纪律，只是滚轮不需要 vk/mods/kind 升级。
    //  注意 lastOpUs 必须**每条消息都立即刷新**：合并只影响「记几条、
    //  算几次」，不能影响「是否正在操作」的判定 —— 否则合并窗口内
    //  录屏会误判空闲而停止写帧。
    // ------------------------------------------------------------
    void OnWheel(int64_t tUs) {
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, tUs);

        const int64_t mergeUs = (int64_t)Cfg().keyMergeMs * 1000;
        if (wheelPend.active && mergeUs > 0 && (tUs - wheelPend.lastUs) <= mergeUs) {
            wheelPend.count++;
            wheelPend.lastUs = tUs;
            return;
        }
        FlushWheel();               // 上一组已到期 → 先落盘，再另起一组
        wheelPend.active  = true;
        wheelPend.firstUs = tUs;    // 记录时间戳 = 组内第一次
        wheelPend.lastUs  = tUs;
        wheelPend.count   = 1;
    }

    void FlushWheel() {
        if (!wheelPend.active) return;
        EmitWheel(wheelPend.firstUs, wheelPend.lastUs, wheelPend.count);
        wheelPend.Reset();
    }

    void EmitWheel(int64_t firstUs, int64_t lastUs, int count) {
        RawRecord r;
        r.tUs = firstUs; r.endUs = lastUs;
        r.type = 'm'; r.kind = OpKind::Normal; r.button = 3;
        r.count = count;                 // 合并进来的滚轮消息数（原始数据如实保留）
        r.title = fg.title; r.exe = fg.exe; r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = firstUs; a.endUs = lastUs;
        a.kind = OpKind::Normal;
        a.src = 0;                       // 滚轮视为键盘操作
        a.intensity = (float)count;
        a.note = "mouse-wheel";
        Notify(a);

        counters.mouseClicks++;          // 合并后只算一次
    }

    // fromFallback=true：这条「笔笔画」实际来自最后一层鼠标回退（见
    // MouseFallbackActive）。仍然按笔笔画落盘（type='p'、计入 penStrokes），
    // 因为用户要的就是「拿不到笔时把鼠标笔画算作笔」；但额外单列计数并在
    // note 上标注来源，使事后可审计、可评估回退是否误判。
    void EmitPenStroke(int64_t downUs, int64_t upUs, bool hover, bool kbAttr,
                       uint8_t device, uint32_t devSeq, bool eraser,
                       float maxP, float avgP, float maxS, float avgS, float len,
                       std::vector<PenPoint>& pts, bool fromFallback = false) {
        if (pts.size() < 2) { pts.clear(); return; }
        RawRecord r;
        r.tUs = downUs; r.endUs = upUs;
        r.type = 'p';
        r.device = device;
        r.deviceId = devSeq;
        r.eraser = eraser;
        r.hover = hover;
        r.kbAttr = kbAttr;
        r.kind = (!hover && kbAttr) ? OpKind::Normal : OpKind::Brush;
        r.maxPressure = maxP; r.avgPressure = avgP;
        r.maxSpeed = maxS;    r.avgSpeed = avgS;
        r.pathLen = len;
        r.ppts = pts;
        r.title = fg.title; r.exe = fg.exe; r.isTarget = true;
        AppendEvent(r);

        OpAtom a;
        a.tUs = downUs; a.endUs = upUs;
        a.kind = r.kind;
        a.src = (!hover && kbAttr) ? 0 : 2;
        a.intensity = len;
        a.note = hover ? "pen-hover"
                 : (fromFallback ? "pen-stroke-from-mouse-fallback"
                 : (kbAttr ? "pen-stroke-as-keyboard" : "pen-stroke"));
        Notify(a);

        if (!hover) {
            counters.penStrokes++;
            counters.penPoints += pts.size();
            if (fromFallback) counters.mouseAsPenStrokes++;
            if (kbAttr) counters.penKbAttr++;
            counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, upUs);
        }
        pts.clear();
    }

    // ---------------- 键盘 ----------------
    void FlushPendingKey() {
        if (!pending.active) return;
        EmitKey(pending.vk, pending.mods, pending.firstUs, pending.lastUs,
                pending.count, pending.kind, pending.penActive);
        pending.Reset();
    }

    void FlushModPendings() {
        for (int i = 0; i < 4; ++i) {
            ModPending& m = modPend[i];
            if (!m.active) continue;
            if (!m.consumed) {
                int vk = (i == 0) ? VK_CONTROL : (i == 1) ? VK_SHIFT : (i == 2) ? VK_MENU : VK_LWIN;
                EmitKey(vk, PB_MOD_NONE, m.firstUs, m.lastUs, m.count, OpKind::Normal, false);
            }
            m.Reset();
        }
    }

    // 落笔瞬间是否有任意键按下。
    // 必须用 GetAsyncKeyState 实时查询物理状态，不能用钩子维护的缓存集合：
    // 低级钩子的 WM_KEYUP 在切换前台、UAC/安全桌面、Ctrl+Alt+Del、部分全屏
    // 应用等场景会丢失，缓存一旦残留某个 VK 就永远非空，导致「每次落笔都判为
    // 有键按下」→ 所有笔笔画被归类为键盘操作，画笔统计恒为 0。
    bool AnyKeyDown() {
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            // 鼠标三键不算「按键」（笔落纸时系统可能合成左键）
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
            if (vk == VK_XBUTTON1 || vk == VK_XBUTTON2) continue;
            if (GetAsyncKeyState(vk) & 0x8000) return true;
        }
        return false;
    }

    void OnKeyDown(int vk, int64_t nowUs) {
        // 前台必须是受监控的绘画软件才记录（与鼠标/笔路径一致）。
        // 此前唯独键盘漏了这个判断，导致在任何软件里按键都被记为操作。
        if (!TargetActive()) return;

        keysDown.insert(vk);
        TouchAct(nowUs);

        const int64_t mergeUs = (int64_t)Cfg().keyMergeMs * 1000;

        if (IsModifierVk(vk)) {
            int idx = ModIndex(vk);
            if (idx < 0) return;
            ModPending& m = modPend[idx];
            if (m.active && !m.consumed && (nowUs - m.lastUs) <= mergeUs) {
                m.count++;
                m.lastUs = nowUs;
            } else {
                FlushModPendings();
                FlushPendingKey();
                m.active = true; m.consumed = false;
                m.firstUs = nowUs; m.lastUs = nowUs; m.count = 1;
            }
            counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, nowUs);
            return;
        }

        // 非修饰键按下：此前的修饰键属于组合键，撤销其独立事件
        for (int i = 0; i < 4; ++i) if (modPend[i].active) modPend[i].consumed = true;

        uint8_t mods = CurrentMods();
        OpKind kind = OpKind::Normal;
        // 撤回：Ctrl+Z（不含 Shift）；重做：Ctrl+Y 或 Ctrl+Shift+Z
        if ((mods & PB_MOD_CTRL) && !(mods & PB_MOD_ALT) && !(mods & PB_MOD_WIN)) {
            if (vk == 'Z' && !(mods & PB_MOD_SHIFT))      kind = OpKind::Undo;
            else if (vk == 'Y' || (vk == 'Z' && (mods & PB_MOD_SHIFT))) kind = OpKind::Redo;
        }

        bool penActive = AnyPenStroke() || pen.TipDown();

        if (pending.active && pending.vk == vk && pending.mods == mods &&
            mergeUs > 0 && (nowUs - pending.lastUs) <= mergeUs) {
            pending.count++;
            pending.lastUs = nowUs;
            // 组合键在连按时可能升级分类（如先 Z 后 Ctrl+Z），取更特殊的
            if (kind != OpKind::Normal) pending.kind = kind;
            return;
        }

        FlushPendingKey();
        pending.active = true;
        pending.vk = vk;
        pending.mods = mods;
        pending.firstUs = nowUs;
        pending.lastUs = nowUs;
        pending.count = 1;
        pending.kind = kind;
        pending.penActive = penActive;
    }

    void OnKeyUp(int vk, int64_t nowUs) {
        keysDown.erase(vk);
        if (!IsModifierVk(vk)) return;
        int idx = ModIndex(vk);
        if (idx < 0) return;
        ModPending& m = modPend[idx];
        if (!m.active) return;
        if (!m.consumed) {
            int base = (idx == 0) ? VK_CONTROL : (idx == 1) ? VK_SHIFT : (idx == 2) ? VK_MENU : VK_LWIN;
            EmitKey(base, PB_MOD_NONE, m.firstUs, m.lastUs, m.count, OpKind::Normal, false);
        }
        m.Reset();
        (void)nowUs;
    }

    // ---------------- 前台 ----------------
    void UpdateForeground(bool force) {
        HWND h = GetForegroundWindow();
        if (!h) return;

        // 轮询路径（force=false）在 hwnd 未变时直接返回，**刻意不重读标题**。
        //
        // 为什么不在这里比对标题：GetWindowTextW 对跨进程窗口是发消息给目标，
        // 目标忙时会阻塞调用线程。而本函数由采集线程的 WM_TIMER(TID_FGPOLL)
        // 与 WM_INPUT(pen) 共用同一消息循环触发 —— 一旦在这里阻塞，数位笔的
        // 原始报文就会排队延迟，直接丢笔数据。PS 标题又含缩放百分比/图层号，
        // 运笔时高频变化，按标题变化触发收尾还会打断连续笔画、让日志充满伪切换。
        //
        // 因此采集层的 fg.title 只反映「hwnd 切换那一刻」的标题快照，够用即可；
        // 「同一 hwnd 内标题变化（如 PS 切文档标签）也要拿到新文档名」这个需求
        // 由录屏线程独立满足：它每轮对锁定的主窗口自己 ReadWndTitle，阻塞只发生
        // 在录屏线程、最坏延迟一次录制决策，绝不影响采集。（见 Recorder::Loop）
        if (!force && fgValid && fg.hwnd == h) return;

        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::string exe = ToLowerA(GetProcessExeName(pid));
        wchar_t titleBuf[1024] = {0};
        GetWindowTextW(h, titleBuf, 1024);
        std::string title = W2U(titleBuf);

        // 切换前先把缓冲事件全部收尾，避免归属错乱
        FlushPendingKey();
        FlushModPendings();
        FlushWheel();          // 滚轮待合并组必须在此收尾，否则会记到新前台的窗口标题下
        FlushMouse(true);
        FlushPen(true);

        fg.hwnd = h;
        fg.exe = exe;
        fg.title = title;
        fg.isTarget = Cfg().IsTarget(exe);
        fg.sinceUs = Clock::NowUs();
        fgValid = true;

        RawRecord r;
        r.tUs = fg.sinceUs;
        r.type = 'f';
        r.exe = exe;
        r.title = title;
        r.isTarget = fg.isTarget;
        AppendEvent(r);

        {
            CsLock lk(self->snapCs_);
            self->fg_ = fg;
            self->counters_.fgExe = exe;
            self->counters_.fgTitle = title;
            self->counters_.targetActive = fg.isTarget;
        }

        LogF("[fg] exe=%s target=%d title=%s", exe.c_str(), (int)fg.isTarget, title.c_str());
        if (self->fgCb_) self->fgCb_(fg);
    }

    // ---------------- 鼠标 ----------------
    // 这个注入鼠标事件是否来自数位笔。
    //
    // 判定逻辑在 PenInject.cpp（纯函数，可离线测试）。此处只负责组装证据。
    // 为什么不能只认 FROMTOUCH 签名：真实用户报告（20261018_144042/_144046）
    // 显示 penRawReports=0 且 mouseStrokes=73/123、penStrokes=0，undo=7、
    // redo=30 证明确实在作画 —— 笔被 Ink/厂商驱动接管后以注入鼠标出现，
    // 但不带 FROMTOUCH 签名，于是全被记成鼠标笔画。新增 IMDT_PEN 判据
    // （GetCurrentInputMessageSource，与厂商签名无关）修复此路径。
    bool IsInjectedByPen(const InjectEvidence& ev, int mx, int my,
                         int64_t nowUs, int64_t lastPenUs, int32_t lastPenX, int32_t lastPenY) {
        PenPositionMatch pm;
        pm.recentPenSample = (lastPenUs != 0 && (nowUs - lastPenUs) < 80000);
        pm.mx = mx; pm.my = my;
        pm.lastPenX = lastPenX; pm.lastPenY = lastPenY;

        InjectReason why = InjectReason::kNotPen;
        bool isPen = IsPenInjectedEvent(ev, RealPenSilent(), &pm, &why);
        if (isPen) everInjectedPen_ = true;   // 注入路径已能识别笔 → 关闭最后一层回退
        if (isPen && why != InjectReason::kFromTouchSignature && !unusualPenSigLogged_) {
            unusualPenSigLogged_ = true;
            // 这条日志是关键证据：说明该环境的笔不带标准签名，
            // 靠 IMDT_PEN 或位置吻合才被识别。下一批用户报告据此判断
            // 是否还有未被覆盖的注入形态。
            LogF("[pen] 注入笔识别命中非标准路径: reason=%s extraInfo=0x%08lX flags=0x%08lX "
                 "srcOk=%d deviceType=0x%X originId=0x%X",
                 InjectReasonName(why), (unsigned long)ev.extraInfo, (unsigned long)ev.flags,
                 (int)ev.srcOk, (unsigned)ev.deviceType, (unsigned)ev.originId);
        }
        return isPen;
    }

    // 原始 HID 笔通道是否静默（从未收到过真实笔采样）。
    // 静默 = Windows Ink 接管了笔，需启用注入鼠标回退采集。
    //
    // 保持原定义（「有无任何采样」）不改为「有无落笔采样」：本机的重复计数守卫
    // （OnPenSample 里丢弃进行中的注入笔画）依赖它在**第一条**真实采样到达时
    // 翻转，而悬停报文通常先于落笔到达 —— 改成落笔口径会把这道守卫推迟到落笔
    // 之后，反而给已验证正常的路径开出重复计数的窗口。
    //
    // Wintab 独占导致的「只有悬停、没有落笔」由另一条正交判据 everPenTipDown_
    // 处理（见 MouseFallbackActive），不通过放宽这里来实现。
    bool RealPenSilent() const { return lastRealPenSampleUs_ == 0; }

    // —— 最后一层回退：数位笔在、但落笔数据一条都拿不到 ——
    //
    // 触发场景（用户复现）：CSP 开启 Wintab 后，驱动把笔尖接触数据交给 Wintab
    // API 由应用独占消费，Raw Input 侧只剩悬停报文；驱动另行合成普通鼠标事件驱动
    // 光标，这些事件既无 FROMTOUCH 签名、系统输入源也报 IMDT_MOUSE，内核态注入
    // 还不带 LLMHF_INJECTED，于是注入路径的三个判据全部落空，笔画被记成鼠标。
    //
    // 两份报告分别命中两侧：
    //   · 20261018_155346：penSamples=759 全为 tip=0、penStrokes=0、
    //     mouseStrokes=182、undo=11（RealPenSilent()=false，走的是普通鼠标路径）
    //   · 20261018_155341：penRawReports=0、注册回读 flags=0x120 且 hwnd 与采集
    //     窗口一致 valid=1 → 注册无误、系统就是不投递；且无 WARN 行（内核注入
    //     不带 INJECTED 标志，旧告警条件 flags&LLMHF_INJECTED 命不中）
    //
    // 有损：这类笔画没有压感与橡皮信息，只保住笔画计数与轨迹。故单独计数
    // （mouseAsPenStrokes）、单独标注 note，并可用配置项关闭。
    bool MouseFallbackActive() {
        MouseFallbackCtx ctx;
        ctx.hasPenDevice    = pen.HasPressurePenDevice();
        ctx.everPenTipDown  = everPenTipDown_;
        ctx.everInjectedPen = everInjectedPen_;
        return Cfg().mouseAsPenFallback && ShouldUseMouseFallback(ctx);
    }

    // 该注入鼠标事件是否来自「手指触摸」而非笔。
    //
    // 为什么需要：MOUSEEVENTF_FROMTOUCH 这个签名**同时覆盖手指触摸与笔**
    // （名字里的 TOUCH 就是这个意思）。带触控的数位屏上，用户用手指缩放
    // 画布时也会产生带此签名的注入鼠标事件，而手指不是画笔，不该计入笔画。
    //
    // 保守设计：只在确证为触摸时返回 true。判定在 PenInject.cpp（纯函数）：
    // 优先用系统输入源 deviceType（含 TOUCH 且不含 PEN），其次用 pointerId
    // 的 GetPointerInfo 结果。两者都不明确时返回 false（当作笔，不误伤）。
    bool IsTouchPointer(const InjectEvidence& ev) {
        if ((ev.extraInfo & PB_INJECT_SIG_MASK) != (MOUSEEVENTF_FROMTOUCH & PB_INJECT_SIG_MASK))
            return false;   // 非签名注入：无 pointerId 可查，保持原行为（当作笔）
        UINT32 pid = ev.extraInfo & 0x000000FF;       // 低字节 = pointerId
        if (pid == 0) return false;
        POINTER_INFO pi{};
        bool known = GetPointerInfo(pid, &pi) ? true : false;
        return IsTouchEvent(ev, pid, known ? (DWORD)pi.pointerType : 0, known);
    }

    // 从 Ink 促进的鼠标事件 dwExtraInfo 还原 pointerId，查系统全局笔状态取压感。
    // dwExtraInfo 布局：高位 = MI_WP_SIGNATURE(0xFF515700)，低字节 = pointerId。
    // GetPointerPenInfo 按 pointerId 查询系统维护的笔状态，后台进程亦可读，
    // 不需要 RegisterPointerInputTarget（那会独占指针、抢走前台应用的笔输入）。
    // 成功返回 true 并给出归一化压感(0..1)与橡皮标志；失败返回 false（调用方退回 1.0）。
    static bool QueryPenPressure(DWORD extraInfo, float& pressure01, bool& eraser, UINT32& outId) {
        const DWORD SIG_MASK = 0xFFFFFF00;         // = PB_INJECT_SIG_MASK
        const DWORD MI_WP_SIGNATURE = 0xFF515700;  // = MOUSEEVENTF_FROMTOUCH
        if ((extraInfo & SIG_MASK) != MI_WP_SIGNATURE) return false;
        UINT32 pid = extraInfo & 0x000000FF;       // 低字节 = pointerId
        if (pid == 0) return false;
        outId = pid;
        POINTER_PEN_INFO ppi{};
        if (!GetPointerPenInfo(pid, &ppi)) return false;
        if (ppi.pointerInfo.pointerType != PT_PEN) return false;
        eraser = (ppi.penFlags & PEN_FLAG_ERASER) != 0;
        if (ppi.penMask & PEN_MASK_PRESSURE) {
            // POINTER_PEN_INFO.pressure 是系统**已归一化**的值，范围恒为 0..1024，
            // 与设备原生压感级数无关（官方文档：a pen pressure normalized to a
            // range between 0 and 1024）。因此这里固定除以 1024，不需要按品牌适配。
            //
            // 注意区分另一条路径的量纲：原始 HID 路径读的是**设备原生量程**
            // （本机 Wacom 为 0..8191，旧款有 0..1024/512/256），那边必须用
            // 描述符里的 pressMin/pressMax 自适应归一，两条路径不可混用。
            float p = (float)ppi.pressure / 1024.0f;
            if (p < 0.f) p = 0.f;
            if (p > 1.f) p = 1.f;
            pressure01 = p;
            return true;
        }
        return false;
    }

    // —— 注入鼠标 → 画笔笔画（回退路径）——
    // fallback=true 表示走「最后一层回退」：事件不是被判定为笔的注入事件，而是
    // 普通鼠标左键（Wintab 独占时驱动自行合成的光标事件）。此时 dwExtraInfo 不含
    // 签名、查不到 pointerId，压感必然退回 1.0，橡皮信息不可得。
    void InjPenDown(int mx, int my, DWORD extraInfo, int64_t nowUs, bool fallback = false) {
        injPen.Reset();
        injPen.inStroke = true;
        injPen.fromMouseFallback = fallback;
        injPen.downUs = nowUs;
        injPen.lastUs = nowUs;
        injPen.lastTUs = nowUs;
        injPen.lastX = mx; injPen.lastY = my;
        injPen.haveLast = false;
        injPen.kbAttr = AnyKeyDown();   // 落笔瞬间有键按下 → 整次记为键盘操作

        // 取压感
        float pr = 1.f;
        bool eraser = false;
        UINT32 pid = 0;
        if (QueryPenPressure(extraInfo, pr, eraser, pid)) {
            injPen.pointerId = pid;
            injPen.nPressure = 1;
            if (!penPressureLogged) {
                penPressureLogged = true;
                LogF("[pen] POINTER pressure ACTIVE: pointerId=%u pressure=%.3f (Ink 通道压感已获取)",
                     (unsigned)pid, pr);
            }
        } else {
            if (!penPressureFailedLogged) {
                penPressureFailedLogged = true;
                LogF("[pen] POINTER pressure unavailable (extraInfo=0x%08lX) -> 压力退回 1.0",
                     (unsigned long)extraInfo);
            }
        }

        PenPoint p;
        p.dtUs = 0; p.x = mx; p.y = my; p.speed = 0.f; p.pressure = pr;
        p.flags = PENF_TIP | PENF_INRANGE | (eraser ? PENF_ERASER : 0);
        if (eraser) injPen.sawEraser = true;
        injPen.pts.push_back(p);
        injPen.nPts = 1;
        injPen.maxP = pr; injPen.sumP = pr;
        if (!injPenLogged) {
            injPenLogged = true;
            LogF("[pen] RAW HID silent -> injected-mouse fallback ACTIVE (first pen-down at %d,%d)", mx, my);
        }
    }
    void InjPenMove(int mx, int my, DWORD extraInfo, int64_t nowUs) {
        if (!injPen.inStroke) return;
        PenPoint p;
        int64_t d = nowUs - injPen.lastTUs;
        double dx = (double)(mx - injPen.lastX), dy = (double)(my - injPen.lastY);
        double dist = sqrt(dx * dx + dy * dy);
        p.dtUs = (int32_t)std::min<int64_t>(std::max<int64_t>(d, 0), INT32_MAX);
        p.speed = (d > 0) ? (float)(dist * 1000000.0 / (double)d) : 0.f;
        p.x = mx; p.y = my;

        // 压感：优先用当前 pointerId 查；查不到退回 1.0
        float pr = 1.f;
        bool eraser = false;
        UINT32 pid = injPen.pointerId;
        if (QueryPenPressure(extraInfo, pr, eraser, pid)) {
            injPen.pointerId = pid;
            injPen.nPressure++;
        } else if (injPen.pointerId) {
            // 用已缓存的 pointerId 再查一次（移动事件 extraInfo 可能不再带签名）
            POINTER_PEN_INFO ppi{};
            if (GetPointerPenInfo(injPen.pointerId, &ppi) && (ppi.penMask & PEN_MASK_PRESSURE)) {
                float q = (float)ppi.pressure / 1024.0f;
                if (q < 0.f) q = 0.f; if (q > 1.f) q = 1.f;
                pr = q;
                eraser = (ppi.penFlags & PEN_FLAG_ERASER) != 0;
                injPen.nPressure++;
            }
        }
        p.pressure = pr;
        p.flags = PENF_TIP | PENF_INRANGE | (eraser ? PENF_ERASER : 0);
        if (eraser) injPen.sawEraser = true;
        injPen.pts.push_back(p);
        injPen.lastX = mx; injPen.lastY = my;
        injPen.lastTUs = nowUs;
        injPen.lastUs = nowUs;
        injPen.nPts++;
        injPen.len += (float)dist;
        injPen.maxS = std::max(injPen.maxS, p.speed);
        injPen.sumS += p.speed;
        injPen.maxP = std::max(injPen.maxP, pr);
        injPen.sumP += pr;
        if (injPen.pts.size() > 24576) InjPenUp(mx, my, extraInfo, nowUs, true);
    }
    void InjPenUp(int mx, int my, DWORD extraInfo, int64_t nowUs, bool reopen = false) {
        if (!injPen.inStroke) return;
        int64_t upUs = nowUs;
        int n = std::max(1, injPen.nPts);
        // 压感统计：有真实压感则用之，否则 0（表示该笔画无压感数据）
        float maxP = injPen.nPressure > 0 ? injPen.maxP : 0.f;
        float avgP = injPen.nPressure > 0 ? (injPen.sumP / (float)n) : 0.f;
        // 设备类型：不再硬编码 PEN_EXTERNAL。
        // 注入路径处理的是系统伪鼠标事件，没有 HID 设备句柄，改用 PenInput
        // 已识别出的笔设备类型；识别不到（或数位屏与数位板并存无法判定）时
        // 退回外接数位板这一最常见默认值。此前数位屏在 Ink 开启时会被错记成
        // 「外接数位板」，界面与日志的设备信息因此误导排查。
        uint8_t devType = pen.DominantPenType();
        if (devType == PEN_UNKNOWN) devType = PEN_EXTERNAL;
        // 橡皮：QueryPenPressure 每个采样都取到了 eraser，此前却在收尾时被丢弃，
        // 导致任何开启 Windows Ink 的品牌（多数品牌默认开）用笔尾橡皮擦时
        // 全部被记成「画笔笔画」，Analyzer 的「橡皮」标签因此恒不出现。
        // devSeq=0 表示「注入路径」——Ink 伪鼠标没有对应的真实 HID 设备序号。
        EmitPenStroke(injPen.downUs, upUs, false, injPen.kbAttr, devType, 0, injPen.sawEraser,
                      maxP, avgP, injPen.maxS, injPen.sumS / (float)n, injPen.len, injPen.pts,
                      injPen.fromMouseFallback);
        bool reopenStroke = reopen;
        bool wasFallback = injPen.fromMouseFallback;
        injPen.Reset();
        if (reopenStroke) InjPenDown(mx, my, extraInfo, nowUs, wasFallback);   // 超长笔画分段后重开
    }

    static void AppendPathPoint(std::vector<PathPoint>& pts, int mx, int my,
                                int64_t nowUs, int64_t prevUs) {
        PathPoint p;
        int px = mx, py = my;
        int64_t dt = 0;
        if (!pts.empty()) {
            const PathPoint& b = pts.back();
            px = b.x; py = b.y;
            dt = nowUs - prevUs;
        }
        double dx = (double)(mx - px), dy = (double)(my - py);
        double dist = sqrt(dx * dx + dy * dy);
        p.dtUs = (int32_t)std::min<int64_t>(std::max<int64_t>(dt, 0), INT32_MAX);
        p.x = mx; p.y = my;
        p.speed = (dt > 0) ? (float)(dist * 1000000.0 / (double)dt) : 0.f;
        pts.push_back(p);
    }

    void OnMouseMove(int mx, int my, const InjectEvidence& ev, int64_t nowUs) {
        if (!TargetActive()) return;
        // 注意：**不在这里无条件刷新活跃时刻**。
        // 纯移动（没按住任何键）不是作画，不该延长录屏时间 ——
        // 只有「按住左键的运笔」才算操作，见下面 mStroke.active 分支。
        bool injected = IsInjectedByPen(ev, mx, my, nowUs, lastPenSampleUs, lastPenX, lastPenY);

        // 最后一层回退进行中：鼠标移动就是运笔轨迹。
        // 必须显式接管 —— 这类事件**不会**被 IsInjectedByPen 判为笔（这正是启用
        // 回退的前提），若沿用下面的分支它们会掉进 freeMove，导致整条笔画只剩
        // 落笔那一个点、被 EmitPenStroke 的 pts.size()<2 直接丢弃。
        if (injPen.inStroke && injPen.fromMouseFallback) {
            TouchAct(nowUs);        // 笔尖接触中 → 真实运笔，算活跃
            InjPenMove(mx, my, ev.extraInfo, nowUs);
            return;
        }

        // Windows Ink 回退：原始 HID 笔静默时，注入鼠标移动 = 运笔
        if (injected && RealPenSilent()) {
            if (injPen.inStroke) {
                // 防御：笔笔画进行中若混入手指移动（多指/交替），
                // 不应把手指点算进笔轨迹。手指触摸直接丢弃。
                if (IsTouchPointer(ev)) return;
                TouchAct(nowUs);        // 笔尖接触中 → 真实运笔，算活跃
                InjPenMove(mx, my, ev.extraInfo, nowUs);
            }
            return;                     // 注入的悬空移动不算活跃
        }

        if (mStroke.active) {
            TouchAct(nowUs);            // 左键按住拖动 → 作画中，算活跃
            if (injected || mStroke.fromPen) { mStroke.fromPen = true; return; }
            AppendPathPoint(mStroke.pts, mx, my, nowUs, mStroke.lastUs);
            mStroke.lastUs = nowUs;
            mStroke.ex = mx; mStroke.ey = my;
            if (mStroke.pts.size() > 16384) FlushMouse(true);
            return;
        }

        if (injected) return;   // 原始笔在工作时，注入的伪鼠标移动不计入鼠标轨迹

        if (!freeMove.active) {
            freeMove.active = true;
            freeMove.firstUs = nowUs;
            freeMove.pts.clear();
            // 段起始时有键按住 → 整段归为键盘操作（与笔画同一规则）
            freeMove.kbAttr = AnyKeyDown();
            PathPoint p; p.dtUs = 0; p.x = mx; p.y = my; p.speed = 0.f;
            freeMove.pts.push_back(p);
        } else {
            AppendPathPoint(freeMove.pts, mx, my, nowUs, freeMove.lastUs);
        }
        freeMove.lastUs = nowUs;
        if (freeMove.pts.size() > 4096) FlushMouse(false);
    }

    void OnMouseButtonDown(int button, int mx, int my, const InjectEvidence& ev, int64_t nowUs) {
        if (!TargetActive()) return;

        if (button != 0) { EmitMouseClick(nowUs, button); return; }

        bool injected = IsInjectedByPen(ev, mx, my, nowUs, lastPenSampleUs, lastPenX, lastPenY);

        // Windows Ink 回退：原始 HID 笔静默时，注入的左键按下 = 落笔
        if (injected && RealPenSilent()) {
            // 手指触摸直接丢弃，不做任何处理（与原始 HID 路径的触摸丢弃一致）。
            // 若不拦在这里，带触控数位屏上的手指操作会被记成画笔笔画。
            if (IsTouchPointer(ev)) {
                if (!touchDropLogged) {
                    touchDropLogged = true;
                    LogF("[pen] drop: Ink 回退路径识别到手指触摸(PT_TOUCH)，不计入笔画");
                }
                FlushMouse(false);
                return;
            }
            FlushMouse(false);
            InjPenDown(mx, my, ev.extraInfo, nowUs);
            return;
        }

        FlushMouse(false);
        if (injected) { mStroke.Reset(); mStroke.fromPen = true; return; }

        // —— 最后一层回退（用户授权）——
        // 数位笔设备在、却拿不到任何落笔数据，注入路径也认不出这是笔时，
        // 把这次鼠标左键当作落笔。典型场景：CSP 开启 Wintab 后驱动独占笔尖
        // 通道，另行合成普通鼠标事件驱动光标（无签名、系统输入源报 IMDT_MOUSE、
        // 内核注入不带 LLMHF_INJECTED，三个判据全部落空）。
        // 门控三条否决见 ShouldUseMouseFallback；代价是无压感、无橡皮，
        // 故单独计数 mouseAsPenStrokes 并在 note 标注来源，可用配置关闭。
        if (MouseFallbackActive()) {
            if (!mouseFallbackLogged_) {
                mouseFallbackLogged_ = true;
                LogF("[pen] 最后一层回退 ACTIVE：已识别数位笔设备但从未收到落笔数据，"
                     "将鼠标左键笔画记为画笔笔画（无压感/无橡皮）。"
                     "penRawReports=%llu 本次 extraInfo=0x%08lX flags=0x%08lX srcOk=%d "
                     "deviceType=0x%X originId=0x%X —— 常见于绘画软件开启 Wintab 独占笔通道",
                     (unsigned long long)counters.penRawReports,
                     (unsigned long)ev.extraInfo, (unsigned long)ev.flags, (int)ev.srcOk,
                     (unsigned)ev.deviceType, (unsigned)ev.originId);
            }
            InjPenDown(mx, my, ev.extraInfo, nowUs, true);
            return;
        }

        // 诊断证据：原始笔通道静默（从未收到 HID 报文）+ 这是一个「系统注入」
        // 的左键按下，但三个判据都没认它是笔 —— 说明该环境的注入形态还没被
        // 覆盖，用户会表现为「画了却只记到鼠标笔画」。限流记一次完整证据，
        // 下一批回传报告据此补判据。（普通鼠标的点击是硬件来源，不会命中此分支。）
        if (RealPenSilent() && (ev.flags & PB_INJ_LLMHF_INJECTED) && !unrecInjectedLogged_) {
            unrecInjectedLogged_ = true;
            LogF("[pen] WARN 未识别的注入左键按下（可能被记成鼠标笔画）: "
                 "extraInfo=0x%08lX flags=0x%08lX srcOk=%d deviceType=0x%X originId=0x%X "
                 "pos=(%d,%d) —— 若此时正在用数位笔作画，请导出诊断信息回传",
                 (unsigned long)ev.extraInfo, (unsigned long)ev.flags, (int)ev.srcOk,
                 (unsigned)ev.deviceType, (unsigned)ev.originId, mx, my);
        }

        mStroke.Reset();
        mStroke.active = true;
        mStroke.downUs = nowUs;
        mStroke.lastUs = nowUs;
        mStroke.button = 0;
        mStroke.sx = mx; mStroke.sy = my;
        mStroke.ex = mx; mStroke.ey = my;
        // 落笔瞬间若有任意键按下 → 整次记为键盘操作
        mStroke.kbAttr = AnyKeyDown();
        PathPoint p; p.dtUs = 0; p.x = mx; p.y = my; p.speed = 0.f;
        mStroke.pts.push_back(p);
        // 笔画开始即视为「正在操作」，刷新活跃时刻
        counters.lastOpUs = std::max<int64_t>(counters.lastOpUs, nowUs);
    }

    void OnMouseButtonUp(int button, int mx, int my, const InjectEvidence& ev, int64_t nowUs) {
        if (button != 0) return;

        // Windows Ink 回退：注入的左键抬起 = 抬笔，收尾一条画笔笔画
        if (injPen.inStroke) {
            InjPenUp(mx, my, ev.extraInfo, nowUs);
            return;
        }

        if (!mStroke.active || mStroke.fromPen) return;
        if (!TargetActive()) { mStroke.Reset(); return; }
        mStroke.ex = mx; mStroke.ey = my;
        mStroke.lastUs = nowUs;
        EmitMouseStroke(mStroke.downUs, nowUs, 0, mStroke.kbAttr, mStroke.pts);
        mStroke.Reset();
    }

    void FlushMouse(bool all) {
        if (all && mStroke.active && !mStroke.fromPen) {
            int64_t nowUs = mStroke.lastUs ? mStroke.lastUs : Clock::NowUs();
            EmitMouseStroke(mStroke.downUs, nowUs, mStroke.button, mStroke.kbAttr, mStroke.pts);
        }
        mStroke.Reset();
        if (freeMove.active) {
            if (all || freeMove.pts.size() > 2048) {
                EmitMouseMove(freeMove.firstUs, freeMove.lastUs, freeMove.pts, freeMove.kbAttr);
            }
            freeMove.Reset();
        }
        SyncStrokeActive();
    }

    // ---------------- 数位笔 ----------------
    void AppendPenPoint(ActivePen& ps, const PenSample& s, bool hover) {
        PenPoint p;
        if (ps.haveLast) {
            int64_t d = s.tUs - ps.lastTUs;
            double dx = (double)(s.x - ps.lastX), dy = (double)(s.y - ps.lastY);
            p.dtUs = (int32_t)std::min<int64_t>(std::max<int64_t>(d, 0), INT32_MAX);
            double dist = sqrt(dx * dx + dy * dy);
            p.speed = (d > 0) ? (float)(dist * 1000000.0 / (double)d) : s.speed;
            if (!hover) ps.len += (float)dist;
        } else {
            p.dtUs = 0;
            p.speed = 0.f;
        }
        p.x = s.x; p.y = s.y;
        if (p.speed <= 0.f) p.speed = s.speed;
        p.pressure = s.pressure;
        p.flags = 0;
        if (s.tipDown) p.flags |= PENF_TIP;
        if (s.barrel)  p.flags |= PENF_BARREL;
        if (s.eraser)  p.flags |= PENF_ERASER;
        if (s.inRange) p.flags |= PENF_INRANGE;
        ps.pts.push_back(p);

        ps.lastX = s.x; ps.lastY = s.y; ps.lastTUs = s.tUs;
        ps.haveLast = true;
        ps.nPts++;

        if (!hover) {
            ps.maxP = std::max(ps.maxP, s.pressure);
            ps.sumP += s.pressure;
            ps.maxS = std::max(ps.maxS, p.speed);
            ps.sumS += p.speed;
        }
    }

    void OnPenSample(const PenSample& s) {
        lastPenSampleUs = s.tUs;
        // 收到真实 HID 笔采样：标记原始笔通道「活着」，据此关闭注入鼠标回退，
        // 避免 Windows Ink 关闭（原始笔正常）时两条路重复计数。
        //
        // 竞态处理：本机实测三条通道是**并发**的（同一次运笔 Raw Input 935 条、
        // WM_POINTER 941 条、注入鼠标 243 条同时到达）。若启动时注入鼠标事件
        // 先于第一条 HID 采样到达，会先开一条 injPen 笔画；随后这条 HID 采样
        // 让 RealPenSilent() 翻转为 false，InjPenMove 不再被调用，该笔画就挂在
        // 原地，直到 400ms 看门狗把它挤出来——于是同一次运笔多记一条重复笔画。
        // 在翻转的瞬间直接丢弃未完成的注入笔画（不落盘），而不是等看门狗收尾。
        if (lastRealPenSampleUs_ == 0 && injPen.inStroke) {
            LogF("[pen] drop: 原始 HID 笔通道已激活，丢弃进行中的注入鼠标笔画（避免重复计数）");
            injPen.Reset();
        }
        lastRealPenSampleUs_ = s.tUs;
        if (s.tipDown) everPenTipDown_ = true;   // 原始通道确实拿到过落笔数据

        // 回退笔画进行中，却来了**真实落笔**数据 → 原始通道其实可用，
        // 立即丢弃回退笔画（不落盘）并把后续交给原始路径，否则同一次运笔
        // 会记成两条。上面的守卫（lastRealPenSampleUs_==0）在这里不生效：
        // Wintab 独占时悬停报文已把该值置非零，必须单独按 tipDown 判定。
        if (s.tipDown && injPen.inStroke && injPen.fromMouseFallback) {
            LogF("[pen] drop: 收到真实落笔数据，丢弃进行中的鼠标回退笔画（避免重复计数）");
            injPen.Reset();
        }
        lastPenX = s.x; lastPenY = s.y;
        counters.penInRange = s.inRange;

        if (!TargetActive()) {
            if (AnyPenActive()) FlushPen(true);   // 收尾所有设备
            return;
        }
        // 注意：**不在这里无条件刷新活跃时刻**。
        // 悬停移动（笔尖未接触）不是作画，不该延长录屏时间；
        // 只有笔尖接触（tipDown）才算「正在操作」，见下面的落笔分支。

        ActivePen& ps = StrokeFor(s.devSeq);   // 本条采样所属的设备桶

        if (!s.inRange) {
            if (ps.inStroke || ps.inHover) FlushPenDev(ps, true);   // 只收尾本设备
            return;
        }

        // 自动标定：落笔时系统光标也跟随笔，可用光标位置反解映射偏差
        if (s.tipDown && ps.calT && (s.tUs - ps.calT) < 120000) {
            POINT cp;
            if (GetCursorPos(&cp)) {
                pen.AddCalibrationSample(s.devSeq, s.device, ps.calLX, ps.calLY, cp.x, cp.y);
            }
        }
        ps.calLX = s.logicalX;
        ps.calLY = s.logicalY;
        ps.calT = s.tUs;

        if (!s.tipDown) {
            // 悬空移动
            if (ps.inStroke) FlushPenDev(ps, true);
            if (!ps.inHover) {
                ps.inHover = true;
                ps.downUs = s.tUs;
                ps.device = s.device;
                ps.pts.clear();
                ps.haveLast = false;
                ps.nPts = 0;
            }
            AppendPenPoint(ps, s, true);
            ps.lastUs = s.tUs;
            if (ps.pts.size() > 8192) FlushPenDev(ps, true);
            return;
        }

        // 落笔
        if (!ps.inStroke) {
            if (ps.inHover) {
                ps.inHover = false;
                ps.pts.clear();
                ps.haveLast = false;
                ps.nPts = 0;
            }
            ps.inStroke = true;
            ps.downUs = s.tUs;
            ps.device = s.device;
            ps.eraser = s.eraser;
            ps.maxP = ps.sumP = ps.maxS = ps.sumS = ps.len = 0.f;
            ps.nPts = 0;
            ps.haveLast = false;
            ps.pts.clear();
            // 落笔瞬间若有任意键按下 → 整次按笔记为「键盘操作」
            ps.kbAttr = AnyKeyDown();
            if (ps.kbAttr) LogF("[pen] stroke attributed to keyboard (key held at pen-down)");
        }
        // 笔尖接触中 = 正在作画 → 刷新采样级活跃时刻。
        // 这是长运笔不被录屏切断的关键：一次连续运笔可持续数秒，
        // 期间不产生任何完成的操作，只能靠采样级信号维持活跃。
        TouchAct(s.tUs);
        AppendPenPoint(ps, s, false);
        ps.lastUs = s.tUs;
        if (ps.pts.size() > 24576) FlushPenDev(ps, true);
    }

    // 收尾单个设备桶
    void FlushPenDev(ActivePen& ps, bool all) {
        if (ps.inStroke) {
            int64_t upUs = ps.lastUs ? ps.lastUs : Clock::NowUs();
            int n = std::max(1, ps.nPts);
            EmitPenStroke(ps.downUs, upUs, false, ps.kbAttr, ps.device, ps.devSeq,
                          ps.eraser, ps.maxP, ps.sumP / (float)n,
                          ps.maxS, ps.sumS / (float)n, ps.len, ps.pts);
        } else if (all && ps.inHover) {
            int64_t upUs = ps.lastUs ? ps.lastUs : Clock::NowUs();
            EmitPenStroke(ps.downUs, upUs, true, false, ps.device, ps.devSeq,
                          ps.eraser, 0.f, 0.f, 0.f, 0.f, 0.f, ps.pts);
        }
        ps.Reset();
    }

    // 收尾所有设备桶
    void FlushPen(bool all) {
        for (auto& kv : penStrokes_) FlushPenDev(kv.second, all);
    }

    // ---------------- 定时器 ----------------
    void OnFlushTimer() {
        int64_t nowUs = Clock::NowUs();
        if (!TargetActive()) {
            if (pending.active || freeMove.active || mStroke.active ||
                AnyPenActive() || wheelPend.active) {
                FlushPendingKey(); FlushModPendings(); FlushWheel();
                FlushMouse(true); FlushPen(true);
            }
        } else {
            // —— 滚轮合并窗口到期 ——
            // 必须在这里主动收尾：用户滚一下就停手时，不会再来任何事件，
            // 若只依赖「下一条消息触发」，这组记录就会一直悬着不落盘
            // （表现为操作数迟迟不涨、退出时才突然多出一条）。
            if (wheelPend.active && wheelPend.lastUs &&
                (nowUs - wheelPend.lastUs) > (int64_t)Cfg().keyMergeMs * 1000) {
                FlushWheel();
            }

            // 进行中：只收尾已空闲的移动段，不切断进行中的笔画
            if (freeMove.active && (nowUs - freeMove.lastUs) > 500000) FlushMouse(false);

            // —— 笔笔画看门狗 ——
            // 部分应用（如 Photoshop）在画布上会吞掉/不发「抬笔」报文，
            // 导致 inRange/tipDown 的下降沿永远不来，笔画一直挂着不收尾，
            // 直到切换前台才被 FlushPen 挤出（症状：只有切前台时才出现一笔）。
            // 这里按「最近采样时刻」超时兜底：静默超过 400ms 即认为笔画已结束。
            // 逐设备桶独立判断：每块设备有自己的 lastUs，一块板静默不应
            // 误收另一块板正在进行的笔画（分桶改造后必须逐桶看门狗）。
            for (auto& kv : penStrokes_) {
                ActivePen& ps = kv.second;
                if (ps.inStroke && ps.lastUs && (nowUs - ps.lastUs) > 400000) {
                    if (!penWatchdogLogged_) {
                        penWatchdogLogged_ = true;
                        LogF("[pen] watchdog: 设备 #%u 笔画静默 %lld ms 未见抬笔事件，自动收尾"
                             "（该应用可能吞掉 in-range 下降沿）",
                             (unsigned)ps.devSeq, (long long)((nowUs - ps.lastUs) / 1000));
                    }
                    FlushPenDev(ps, false);
                }
                if (ps.inHover && ps.lastUs && (nowUs - ps.lastUs) > 900000) {
                    FlushPenDev(ps, true);
                }
            }

            // 注入鼠标回退路径同样需要看门狗（抬笔事件可能丢失）
            if (injPen.inStroke && injPen.lastUs &&
                (nowUs - injPen.lastUs) > 400000) {
                InjPenUp(injPen.lastX, injPen.lastY, 0, nowUs);
            }
        }
        Events().Flush();

        // 同步计数快照
        {
            CsLock lk(self->snapCs_);
            counters.targetActive = TargetActive();
            self->counters_ = counters;
            std::vector<PenDeviceInfo> ds = pen.Devices();
            counters.devices = ds.size();
            self->counters_.devices = ds.size();
            self->counters_.deviceNames.clear();
            for (const auto& d : ds) self->counters_.deviceNames.push_back(d.name);
        }
    }

    void FlushAll() {
        FlushPendingKey();
        FlushModPendings();
        FlushWheel();          // 退出前把最后一组滚轮也落盘，否则会丢
        FlushMouse(true);
        FlushPen(true);
        Events().Flush();
    }
};

// ============================================================
//  静态钩子回调（低级钩子必须在安装线程的消息循环中被调用）
// ============================================================
static Capture::Impl* g_impl = nullptr;

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_impl) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
        // 钩子回调必须立即返回，否则系统会因超时静默摘除钩子
        int64_t nowUs = Clock::NowUs();
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)      g_impl->OnKeyDown((int)k->vkCode, nowUs);
        else if (wp == WM_KEYUP || wp == WM_SYSKEYUP)     g_impl->OnKeyUp((int)k->vkCode, nowUs);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_impl) {
        MSLLHOOKSTRUCT* m = (MSLLHOOKSTRUCT*)lp;
        int64_t nowUs = Clock::NowUs();
        int mx = m->pt.x, my = m->pt.y;

        // 采集注入证据。m->flags 此前从未被使用，而它是唯一不依赖厂商
        // dwExtraInfo 签名的注入标志（LLMHF_INJECTED），也是诊断「未识别
        // 的注入事件」的关键证据。
        //
        // 性能：GetCurrentInputMessageSource 是一次 user32 调用，而本回调
        // 每条鼠标移动都会进入（数位笔轮询可达 1000Hz）。廉价的 FROMTOUCH
        // 签名已命中时就不查它 —— 本机 Wacom 走的就是这条快路径，零额外开销；
        // 只有签名不命中（即真实用户报告里那种环境）才付出一次查询。
        DWORD extraInfo = (DWORD)m->dwExtraInfo;
        bool sigHit = (extraInfo & PB_INJECT_SIG_MASK) == (MOUSEEVENTF_FROMTOUCH & PB_INJECT_SIG_MASK);
        InjectEvidence ev = CaptureInjectEvidence(extraInfo, m->flags, !sigHit);

        switch (wp) {
        case WM_MOUSEMOVE:   g_impl->OnMouseMove(mx, my, ev, nowUs); break;
        case WM_LBUTTONDOWN: g_impl->OnMouseButtonDown(0, mx, my, ev, nowUs); break;
        case WM_LBUTTONUP:   g_impl->OnMouseButtonUp(0, mx, my, ev, nowUs); break;
        case WM_RBUTTONDOWN: g_impl->OnMouseButtonDown(1, mx, my, ev, nowUs); break;
        case WM_MBUTTONDOWN: g_impl->OnMouseButtonDown(2, mx, my, ev, nowUs); break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: g_impl->EmitMouseClick(nowUs, 3); break;
        default: break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void CALLBACK FgProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && g_impl) g_impl->UpdateForeground(true);
}

// ============================================================
//  采集线程消息窗口
// ============================================================
static const wchar_t* const PB_CAPTURE_WNDCLASS = L"PaintingBuddyCaptureWnd";

LRESULT CALLBACK Capture::WndProcS(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT:
        if (g_impl) g_impl->pen.OnRawInput((HRAWINPUT)lp, Clock::NowUs());
        return 0;
    case WM_INPUT_DEVICE_CHANGE:
        // 热插拔通知：只记录，不在此处 Unregister+Register。
        // 重注册会再次触发本消息，形成「注册→通知→重注册」死循环。
        // 新设备的解析由 OnRawInput 里的懒加载兜底（首次收到该设备报文时 LoadDeviceInfo）。
        if (g_impl) {
            LogF("[pen] WM_INPUT_DEVICE_CHANGE type=%u handle=%p",
                 (unsigned)wp, (void*)lp);
        }
        return 0;
    case WM_TIMER:
        if (g_impl) {
            if (wp == TID_FLUSH)  g_impl->OnFlushTimer();
            if (wp == TID_FGPOLL) g_impl->UpdateForeground(false);
        }
        return 0;
    case WM_PB_STOP:
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 启动握手：采集线程装好钩子/消息窗口后立即置信号，
// 否则 Start() 只能等到线程退出，白等超时
struct CaptureStartup {
    Capture* self = nullptr;
    HANDLE   readyEvent = nullptr;
    volatile bool ok = false;
    volatile bool degraded = false;
};

DWORD WINAPI Capture::ThreadProc(LPVOID param) {
    CaptureStartup* su = (CaptureStartup*)param;
    Capture* self = su->self;
    Impl* impl = new Impl();
    impl->self = self;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &Capture::WndProcS;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = PB_CAPTURE_WNDCLASS;
    WNDCLASSW exist{};
    if (!GetClassInfoW(wc.hInstance, PB_CAPTURE_WNDCLASS, &exist)) {
        if (!RegisterClassW(&wc)) {
            LogF("[cap] RegisterClass failed: %lu", GetLastError());
            SetEvent(su->readyEvent);
            delete impl;
            return 1;
        }
    }
    // 关键：不能用 HWND_MESSAGE（纯消息窗口）。
    // Raw Input 的 WM_INPUT 对 message-only 窗口投递不可靠，会导致数位笔
    // 数据完全收不到（键盘/鼠标走全局钩子、前台走 WinEvent，均不受影响，
    // 因此症状表现为「只有笔没数据」）。
    // 改用常规 WS_POPUP 顶层窗口：创建后不 ShowWindow（保持不可见），
    // 并加 WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE 确保不进任务栏/Alt-Tab、
    // 不抢前台焦点，同时处于正常窗口层级以可靠接收 WM_INPUT。
    impl->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 PB_CAPTURE_WNDCLASS, L"PB Capture",
                                 WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!impl->hwnd) {
        LogF("[cap] CreateWindow failed: %lu", GetLastError());
        SetEvent(su->readyEvent);
        delete impl;
        return 1;
    }
    // 刻意不调用 ShowWindow：窗口保持隐藏，仅用于承载消息与 Raw Input

    g_impl = impl;
    self->impl_ = impl;
    self->hwnd_ = impl->hwnd;

    // —— 数位笔 Raw Input ——
    impl->pen.onSample = [impl](const PenSample& s) { impl->OnPenSample(s); };
    impl->pen.onDeviceFound = [](const PenDeviceInfo& d) {
        LogF("[pen] found: %s", d.name.c_str());
    };
    impl->penRegistered = impl->pen.Register(impl->hwnd);

    // 会话级状态复位 + 记录回退武装状态。
    // 显式调用（而非只依赖 new Impl() 的默认初始化）是为了让「跨会话可再次触发
    // 回退」这条用户要求成为可审查的约定，见 ResetSessionState 的说明。
    impl->ResetSessionState();
    // 这条日志是每个会话回退状态的锚点：用户回传报告后，据此确认新会话确实
    // 重新武装了回退（而不是沿用上一会话「已拿到落笔数据」的关闭状态）。
    LogF("[pen] session armed: mouseAsPenFallback=%d hasPenDevice=%d "
         "everPenTipDown=0 everInjectedPen=0（门控已按新会话复位）",
         (int)Cfg().mouseAsPenFallback, (int)impl->pen.HasPressurePenDevice());

    // —— 键盘 / 鼠标低级钩子 ——
    HINSTANCE hi = GetModuleHandleW(nullptr);
    impl->kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, hi, 0);
    impl->msHook = SetWindowsHookExW(WH_MOUSE_LL, MsProc, hi, 0);
    if (!impl->kbHook) LogF("[cap] WH_KEYBOARD_LL failed: %lu", GetLastError());
    if (!impl->msHook) LogF("[cap] WH_MOUSE_LL failed: %lu", GetLastError());

    // —— 前台窗口变化 ——
    impl->fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                   nullptr, FgProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!impl->fgHook) LogF("[cap] SetWinEventHook failed: %lu", GetLastError());

    SetTimer(impl->hwnd, TID_FLUSH, FLUSH_MS, nullptr);
    SetTimer(impl->hwnd, TID_FGPOLL, FGPOLL_MS, nullptr);

    impl->UpdateForeground(true);
    impl->counters.running = true;
    impl->initOk = impl->kbHook && impl->msHook;

    su->ok = impl->initOk;
    su->degraded = !impl->penRegistered;
    SetEvent(su->readyEvent);

    LogF("[cap] thread started: kb=%d mouse=%d pen=%d fg=%d",
         (int)(impl->kbHook != nullptr), (int)(impl->msHook != nullptr),
         (int)impl->penRegistered, (int)(impl->fgHook != nullptr));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // —— 退出清理 ——
    KillTimer(impl->hwnd, TID_FLUSH);
    KillTimer(impl->hwnd, TID_FGPOLL);
    impl->FlushAll();
    if (impl->fgHook) UnhookWinEvent(impl->fgHook);
    if (impl->kbHook) UnhookWindowsHookEx(impl->kbHook);
    if (impl->msHook) UnhookWindowsHookEx(impl->msHook);
    impl->pen.Unregister();
    g_impl = nullptr;
    self->impl_ = nullptr;
    self->hwnd_ = nullptr;
    DestroyWindow(impl->hwnd);
    LogF("[cap] thread stopped");
    delete impl;
    return 0;
}

// ============================================================
//  Capture 公共接口
// ============================================================
Capture::Capture() {
    InitializeCriticalSection(&snapCs_);
}
Capture::~Capture() {
    Stop();
    DeleteCriticalSection(&snapCs_);
}

Capture& Capture::I() {
    static Capture inst;
    return inst;
}

bool Capture::Start() {
    if (InterlockedCompareExchange(&started_, 1, 0) != 0) return true;
    InterlockedExchange(&stopReq_, 0);

    CaptureStartup* su = new CaptureStartup();
    su->self = this;
    su->readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    thread_ = CreateThread(nullptr, 0, ThreadProc, su, 0, &threadId_);
    if (!thread_) {
        LogF("[cap] CreateThread failed: %lu", GetLastError());
        CloseHandle(su->readyEvent);
        delete su;
        InterlockedExchange(&started_, 0);
        return false;
    }

    WaitForSingleObject(su->readyEvent, 10000);
    bool ok = su->ok;
    bool degraded = su->degraded;
    CloseHandle(su->readyEvent);
    delete su;

    LogF("[cap] Start -> %s%s", ok ? "ok" : "FAILED",
         degraded ? " (pen degraded)" : "");
    return ok;
}

void Capture::Stop() {
    if (!started_) return;
    if (impl_) impl_->FlushAll();
    if (threadId_ == 0 && thread_) threadId_ = GetThreadId(thread_);
    if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_) {
        WaitForSingleObject(thread_, 5000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    InterlockedExchange(&stopReq_, 1);
    InterlockedExchange(&started_, 0);
}

bool Capture::IsRunning() const { return started_ != 0; }

void Capture::SetForegroundListener(std::function<void(const ForegroundState&)> cb) { fgCb_ = std::move(cb); }
void Capture::SetOpListener(std::function<void(const OpAtom&)> cb) { opCb_ = std::move(cb); }

CaptureCounters Capture::Snapshot() {
    CsLock lk(snapCs_);
    if (impl_) {
        impl_->counters.running = true;
        impl_->counters.targetActive = impl_->TargetActive();
        counters_ = impl_->counters;
        std::vector<PenDeviceInfo> ds = impl_->pen.Devices();
        counters_.devices = ds.size();
        uint64_t raw = 0, parsed = 0;
        impl_->pen.Stats(raw, parsed);
        counters_.penRawReports = raw;
        counters_.penSamples = parsed;
        counters_.deviceNames.clear();
        for (const auto& d : ds) {
            std::string label = PenInput::DeviceTypeName(d.type);
            label += d.hasPressure ? "（有压力）" : "（无压力）";
            label += "  ";
            label += d.name;
            // 可行动提示：设备被识别、也收到了报文，却一条都没解析成功 ——
            // 这是解析路径坏了的明确信号（描述符不规范、usage 划分异常等）。
            // 此前只有全局计数，无法定位到哪台设备，且界面上静默表现为零笔画。
            // 这里直接告诉用户该导出诊断信息回传，而不是让他以为笔没反应。
            if (d.devReports > 0 && d.devParsed == 0) {
                label += "  ⚠ 收到报文但解析失败：请在设置页导出诊断信息回传";
            }
            counters_.deviceNames.push_back(label);
        }
    }
    return counters_;
}

bool Capture::IsTargetActive() {
    CsLock lk(snapCs_);
    return counters_.targetActive;
}

int64_t Capture::LastOpUs() {
    CsLock lk(snapCs_);
    return counters_.lastOpUs;
}

ForegroundState Capture::FgSnapshot() {
    CsLock lk(snapCs_);
    return fg_;
}

void Capture::FlushNow() {
    if (impl_) impl_->FlushAll();
    Events().Flush();
}

} // namespace pb
