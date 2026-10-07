#ifndef RECORDER_H
#define RECORDER_H

#include <stddef.h>
#include <stdint.h>

// 录音模块：把「处理后音频」（高通 + 增益，即蓝牙发出的同一路音频）在 PSRAM 里
// 滚动缓存 30s，触发时用自带的 Opus 编码器压缩、以 Ogg/Opus (.opus) 写入 SD 卡。
//
// 两种录音会话，同一时刻只会写一个文件：
//   被动录音 recorder_start()/recorder_stop()
//       —— 声纹匹配触发。只保留触发前 30s 内「有人说话」的片段（VAD 门控，
//          说话人不必是用户本人）+ 触发之后的连续音频。
//   主动录音 recorder_active_start()/recorder_active_stop()
//       —— 手机 0x10/0x11 触发。从收到命令此刻起连续录，无预缓冲、无门控。
//   主动优先级更高：recorder_active_start() 会先停掉正在进行的被动录音，
//   并在 /omi/journal.txt 里记一条 link（被动是被这条主动打断的），供 0x12 删除用。
//
// 文件名携带元数据：p_<open_unix>_<pos>.opus / a_<open_unix>_<pos>.opus
//   open_unix = 会话打开时刻的 unix 秒（uint32；从未对时为 0）
//   pos       = 打开时刻的 s_produced（uint32，绝对样本号）
// 配合每帧 granule（绝对样本位置 ×3），回传时无需任何旁车文件即可反推
// 帧的录制时刻：帧unix = open_unix + (granule48/3 - 320 - pos)/16000。
//
// 生产者 recorder_feed() 只做 memcpy + 能量统计，绝不阻塞音频路径；
// Opus 编码与 SD 写入全部由低优先级后台任务完成。

/**
 * @brief 挂载 SD 卡、分配 PSRAM 环形缓存、创建 Opus 编码器与后台任务。
 * @return true 表示环形缓存分配成功。SD 失败非致命：仍累积预缓冲，
 *         只是各种 *_start() 会失败。
 */
bool recorder_init();

/** @brief SD 卡已挂载且可写。 */
bool recorder_sd_ok();

/**
 * @brief SD 卡诊断（串口命令 sdcheck）。
 *
 * 若当前未挂载则重新尝试 SD.begin；随后打印卡类型（无卡/MMC/SDSC/SDHC）、
 * 总容量与文件系统，帮助区分「没卡 / 格式不支持(exFAT) / 接触不良」。
 */
void recorder_sd_check();

/**
 * @brief 原始 SPI 探测（串口命令 sdprobe）。
 *
 * 绕过 SD 库，手动发 CMD0/CMD8 并打印 MISO 上真实读到的响应字节：
 *  - 收到 0x01 → 卡在通信，问题在库/初始化阶段；
 *  - 全 0xFF   → 卡完全无响应（没插卡 / 接线 / 供电 / 引脚不对）。
 */
void recorder_sd_probe();

/**
 * @brief SD 引脚电平探测（串口命令 sdpins）。
 *
 * 对 SCK/MISO/MOSI/CS 四个引脚分别做「内部上拉读 / 内部下拉读」：
 *  - 上拉读 0 且下拉读 0 → 被外部拉低（短路 GND / 接错引脚）
 *  - 上拉读 1 且下拉读 1 → 被外部拉高
 *  - 上拉读 1 且下拉读 0 → 悬空/高阻（正常的卡输入脚）
 * 用来判断 MISO 读 0x00 到底是「线被拉死」还是「卡没接上」。
 */
void recorder_sd_pins();

/**
 * @brief 原生 SD/MMC（1-bit）挂载探测（串口命令 sdmmc）。
 *
 * 某些 XIAO ESP32S3 Sense 的 microSD 是接成原生 SD/MMC 而非 SPI。
 * 本函数依次尝试几组候选 (CLK,CMD,D0) 引脚做 1-bit 挂载，打印哪一组成功、
 * 以及卡的容量——成功即说明应把存储层从 SPI(SD) 切到 SD_MMC。
 */
void recorder_sd_mmc();

/**
 * @brief 追加处理后的 PCM 到滚动预缓冲。非阻塞。
 *
 * 可从音频路径安全调用：只拷贝进 PSRAM，不分配、不加锁、不写 SD。
 */
void recorder_feed(const int16_t *samples, size_t n);

// --- 被动录音 ---

/**
 * @brief 开始被动录音：写入触发前 30s 的语音片段 + 之后的连续音频。
 * @return false 表示 SD 不可用或已有会话在写。
 */
bool recorder_start();

/** @brief 结束被动录音：排空缓冲、收尾 Ogg 流、关闭文件。 */
void recorder_stop();

/** @brief 被动录音是否正在写。 */
bool recorder_is_writing();

// --- 主动录音 ---

/**
 * @brief 开始主动录音：从此刻起连续录（无预缓冲）。
 *
 * 会先停掉正在进行的被动录音（主动优先级更高，被动已写内容保留不删），
 * 并把打断关联写入 /omi/journal.txt。
 * @return false 表示 SD 不可用。
 */
bool recorder_active_start();

/** @brief 结束主动录音并落盘。 */
void recorder_active_stop();

/** @brief 主动录音是否正在写。 */
bool recorder_active_writing();

// --- 时间（对时 / 北京时间戳）---

/**
 * @brief 手机对时（0x21 负载 = 4 字节小端 unix 秒）。
 *
 * 记录同步时刻并串口回显对应的北京时间（UTC+8），此后
 * recorder_now_unix() 按本地 millis 推算当前 unix。
 */
void recorder_sync_time(uint32_t unix);

/** @brief 当前 unix 秒（对时后有效；未对时返回 0）。 */
uint32_t recorder_now_unix();

/**
 * @brief unix 秒 → 北京时间 BCD 6 字节 {YY,MM,DD,HH,MM,SS}。
 * @param unix 0（未知时间）时输出全 0xFF（非法 BCD，手机可识别）。
 */
void recorder_beijing_bcd(uint32_t unix, uint8_t bcd[6]);

// --- 录音管理（0x12 / 0x13 / 0x20 支持）---

/**
 * @brief 删除最近一条主动录音，连同被它打断、时间相连的被动录音。
 *
 * 先停掉正在写的会话；「最近」按文件名里的 open_unix 排序（全部为 0 时
 * 退化为 pos 排序）；被打断的被动从 journal.txt 的 link 行解析。
 */
void recorder_delete_last_active();

/** @brief 删除 SD 上所有录音（/omi 下所有 .opus + journal.txt）。 */
void recorder_delete_all();

/**
 * @brief 收集 /omi 下所有录音文件名，按 (open_unix, pos) 升序（unix=0 视为最旧）。
 * @param names 输出数组（每项 64 字节）
 * @param max   数组容量
 * @return 收集到的条数（可能为 0）
 */
int recorder_collect_files(char names[][64], int max);

/**
 * @brief 由文件名 + granule 反推该帧录制时刻的北京时间 BCD。
 * @param granule48 该帧的 Ogg granule（帧结束绝对样本号 × 3）
 * @return false 表示文件名非法或录制时未对时（bcd 填 0xFF）。
 */
bool recorder_frame_time(const char *name, int64_t granule48, uint8_t bcd[6]);

/** @brief 滚动预缓存当前持有的秒数。 */
size_t recorder_prebuffer_seconds();

#endif // RECORDER_H
