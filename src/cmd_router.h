#ifndef CMD_ROUTER_H
#define CMD_ROUTER_H

// =============================================================================
// cmd_router.h —— 命令路由模块（挂在应用层下）的对外接口
//
// 【本文件的作用】
// 把「上游收到的命令」翻译成「对业务模块的具体调用」。项目里命令有两条来路：
//   ① 手机通过 BLE 写命令特征（19B10003）—— 字节流，由协议层转发进来；
//   ② 串口控制台 —— 一行一条的可读命令（enroll / status / replay ...）。
// 两条来路最终都汇到同一张分派表，保证「同一语义只有一份实现」。
// 实现见 cmd_router.cpp。
//
// 【为什么要有这一层】
// 重构前这些代码长在 src/app.cpp 里（原 :79-255），和 BLE 回调类直接耦合 ——
// VoiceprintControlCallback::onWrite 里直接写着 voiceprintPendingPayload 等业务状态。
// 拆出来后：协议层只负责「把字节转交给注册进来的回调」，语义解释全在本模块，
// 换 BLE 栈/加命令/单独测命令表都不再需要动协议层。
//
// 【分层】路由层，挂在应用层下。允许调用业务层（audio_tx / recorder /
//         speaker_monitor），不允许调用协议层（不 include ble_transport.h）——
//         它既不知道命令从哪条链路来，也不需要自己回包。
//
// 【依赖】Arduino.h（Serial）、config.h（VP_CMD_* 宏）、audio_tx.h、
//         recorder.h、speaker_monitor.h
// 【被谁调用】app.cpp：setup_app() 里把 cmd_router_dispatch 注册给协议层；
//             loop_app() 里每轮调 cmd_router_tick()
// =============================================================================

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 手机通过 BLE 写命令特征时，协议层转交过来的原始字节。
 *
 * 功能：等价于重构前 src/app.cpp 里的 onBleCommand()（原 :695-711 的业务部分）——
 *       把写来的字节截断到 5 字节后存进「命令邮箱」，等主循环里的
 *       cmd_router_tick() 去消费。之所以不在 BLE 回调里就地执行：
 *       BLE 写入发生在 Bluedroid 任务上，而大多数命令会驱动声纹监控的共享
 *       窗口缓冲 / 读 SD 卡，必须回到主循环线程执行。
 * 入参：
 *   - payload：收到的原始字节（首字节是命令码，0x21 时后 4 字节是 unix 时间）。
 *   - len    ：字节数（协议层已保证 ≥1）。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - voiceprintPendingPayload / voiceprintPendingLen / voiceprintPendingCmd
 *     → src/cmd_router.cpp 文件内静态量（本函数写入）
 */
void cmd_router_dispatch(const uint8_t *payload, size_t len);

/**
 * @brief 每轮主循环调用一次：处理串口命令 + 消费 BLE 命令邮箱。
 *
 * 功能：等价于重构前 src/app.cpp 的 pollVoiceprintCommands()（原 :615-691）——
 *       ① 把串口收到的字符攒成一行，匹配命令名后分发；
 *       ② 若邮箱里有手机写来的命令，先打印一行日志，再分发
 *          （0x21 对时特殊处理：解析 4 字节小端 unix 后直接调 recorder_sync_time）。
 * 入参/出参：无。
 * 引用的变量（定义位置）：
 *   - voiceprintPending* 邮箱 → src/cmd_router.cpp 文件内静态量
 *   - VP_CMD_* 命令码 → src/config.h
 *   - 各业务接口 → src/speaker_monitor.h / src/recorder.h / src/audio_tx.h
 */
void cmd_router_tick(void);

#endif // CMD_ROUTER_H
