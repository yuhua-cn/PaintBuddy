#pragma once
// ============================================================
//  存储层：目录管理、JSONL 事件流写入、原子读写、原子加载
//  数据即真相 —— 每天一个 .jsonl，只追加，永不改写历史
// ============================================================
#include "Common.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstdio>

namespace pb {

// —— 文件工具 ——
bool ReadWholeFile(const std::string& pathUtf8, std::string& out);
bool WriteWholeFileAtomic(const std::string& pathUtf8, const std::string& data);
std::vector<std::string> ListFiles(const std::string& dirUtf8, const std::string& suffixLower);
int64_t FileSizeBytes(const std::string& pathUtf8);

// —— 数据目录健康状态 ——
//
// 为什么需要它：pb.log 与当天 .jsonl 的写入失败原本完全静默 ——
// AppendLine 里 fp_ 为 null 就直接 return，而唯一记录该失败的 LogF 又写不进
// 同一个目录，于是「cannot open day log」这条关键报错永远不会出现。用户看到
// 的只是「录屏正常、总览图与习惯全空」，磁盘上一个字节的证据都没有。
//
// 这里把 EnsureDirs 的探测结果留在内存里，供诊断导出与界面报告取用，
// 使「写入失败」从不可观测变成可观测。刻意**不**在探测失败时改目录：
// 换目录会让读取端跟着换路径，既有历史数据将从界面上消失 —— 那才是真的
// 破坏正常功能。宁可如实报告，也不擅自搬家。
struct DataDirHealth {
    bool  baseOk    = true;   // 数据根目录创建成功
    bool  logsOk    = true;   // logs 子目录创建成功
    bool  writable  = true;   // logs 目录可写（临时文件探测通过）
    DWORD lastError = 0;      // 失败时的 GetLastError，0 = 无错误
    std::string note;         // 人类可读说明；空 = 一切正常
};
const DataDirHealth& DataDirHealthState();

// 打开当天日志失败后的重试冷却（微秒）。见 EventWriter::EnsureForToday。
constexpr int64_t EVENT_OPEN_RETRY_COOLDOWN_US = 30LL * 1000000LL;

// —— 开机自动启动（HKCU Run 键） ——
// 实现见 Store.cpp 末尾的说明（为何不单独建文件）。
std::wstring ExePathW();              // 当前 exe 绝对路径
bool         AutoStartApply(bool on); // 写入（带 --tray）/ 删除 Run 值，幂等
bool         AutoStartEnabled();      // 注册表里当前是否已存在该值
void         AutoStartSync(bool want);// 按期望值对齐注册表，仅在不符时写

// —— 事件流写入器（按天分文件的 JSONL） ——
class EventWriter {
public:
    // 追加一条已序列化好的 JSON 行
    void AppendLine(const std::string& line);
    void Flush();
    void Close();
    // 当天文件名变化时自动切换
    std::string CurrentPath() const;
    // 因写入端打开失败而累计丢弃的事件条数（诊断用）。
    uint64_t DroppedEvents();

private:
    void EnsureForToday(int64_t nowUs);
    std::string curDay_;
    FILE* fp_ = nullptr;
    int   pending_ = 0;
    // 上次尝试打开当天日志失败的时刻；0 = 上次尝试成功（或还没试过）。
    // 失败后必须冷却，否则每条事件都会重走一遍完整的路径拼接 + 打开尝试
    // （且必然再次失败），高频运笔时是纯白耗。见 EnsureForToday。
    int64_t lastOpenFailUs_ = 0;
    // 累计因写入失败而丢弃的事件条数。仅用于诊断导出，使「数据丢了」可量化。
    uint64_t droppedEvents_ = 0;
    std::mutex mtx_;
};

EventWriter& Events();   // 全局单例

// —— 单条事件序列化 / 追加 ——
std::string SerializeEvent(const RawRecord& r);
void        AppendEvent(const RawRecord& r);
// 解析一行已序列化的事件（SerializeEvent 的逆）。
// 主要用于单元测试验证「落盘→重载」往返一致（如键盘归属标记不丢）。
bool        ParseEventLine(const std::string& line, RawRecord& out);

// —— 键名描述（采集与分析共用，避免重复实现）——
std::string DescribeKey(int vk, uint8_t mods);

// —— 读取一天的原始事件 ——
struct DayEvents {
    std::string day;                      // YYYYMMDD
    std::vector<RawRecord> records;
};
// parsePoints=false 时跳过 pts 轨迹数组的解析（分析层完全不用轨迹，
// 只关心时间戳与分类）。当天的日志可达数 MB、其中绝大部分是轨迹点，
// 跳过解析可让「边画边自动重算」保持低开销。
bool LoadDay(const std::string& dayYYYYMMDD, DayEvents& out, bool parsePoints = true);
std::vector<std::string> ListDays();      // 有数据的所有日期，升序

} // namespace pb
