#ifndef AUDIO_TX_H
#define AUDIO_TX_H

// =============================================================================
// audio_tx.h —— 蓝牙音频推送模块（业务层）的对外接口
//
// 【本文件的作用】
// 管理「实时音频推给手机」这条链路：三态状态机（暂停 / 实时 / 回放）、
// Opus 编码结果入环形缓冲、以及 0x20 录音回传状态机。
// 实现见 audio_tx.cpp。本模块在分层架构中属于【业务层】。
//
// 【分层约定（务必遵守）】
// 本模块**不 include ble_transport.h**，即不直接调用协议层的发送函数。
// 需要发送时一律调用**由应用层注册进来的 sink 函数指针**（见 audio_tx_set_sink）；
// 需要知道「BLE 是否可发」时，也由应用层把状态作为参数传进来（见 audio_tx_tick）。
// 这对应 Guideline 的「禁止业务层调用底层，必须经应用层协调」。
//
// 【依赖】Arduino.h、config.h、recorder.h（读录音文件）、ogg_opus.h（解 Ogg 容器）、SD/FS
// 【被谁调用】app.cpp（接线与每轮 tick）、cmd_router.cpp（命令触发的状态切换）
// =============================================================================

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 音频帧发送出口的函数指针类型（由应用层实现，通常直连 ble_transport_send_audio_frame）。
 *
 * 入参：
 *   - seq    ：2 字节包头序号（实时链路用内部自增计数，回传链路用回传序号）。
 *   - subIdx ：1 字节子序号（实时/回传都固定 0）。
 *   - payload：负载（实时=opus 数据；回传=[6B BCD 时间][opus]）。
 *   - len    ：负载长度。
 * 出参：true = 已真正发出；false = 未连接/未订阅/超长被丢弃。
 *       ⚠️ 调用方必须据此决定序号是否自增 —— 重构前序号只在真正 notify 之后才自增。
 */
typedef bool (*audio_tx_sink_fn)(uint16_t seq, uint8_t subIdx,
                                 const uint8_t *payload, size_t len);

/**
 * @brief 初始化推送模块（打印开机初始推送状态）。
 * 入参/出参：无。
 * 引用的变量（定义位置）：状态静态量 → src/audio_tx.cpp
 */
void audio_tx_init(void);

/**
 * @brief 注册音频帧发送出口。
 *
 * 功能：把「帧要发出去」这件事委托给应用层提供的函数，从而让本业务模块
 *       不必认识协议层（ble_transport）。
 * 入参：fn = 发送函数指针，通常由 app.cpp 传入 ble_transport_send_audio_frame。
 * 出参：无。
 * 引用的变量（定义位置）：s_sink → src/audio_tx.cpp 文件内静态量
 */
void audio_tx_set_sink(audio_tx_sink_fn fn);

/**
 * @brief 每轮主循环调用：按当前状态推送音频。
 *
 * 功能：等价于重构前 loop_app() 里那段闸门（原 app.cpp:1180-1186）：
 *       未连接/未订阅 → 什么都不做；回放态 → 推回传帧；否则 → 走实时链路。
 * 入参：
 *   - bleReady：BLE 是否已连接且手机已订阅音频。
 *               **由 app.cpp 读 ble_transport_is_connected() 与
 *               ble_transport_audio_subscribed() 后传入**（业务层不得反向查询协议层）。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - s_bleReady → src/audio_tx.cpp 文件内静态量（本函数写入）
 *   - 回放相关静态量 → src/audio_tx.cpp
 */
void audio_tx_tick(bool bleReady);

/**
 * @brief 收到一帧已编码的 Opus 数据，存入发送环形缓冲。
 *
 * 功能：等价于重构前 app.cpp 的 onOpusEncoded()（原 :714-743）；
 *       由 app.cpp 通过 opus_set_callback() 注册给 Opus 编码器。
 * 入参：data = Opus 数据；len = 字节数。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - s_ring / s_ringWrite / s_ringRead → src/audio_tx.cpp 文件内静态量
 *   - AUDIO_TX_BUFFER_SIZE / OPUS_OUTPUT_MAX_BYTES → src/config.h
 */
void audio_tx_on_opus(uint8_t *data, size_t len);

/**
 * @brief 手机音频订阅状态变化时的处理（“订阅即开始”）。
 *
 * 功能：等价于重构前 AudioCCCDCallback::onWrite() 里的那 4 行（原 app.cpp:847-851）。
 *       手机开启音频通知即视为「明确要音频」，若当前是暂停态则自动切到实时发送；
 *       若正在回放（REPLAY）则不打断。
 * 入参：subscribed = true 已订阅，false 取消订阅（后者不做任何动作）。
 * 出参：无。
 * 引用的变量（定义位置）：状态静态量、bleAudioTxSetState() → src/audio_tx.cpp
 * ⚠️ 绝不能改接 audio_tx_request_live()：那个函数会先停掉回放，破坏「不打断回放」的规则。
 */
void audio_tx_on_subscribe(bool subscribed);

/**
 * @brief 开始 0x20 录音回传（抢占总线，暂停实时推送）。
 * 入参/出参：无。
 * 说明：等价于重构前 app.cpp 的 startReplay()（:401-419）。由 cmd_router 调用。
 */
void audio_tx_start_replay(void);

/**
 * @brief 停止 0x20 录音回传，恢复到进入回传前的状态。
 * 入参/出参：无。
 * 说明：等价于重构前 app.cpp 的 stopReplay()（:421-430）。由 cmd_router 调用。
 */
void audio_tx_stop_replay(void);

/**
 * @brief 请求切到「实时发送」（0x30）。若正在回放则先停回放。
 * 入参/出参：无。
 * 说明：等价于重构前 app.cpp 的 bleAudioTxRequestLive()（:434-440）。
 */
void audio_tx_request_live(void);

/**
 * @brief 请求「暂停推送」（0x31）。若正在回放则先停回放。
 * 入参/出参：无。
 * 说明：等价于重构前 app.cpp 的 bleAudioTxRequestPause()（:444-450）。
 */
void audio_tx_request_pause(void);

/**
 * @brief 取当前推送状态的中文名（串口日志用）。
 * 入参：无。
 * 出参：静态字符串（调用方不得释放），如「暂停」「实时发送」「回放中」。
 * 说明：等价于重构前 app.cpp 的 bleAudioTxStateName(bleAudioTxState) 组合调用（:562）。
 */
const char *audio_tx_state_name(void);

#endif // AUDIO_TX_H
