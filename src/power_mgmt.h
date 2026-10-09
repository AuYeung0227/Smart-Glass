#ifndef POWER_MGMT_H
#define POWER_MGMT_H

// =============================================================================
// power_mgmt.h —— 电源管理模块（底层）的对外接口
//
// 【本文件的作用】
// 声明「按键 ISR / LED 指示 / 电源状态机 / 深睡」这一组底层功能的对外接口，
// 实现见同目录 power_mgmt.cpp。本模块在分层架构中属于【底层】。
//
// 【分层约定（务必遵守）】
// 本模块不依赖任何业务模块（voiceprint / recorder / audio_tx 等），也不依赖协议层
// （ble_transport）。它需要知道的「BLE 是否已连接」「SD 卡是否挂载」，全部由应用层
// app.cpp 作为函数参数传进来 —— 对应 Guideline 的
// 「禁止出现业务层调用底层这种跨层调用的行为，必须经应用层协调」。
//
// 【依赖】Arduino.h、config.h（引脚号与时间阈值宏）
// 【被谁调用】app.cpp 的 setup_app() / loop_app() / 连接状态回调
// =============================================================================

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief 初始化电源管理：GPIO、按键中断、LED 初值、CPU 频率、活动时间戳。
 *
 * 功能：把原先散落在 setup_app() 里的电源相关初始化集中到一处
 *       （原 app.cpp:1088-1098 的 pinMode/digitalWrite/attachInterrupt/ledMode，
 *         以及 :1101-1102 的 setCpuFrequencyMhz/lastActivity）。
 * 入参：无。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - POWER_BUTTON_PIN / STATUS_LED_PIN / NORMAL_CPU_FREQ_MHZ → src/config.h
 *   - ledMode / lastActivity                                  → src/power_mgmt.cpp 文件内静态量
 */
void power_mgmt_init(void);

/**
 * @brief 每轮主循环调用一次：处理按键 + 刷新 LED。
 *
 * 功能：等价于原 loop_app() 里先后调用的 handleButton() 与 updateLED()。
 *       之所以把「BLE 连接状态 / SD 挂载状态」做成入参，是因为底层不允许反向查询
 *       协议层与业务层（见文件头的分层约定）。
 * 入参：
 *   - bleConnected：BLE 是否已连接。由 app.cpp 读 ble_transport_is_connected() 后传入。
 *                   供 LED 指示（已连接=常亮 / 未连接=慢闪）与关机日志使用。
 *   - sdMounted   ：SD 卡是否挂载。由 app.cpp 读 recorder_sd_ok() 后传入。
 *                   供 LED 判断「GPIO21 是否已被 SD 片选占用」使用。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - s_bleConnected / s_sdMounted → src/power_mgmt.cpp 文件内静态量（本函数写入）
 *   - buttonPressed / buttonPressTime / ledMode → src/power_mgmt.cpp 文件内静态量
 */
void power_mgmt_poll(bool bleConnected, bool sdMounted);

/**
 * @brief 每轮主循环调用一次：省电模式进入/退出判断。
 *
 * 功能：等价于原 loop_app() 尾部的省电判断块（原 app.cpp:1189-1195）。
 * 入参：
 *   - now         ：本轮循环开始时捕获的 millis()，【由 app.cpp 传入】。
 *                   ⚠️ 必须由调用方传入而不是本函数自己取 millis()：原代码就是用
 *                   循环顶部那个「陈旧的 now」去计算 now - lastActivity 的，
 *                   若这里重新取时间会改变行为（详见 power_mgmt.cpp 内注释）。
 *   - bleConnected：BLE 是否已连接。连接期间不允许进入省电模式。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - lastActivity / powerSaveMode → src/power_mgmt.cpp 文件内静态量
 *   - IDLE_THRESHOLD_MS / MIN_CPU_FREQ_MHZ / NORMAL_CPU_FREQ_MHZ → src/config.h
 */
void power_mgmt_idle_check(unsigned long now, bool bleConnected);

/**
 * @brief 记录一次「用户活动」，用于推迟进入省电模式。
 *
 * 功能：等价于原 ServerHandler::onConnect() 里的 `lastActivity = millis()`（原 app.cpp:818）。
 *       BLE 客户端连上时由 app.cpp 的连接回调调用。
 * 入参：无。
 * 出参：无。
 * 引用的变量（定义位置）：lastActivity → src/power_mgmt.cpp 文件内静态量
 */
void power_mgmt_note_activity(void);

#endif // POWER_MGMT_H
