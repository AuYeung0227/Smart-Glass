#ifndef OGG_OPUS_H
#define OGG_OPUS_H

#include <Arduino.h>
#include <FS.h>
#include <ogg/ogg.h>

#include <stddef.h>
#include <stdint.h>

// 最小可用的 Ogg/Opus 容器（写 + 读两侧）。
//
// 写侧：Opus 原始包没有时间与流结构信息，裸拼不是可播放文件。Ogg 提供封装：
//   每页带 granule position（该页最后一个完成包对应的媒体位置），
//   流以两个头包开头——
//     OpusHead : 版本、声道、pre-skip、原始采样率、声道映射
//     OpusTags : vendor 字符串 + 用户注释（此处为空）
// 有这两者后，文件即可被 VLC/ffmpeg 直接作为 .opus 播放。
//
// pre-skip 取 0：16kHz 下 libopus 无需解码器延迟补偿，且 0 让 granule 直接等于
// 采样位置，回传时反推帧时间最直观。
//
// granule 单位：RFC 7845 规定 Opus 在 Ogg 中的 granule 恒为 **48kHz 刻度**。
// 本模块由调用者传入每帧的 granule48（帧结束位置的绝对样本号 × 3），
// 这样 granule 同时承担「播放时长」与「录制时刻的绝对位置」两个职责：
// 回传时 帧捕获时间 = 会话打开时刻 + (granule48/3 - 320 - 打开时s_produced)/16000。

// ---------------------------------------------------------------------------
// 写侧
// ---------------------------------------------------------------------------
typedef struct {
    ogg_stream_state os;   // libogg 页构建器
    int serial;            // 本流的序列号（区分多个流）
    int64_t packetno;      // 已写包序号（头包 0/1，音频包从 2 起，必须连续）
    bool header_done;      // OpusHead + OpusTags 已落盘
} OggOpusWriter;

/**
 * @brief 开始 Ogg/Opus 流：写 OpusHead + OpusTags 两个头页。
 * @param w      写入器
 * @param f      已打开的文件（页追加写入）
 * @param serial 流序列号（如 millis()，保证唯一即可）
 * @return true 表示头页已写入
 */
bool ogg_opus_begin(OggOpusWriter *w, File &f, uint32_t serial);

/**
 * @brief 追加一个 Opus 包（一个 20ms 帧）。
 * @param granule48 该帧结束位置的绝对采样号 × 3（48kHz 刻度）
 *
 * libogg 会攒页惰性输出，剩余缓冲由 ogg_opus_end() 冲刷。
 * 返回 false 表示 SD 写失败（卡满/拔出）。
 */
bool ogg_opus_write(OggOpusWriter *w, File &f, const uint8_t *pkt, size_t len,
                    int64_t granule48);

/**
 * @brief 冲刷剩余页并释放流状态。
 * @param eos true 表示正常结束（末页打 end-of-stream 标记，播放器不会报截断）
 */
bool ogg_opus_end(OggOpusWriter *w, File &f, bool eos);

// ---------------------------------------------------------------------------
// 读侧（回传用）
// ---------------------------------------------------------------------------
#define OGG_READ_CHUNK 4096   // 每次从 SD 读入的字节数
#define OGG_SYNC_BUF 8192     // sync 缓冲必须 ≥ 最大页尺寸（约 4.5KB）

typedef struct {
    ogg_sync_state sync;   // 从文件字节流切出页面
    ogg_stream_state os;   // 从页面里解出包
    bool stream_init;      // os 已按页序列号初始化
    int header_count;      // 已丢弃的头包数（OpusHead、OpusTags）
} OggOpusReader;

/** @brief 初始化读侧（仅分配内存，文件由 read_packet 消费）。 */
bool ogg_opus_open_read(OggOpusReader *r);

/**
 * @brief 读取下一个音频包。
 *
 * 自动丢弃 OpusHead/OpusTags 两个头包；每次调用从文件增量读入若干块，
 * 内存占用为 OGG_SYNC_BUF + 单个包缓冲。
 *
 * @param out      包数据输出缓冲
 * @param cap      out 容量
 * @param len      输出：包长度
 * @param granule48 输出：该包的 granule position（48kHz 刻度）
 * @return true 取到一帧；false = 文件结束或无更多包
 */
bool ogg_opus_read_packet(OggOpusReader *r, File &f, uint8_t *out, size_t cap,
                          size_t *len, int64_t *granule48);

/** @brief 释放读侧状态。 */
void ogg_opus_close_read(OggOpusReader *r);

#endif // OGG_OPUS_H
