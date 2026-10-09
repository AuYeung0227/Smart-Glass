#include "cmd_router.h"

// =============================================================================
// cmd_router.cpp —— 命令路由模块实现
//
// 【本文件的作用】
// 1) voiceprintCmdName()        —— 命令码 → 中文名（只给日志用）；
// 2) voiceprintHandleCommand()  —— 命令码 → 具体业务调用（唯一的分派点）；
// 3) voiceprintPending* 邮箱    —— BLE 任务与主循环之间的交接区；
// 4) cmd_router_dispatch()      —— 协议层转交来的原始字节 → 存入邮箱；
// 5) cmd_router_tick()          —— 每轮主循环：读串口行 + 消费邮箱，然后分派。
// 这些代码原先是 src/app.cpp 的一部分（原 :79-255、:272-283），
// 按分层重构搬迁至此，业务调用逐行未改。
//
// 【搬迁做的唯一改动：函数名】
//   pollVoiceprintCommands() → cmd_router_tick()      （对外可见，加模块前缀）
//   onBleCommand()           → cmd_router_dispatch()  （对外可见，加模块前缀）
//   其余内部函数与变量名一律保持原样。
//
// 【分层】路由层，挂在应用层下。只调业务层，不调协议层。
// 【配套头文件】cmd_router.h（对外接口说明）
// =============================================================================

#include <Arduino.h> // Serial / strcasecmp / atol —— 必须显式包含

#include "audio_tx.h"        // audio_tx_start_replay / _stop_replay / _request_live / _request_pause / _state_name
#include "config.h"          // VP_CMD_* 命令码宏
#include "recorder.h"        // recorder_* 录音与 SD 卡接口、recorder_sync_time
#include "speaker_monitor.h" // speaker_monitor_* 声纹监控接口

// -------------------------------------------------------------------------
// 命令码 → 中文名（原 app.cpp:79-98，逐行搬迁，static 保持不变）
// -------------------------------------------------------------------------

/**
 * @brief 取命令码对应的中文名，只用于串口日志回显。
 * 入参：cmd = 命令码（VP_CMD_*，定义在 src/config.h）。
 * 出参：静态字符串，调用方不得释放；未知命令返回「未知」。
 */
static const char *voiceprintCmdName(uint8_t cmd)
{
    switch (cmd) {
    case VP_CMD_ENROLL: return "开始录入";
    case VP_CMD_FINISH: return "结束录入";
    case VP_CMD_ABORT:  return "放弃录入";
    case VP_CMD_ERASE:  return "删除模板";
    case VP_CMD_STATUS: return "查询状态";
    case VP_CMD_REC_START: return "开始主动录音";
    case VP_CMD_REC_STOP:  return "停止主动录音";
    case VP_CMD_DEL_LAST:  return "删除最近录音";
    case VP_CMD_DEL_ALL:   return "删除全部录音";
    case VP_CMD_REPLAY:    return "开始回传";
    case VP_CMD_SET_TIME:  return "对时";
    case VP_CMD_REPLAY_STOP: return "停止回传";
    case VP_CMD_AUDIO_SEND_START: return "开始推送音频";   // 0x30
    case VP_CMD_AUDIO_SEND_PAUSE: return "暂停推送音频";   // 0x31
    default:            return "未知";
    }
}

// -------------------------------------------------------------------------
// 命令码 → 业务调用（原 app.cpp:100-168，逐行搬迁，static 保持不变）
// 这是「语义 → 动作」的唯一落点：串口与 BLE 两条来路都走这里。
// -------------------------------------------------------------------------

/**
 * @brief 执行一条命令码对应的业务动作。
 * 入参：cmd = 命令码（VP_CMD_*，定义在 src/config.h）。
 * 出参：无（结果通过串口打印 + 各业务模块自身状态体现）。
 * 引用的变量（定义位置）：
 *   - VP_CMD_* → src/config.h
 *   - speaker_monitor_* → src/speaker_monitor.h
 *   - recorder_active_start / _active_stop / _delete_last_active / _delete_all → src/recorder.h
 *   - audio_tx_start_replay / _stop_replay / _request_live / _request_pause
 *     / _state_name → src/audio_tx.h
 */
static void voiceprintHandleCommand(uint8_t cmd)
{
    switch (cmd) {
    case VP_CMD_ENROLL:
        if (!speaker_monitor_enroll_start()) {                      // 请求开始录入声纹模板
            Serial.println("【命令】无法开始录入（正在录入，或声纹模块未启用）");
        }
        break;
    case VP_CMD_ABORT:
        speaker_monitor_enroll_abort();                             // 放弃本次录入
        Serial.println("【命令】已放弃录入");
        break;
    case VP_CMD_FINISH:
        // 结果行由 voiceprint_enroll_finish() 自己打印。
        if (!speaker_monitor_enroll_finish_now()) {                 // 提前结束录入并存模板
            Serial.println("【命令】当前不在录入，无需结束");
        }
        break;
    case VP_CMD_ERASE:
        speaker_monitor_erase_template();                           // 删除已存模板
        break;
    case VP_CMD_STATUS:
        Serial.printf("【命令】状态=%s 模板=%s 已录取段数=%d 音频推送=%s\n",
                      speaker_monitor_state_name(),                 // 声纹监控当前状态名
                      speaker_monitor_has_template() ? "有" : "无",  // 是否已有模板
                      speaker_monitor_enroll_segments(),            // 已录取段数
                      audio_tx_state_name());   // 追加当前蓝牙推送状态（已移交 src/audio_tx.cpp）
        break;
    case VP_CMD_REC_START:
        // 主动录音优先级更高：先停掉被动录音，再开始连续录制。
        speaker_monitor_force_idle();                               // 让声纹状态机回待机
        if (!recorder_active_start()) {                             // 开始主动录音
            Serial.println("【命令】主动录音启动失败（无SD卡？）");
        }
        break;
    case VP_CMD_REC_STOP:
        recorder_active_stop();                                     // 停止主动录音
        break;
    case VP_CMD_DEL_LAST:
        // 先保证状态机回待机（被动录音落盘），再删文件。
        speaker_monitor_force_idle();                               // 让声纹状态机回待机
        recorder_delete_last_active();                              // 删最近一条
        break;
    case VP_CMD_DEL_ALL:
        speaker_monitor_force_idle();                               // 让声纹状态机回待机
        recorder_delete_all();                                      // 删全部录音
        break;
    case VP_CMD_REPLAY:
        audio_tx_start_replay();                          // 已移交 src/audio_tx.cpp
        break;
    case VP_CMD_REPLAY_STOP:
        audio_tx_stop_replay();                           // 已移交 src/audio_tx.cpp
        break;
    case VP_CMD_AUDIO_SEND_START:
        audio_tx_request_live();                          // 0x30：开始/恢复实时推送（已移交 src/audio_tx.cpp）
        break;
    case VP_CMD_AUDIO_SEND_PAUSE:
        audio_tx_request_pause();                         // 0x31：暂停实时推送（已移交 src/audio_tx.cpp）
        break;
    case VP_CMD_SET_TIME:
        // 0x21 的负载（4 字节 unix）由 cmd_router_tick() 解析，
        // 不会走到这里。
        Serial.println("【命令】对时命令缺少时间负载");
        break;
    default:
        Serial.printf("【命令】未知命令 0x%02X\n", cmd);
        break;
    }
}

// -------------------------------------------------------------------------
// 命令邮箱（原 app.cpp:170-175，逐行搬迁）
// BLE 写入发生在 Bluedroid 任务上，不能由它去驱动声纹监控的共享窗口缓冲，
// 所以把命令存进邮箱，交给主循环里的 cmd_router_tick() 执行。
// -------------------------------------------------------------------------

// BLE writes arrive on the Bluedroid task, which must not drive the monitor's
// shared window buffer. Queue the command and let the main loop run it.
// 0x21（对时）需要带 4 字节 unix 负载，所以把整个 value（≤5 字节）一起存下来。
static volatile uint8_t voiceprintPendingPayload[5]; // 邮箱：命令码 + 最多 4 字节负载
static volatile size_t voiceprintPendingLen = 0;     // 邮箱里实际字节数
static volatile uint8_t voiceprintPendingCmd = 0;    // 邮箱就绪标志（0 = 空）

// -------------------------------------------------------------------------
// 主循环侧：读串口行 + 消费邮箱（原 pollVoiceprintCommands()，:179-255）
// -------------------------------------------------------------------------

/**
 * @brief 每轮主循环调用：处理串口命令，并消费 BLE 命令邮箱。
 * 入参/出参：无。
 * 引用的变量（定义位置）：
 *   - line / len（本函数内 static 局部量，跨轮保持半行输入）
 *   - voiceprintPending* 邮箱（本文件上方）
 *   - 命令码宏 VP_CMD_* → src/config.h
 *   - recorder_sd_check / _sd_probe / _sd_pins / _sd_mmc / _sync_time → src/recorder.h
 */
void cmd_router_tick(void)
{
    static char line[32];                                // 串口行缓冲（原 app.cpp:181）
    static size_t len = 0;                               // 当前已攒字节数（原 app.cpp:182）

    while (Serial.available() > 0) {                     // 把串口缓冲区里的字符全部读走
        char c = (char)Serial.read();                    // 取一个字符
        if (c == '\r' || c == '\n') {                    // 行结束符
            if (len == 0) {                              // 空行直接忽略
                continue;
            }
            line[len] = '\0';                            // 补字符串结尾
            len = 0;                                     // 缓冲复位，准备下一行

            // ---- 以下是串口命令名 → 命令码的映射（大小写不敏感）----
            if (!strcasecmp(line, "enroll")) {           // 开始录入
                voiceprintHandleCommand(VP_CMD_ENROLL);
            } else if (!strcasecmp(line, "abort")) {     // 放弃录入
                voiceprintHandleCommand(VP_CMD_ABORT);
            } else if (!strcasecmp(line, "finish")) {    // 结束录入
                voiceprintHandleCommand(VP_CMD_FINISH);
            } else if (!strcasecmp(line, "erase")) {     // 删模板
                voiceprintHandleCommand(VP_CMD_ERASE);
            } else if (!strcasecmp(line, "status")) {    // 查状态
                voiceprintHandleCommand(VP_CMD_STATUS);
            } else if (!strcasecmp(line, "recstart")) {  // 开始主动录音
                voiceprintHandleCommand(VP_CMD_REC_START);
            } else if (!strcasecmp(line, "recstop")) {   // 停止主动录音
                voiceprintHandleCommand(VP_CMD_REC_STOP);
            } else if (!strcasecmp(line, "dellast")) {   // 删最近一条
                voiceprintHandleCommand(VP_CMD_DEL_LAST);
            } else if (!strcasecmp(line, "delall")) {    // 删全部
                voiceprintHandleCommand(VP_CMD_DEL_ALL);
            } else if (!strcasecmp(line, "replay")) {    // 开始回传
                voiceprintHandleCommand(VP_CMD_REPLAY);
            } else if (!strcasecmp(line, "replaystop")) { // 停止回传
                voiceprintHandleCommand(VP_CMD_REPLAY_STOP);
            } else if (!strcasecmp(line, "sendstart")) { // 开始/恢复实时推送
                voiceprintHandleCommand(VP_CMD_AUDIO_SEND_START);   // 串口别名：开始推送
            } else if (!strcasecmp(line, "sendstop")) {  // 暂停实时推送
                voiceprintHandleCommand(VP_CMD_AUDIO_SEND_PAUSE);   // 串口别名：暂停推送
            } else if (!strcasecmp(line, "sdcheck")) {   // SD 卡检查
                recorder_sd_check();
            } else if (!strcasecmp(line, "sdprobe")) {   // SD 卡探测
                recorder_sd_probe();
            } else if (!strcasecmp(line, "sdpins")) {    // 打印 SD 引脚
                recorder_sd_pins();
            } else if (!strcasecmp(line, "sdmmc")) {     // SD 走 MMC 模式
                recorder_sd_mmc();
            } else if (!strncasecmp(line, "settime", 7)) { // 串口对时
                // 串口对时：settime <unix 秒>
                uint32_t unix = (uint32_t)atol(line + 7);  // 跳过 "settime" 取后面的秒数
                recorder_sync_time(unix);                  // 交给录音模块记录时间基准
            } else {
                Serial.printf("【命令】无法识别的命令 '%s'\n", line); // 未知命令提示
            }
        } else if (len < sizeof(line) - 1) {             // 普通字符：只要没满就攒进缓冲
            line[len++] = c;
        }
    }

    // ---- 消费 BLE 命令邮箱 ----
    uint8_t pending = voiceprintPendingCmd;              // 取命令码（同时充当「有新命令」标志）
    if (pending != 0) {
        voiceprintPendingCmd = 0;                        // 先清标志，避免命令被重复执行
        Serial.printf("【命令】手机写入 0x%02X（%s）\n", pending,
                      voiceprintCmdName(pending));       // 日志回显命令名
        if (pending == VP_CMD_SET_TIME && voiceprintPendingLen >= 5) { // 0x21 且带够 4 字节负载
            // 0x21 负载 = 4 字节小端 unix 时间戳（秒）。
            uint32_t unix = (uint32_t)voiceprintPendingPayload[1]
                          | ((uint32_t)voiceprintPendingPayload[2] << 8)
                          | ((uint32_t)voiceprintPendingPayload[3] << 16)
                          | ((uint32_t)voiceprintPendingPayload[4] << 24); // 小端拼出 uint32
            recorder_sync_time(unix);                    // 直接对时，不走命令表
        } else {
            voiceprintHandleCommand(pending);            // 其余命令走统一分派
        }
    }
}

// -------------------------------------------------------------------------
// 协议层侧：接收原始字节（原 onBleCommand()，:272-283）
// -------------------------------------------------------------------------

/**
 * @brief 手机写入命令特征（19B10003）的接收端（由 app.cpp 注册给协议层）。
 *
 * 功能：把手机写来的字节截断到 5 字节后存进命令邮箱，供 cmd_router_tick()
 *       在主循环里消费。之所以不在 BLE 回调里直接执行命令：BLE 写入发生在
 *       Bluedroid 任务上，不能由它去驱动声纹监控的共享窗口缓冲，
 *       必须甩回主循环处理。
 * 入参：payload = 原始字节；len = 字节数。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - voiceprintPendingPayload / voiceprintPendingLen / voiceprintPendingCmd
 *     → src/cmd_router.cpp 上方（本文件内静态量）
 * 备注：协议层的「取长度、空写入早退」已在 src/ble_transport.cpp 的
 *       VoiceprintControlCallback 里完成，本函数只负责业务侧落库。
 */
void cmd_router_dispatch(const uint8_t *payload, size_t len)
{
    // 先写负载字节，最后写命令字节：tick 一看到 cmd 非零，负载必已就位。
    size_t n = (len > sizeof(voiceprintPendingPayload))
                   ? sizeof(voiceprintPendingPayload)
                   : len;                                    // 截断到邮箱容量（最多 5 字节）
    for (size_t i = 0; i < n; i++) {                         // 逐字节存入邮箱
        voiceprintPendingPayload[i] = payload[i];
    }
    voiceprintPendingLen = n;                                // 记下实际长度
    voiceprintPendingCmd = voiceprintPendingPayload[0];      // 最后写命令字节（作为「就绪」标志）
}
