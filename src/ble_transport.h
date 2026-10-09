#ifndef BLE_TRANSPORT_H
#define BLE_TRANSPORT_H

// =============================================================================
// ble_transport.h —— BLE 传输/协议层（GATT）的对外接口
//
// 【本文件的作用】
// 声明「BLE 协议栈初始化、GATT 服务与特征注册、连接状态、特征通知（notify）」
// 这一组协议层功能的对外接口，实现见 ble_transport.cpp。
// 本模块在分层架构中属于【协议层】。
//
// 【分层约定（务必遵守）】
// 本模块**不认识任何业务模块**（voiceprint / recorder / audio_tx 等）。
// 它不会直接调用业务函数，只把「收到了什么」交给上面注册进来的回调：
//   - 手机写入命令特征  → s_rxFn        （由 app.cpp 注册 cmd_router 的分发函数）
//   - 连接/断开         → s_connFn      （由 app.cpp 注册）
//   - 音频订阅状态变化  → s_subFn       （由 app.cpp 注册 audio_tx 的处理函数）
// 这一设计对应 Guideline 的「必须经应用层协调」，也是本次重构要切断的核心耦合点：
// 重构前 VoiceprintControlCallback 里直接写着业务函数名（如 startReplay()）。
//
// 【依赖】Arduino.h、BLEDevice.h 等 BLE 库、ota.h（同层协议 peer）、config.h
// 【被谁调用】app.cpp（setup_app 接线 + loop_app 查询状态）
// =============================================================================

#include <Arduino.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 手机写入命令特征（19B10003）时的回调类型。
 * 入参：payload = 收到的原始字节；len = 字节数。
 * 说明：本模块不做任何解析，把原始字节原样转发给注册方。
 */
typedef void (*omi_ble_rx_fn)(const uint8_t *payload, size_t len);

/**
 * @brief BLE 连接状态变化 / 音频订阅状态变化的回调类型。
 * 入参：connected = true 表示已连接 / 已订阅，false 表示断开 / 取消订阅。
 */
typedef void (*omi_ble_conn_fn)(bool connected);

/**
 * @brief 初始化 BLE：起协议栈、建服务与特征、开始广播。
 *
 * 功能：等价于重构前 app.cpp 的 configure_ble()（原 :972-1076）。
 * 入参：
 *   - initialBatteryPct：开机首次电量百分比。**由应用层读出后传入**
 *                        （协议层不认识 battery 模块）。写进电池特征作初值。
 *   - audioCodecId     ：音频 codec 标识。**由应用层读出后传入**
 *                        （协议层不认识 opus 模块）。写进 codec 特征作初值。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - UUID / 设备名 / MTU / 广播间隔等宏 → src/config.h
 *   - s_connected / s_audioSubscribed / 各特征指针 → src/ble_transport.cpp 文件内静态量
 * 注意：必须在 app.cpp 里先 `ble_transport_set_*_callback()` 注册好回调再调用本函数，
 *       否则回调为空指针时对应事件会被静默忽略。
 */
void ble_transport_init(uint8_t initialBatteryPct, uint8_t audioCodecId);

/**
 * @brief 查询 BLE 是否已连接。
 * 入参：无。出参：true = 有客户端连接。
 * 引用的变量（定义位置）：s_connected → src/ble_transport.cpp 文件内静态量
 */
bool ble_transport_is_connected(void);

/**
 * @brief 查询手机是否已订阅音频通知（写过 CCCD 的 notify 位）。
 * 入参：无。出参：true = 已订阅，可以推音频。
 * 引用的变量（定义位置）：s_audioSubscribed → src/ble_transport.cpp 文件内静态量
 */
bool ble_transport_audio_subscribed(void);

/**
 * @brief 更新电池特征的数值（连接时同时 notify 给手机）。
 *
 * 功能：等价于重构前 app.cpp 的 updateBatteryService()（原 :957-967）——
 *       重构前它是「取 app.cpp 的全局电量 + 写特征 + 连接时 notify」三合一，
 *       现在拆成「应用层取电量（battery_percentage）→ 传入本函数写并上报」。
 * 入参：pct = 0-100 的电量百分比。
 * 出参：无。
 * 引用的变量（定义位置）：s_batteryLevelCharacteristic / s_connected → src/ble_transport.cpp
 */
void ble_transport_update_battery(uint8_t pct);

/**
 * @brief 通过音频特征（19B10001）发一帧数据给手机。
 *
 * 功能：拼上 3 字节包头 [2B 序号][1B 子序号] 后 setValue + notify。
 *       等价于重构前 app.cpp 的 broadcastAudioPacket()（原 :745-761），
 *       并统一接管了回传路径原先自己拼包发通知的那段（原 :499-507）。
 * 入参：
 *   - seq    ：2 字节包头序号（实时链路用 audioPacketIndex，回传链路用 replaySeq）。
 *   - subIdx ：1 字节子序号（实时链路固定 0；留给将来分片用）。
 *   - payload：负载字节（实时链路是 opus 数据；回传链路是 [6B BCD 时间][opus]）。
 *   - len    ：payload 字节数。
 * 出参：**true = 真的发出去了**（notify 已调用）；false = 未连接/未订阅/超长，已丢弃。
 *       返回值是给调用方判断「序号要不要自增」用的 —— 重构前序号只在真正
 *       notify 之后才自增，返回值保证这一语义不变。
 * 引用的变量（定义位置）：
 *   - s_packetBuf / s_audioDataCharacteristic / s_connected / s_audioSubscribed → src/ble_transport.cpp
 *   - AUDIO_PACKET_HEADER_SIZE / OPUS_OUTPUT_MAX_BYTES / REPLAY_PACKET_MAX_BYTES → src/config.h
 */
bool ble_transport_send_audio_frame(uint16_t seq, uint8_t subIdx,
                                    const uint8_t *payload, size_t len);

/**
 * @brief 注册「手机写入命令特征」的回调（协议层 → 上层的唯一入口）。
 * 入参：fn = 回调函数指针，传 nullptr 表示取消注册。
 * 出参：无。
 * 引用的变量（定义位置）：s_rxFn → src/ble_transport.cpp 文件内静态量
 */
void ble_transport_set_rx_callback(omi_ble_rx_fn fn);

/**
 * @brief 注册「BLE 连接/断开」的回调。
 * 入参：fn = 回调函数指针。连接与断开都会调用；断开时传 false。
 * 出参：无。
 * 引用的变量（定义位置）：s_connFn → src/ble_transport.cpp 文件内静态量
 */
void ble_transport_set_conn_callback(omi_ble_conn_fn fn);

/**
 * @brief 注册「音频订阅状态变化」的回调。
 * 入参：fn = 回调函数指针。手机开通知=true，关通知=false。
 * 出参：无。
 * 引用的变量（定义位置）：s_subFn → src/ble_transport.cpp 文件内静态量
 */
void ble_transport_set_subscribe_callback(omi_ble_conn_fn fn);

#endif // BLE_TRANSPORT_H
