#pragma once
// ============================================================
//  录屏文件管理：按绘画文档归档 + 缩略图 + 拼接导出
//
//  目录布局（一个绘画文档 = 一个缓存文件夹 = 界面上的一张卡片）
//  ──────────────────────────────────────────────────────────
//    videos\
//      shijian\
//        shijian_20261017_091530.mp4     ← 该文档的一次录制会话
//        shijian_20261017_103011.mp4     ← 同一文档的另一次会话
//        thumb.png                       ← 卡片缩略图（首次写帧时生成）
//      插画2\
//        插画2_20261017_142200.mp4
//        thumb.png
//      <未归档>.mp4                       ← 旧版布局遗留的散文件（如有）
//
//  导出 = 把同一文件夹内的多个 mp4 按时间顺序解码后重新编码成一个文件。
//  之所以重编码而不是拼接容器：各会话的分辨率/帧率可能不同（窗口大小
//  变过），直接拼接容器会产生播放器无法正确解码的混合流。
//  实现只用 Media Foundation，不引入 ffmpeg 等第三方依赖。
// ============================================================
#include "Common.h"
#include <string>
#include <vector>
#include <functional>

namespace pb {

// 界面上的一张录屏卡片 = 一个绘画文档的缓存文件夹
struct DocCard {
    std::wstring doc;              // 文档名（卡片标题，如 shijian）
    std::wstring folder;           // 文件夹绝对路径
    std::vector<std::wstring> files;   // 该文件夹内的 mp4（按文件名升序 = 时间升序）
    std::wstring thumbPath;        // 缩略图 PNG 路径（可能不存在）
    int64_t      totalBytes = 0;
    double       totalSeconds = 0.0;
    bool         isLegacy = false; // true = 未归档的散文件（不在子文件夹里）

    int FileCount() const { return (int)files.size(); }
};

// 目录是否存在（界面选择路径时要校验用户填的目录）
bool DirExistsW(const std::wstring& path);

// 扫描 videos 目录，返回全部卡片（按文档名排序，未归档的排最后）。
// 时长通过 MF 元数据读取（不解码整段），因此扫描很快。
std::vector<DocCard> ScanDocCards();

// 取某个文档的缓存文件夹路径（不存在则创建）
std::wstring DocFolderPath(const std::wstring& docName);

// 把 BGRA32 位图写成 PNG（缩略图用）。
// stride 为字节跨度；bits 指向首行起点。
bool SavePngFromBgra(const std::wstring& path, int w, int h, int stride, const void* bits);

// 读取一张 PNG 为 BGRA32 位图（界面绘制缩略图用）。
// 失败返回 false；成功时 out 按 w*h*4 连续存放（stride = w*4）。
bool LoadPngToBgra(const std::wstring& path, int& w, int& h, std::vector<uint8_t>& out);

// 读取一个 mp4 的时长（秒）与分辨率。失败返回 false。
bool ProbeVideo(const std::wstring& path, double& seconds, int& width, int& height);

// 解码视频首帧并缩放为缩略图，写为 PNG。
// 从视频文件生成（而非录制时抓帧）：新旧文件通用，且不侵入录制热路径。
// 耗时约等于解码一帧，应在后台线程调用。
bool MakeThumbnailFromVideo(const std::wstring& videoPath, const std::wstring& outPng,
                            int maxWidth, std::string& err);

// 某张卡片应使用的缩略图路径。
// 文件夹卡片 → <folder>\thumb.png；未归档散文件卡片 → videos\.thumbs\<doc>.png
// （刻意不移动用户既有文件，故散文件的缩略图另存于隐藏子目录）。
std::wstring ThumbPathFor(const DocCard& card);

// 拼接导出：把 card.files 依序重编码为 outPath 指向的单个 mp4。
// progress 回调收 0..1 进度，返回 false 表示请求取消。
// 出错时 err 给出原因。整个过程应在后台线程调用（耗时与总时长成正比）。
bool ConcatExport(const DocCard& card, const std::wstring& outPath,
                  std::string& err,
                  const std::function<bool(float)>& progress = nullptr);

// 把整个文件夹（或单个文件）移入 Windows 回收站（可恢复）。
// 刻意不做永久删除：录屏是用户数据，清空必须可挽回。
bool RecyclePath(const std::wstring& path, std::string& err);

} // namespace pb
