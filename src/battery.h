#ifndef BATTERY_H
#define BATTERY_H

// =============================================================================
// battery.h —— 电池电量采集模块（底层）的对外接口
//
// 【本文件的作用】
// 声明「读电池 ADC、换算电压与百分比」这一组底层功能的对外接口，实现见 battery.cpp。
// 本模块在分层架构中属于【底层】：不认识 BLE，也不认识任何业务模块。
// 电量值往上只通过 battery_percentage() 取值器交出去，由应用层 app.cpp 决定
// 要不要、什么时候通过 ble_transport_update_battery() 推给手机。
//
// 【依赖】Arduino.h、config.h（ADC 引脚、分压比、电压阈值宏）
// 【被谁调用】app.cpp 的 setup_app() / loop_app()
// =============================================================================

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief 配置电池 ADC（分辨率与衰减）。
 *
 * 功能：等价于原 setup_app() 里的
 *       analogReadResolution(12) + analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db)
 *       （原 app.cpp:1108-1109）。
 * 入参：无。
 * 出参：无。
 * 引用的变量（定义位置）：BATTERY_ADC_PIN → src/config.h
 */
void battery_init(void);

/**
 * @brief 采一次电池电量，更新模块内部的电压与百分比。
 *
 * 功能：连续采 10 次 ADC 取平均 → 换算电压 → 套分压比 → 换算百分比 → 平滑与钳位，
 *       并打印一行日志。等价于原 app.cpp 的 readBatteryLevel()（:895-955）。
 *       ⚠️ 本函数内部含 10 次 delay(10)，共约 100ms 阻塞，与重构前一致。
 * 入参：无。
 * 出参：无（结果留在模块内部，用 battery_percentage() 取）。
 * 引用的变量（定义位置）：
 *   - s_batteryVoltage / s_batteryPercentage → src/battery.cpp 文件内静态量
 *   - BATTERY_ADC_PIN / VOLTAGE_DIVIDER_RATIO / BATTERY_MAX_VOLTAGE / BATTERY_MIN_VOLTAGE → src/config.h
 */
void battery_read(void);

/**
 * @brief 取最近一次 battery_read() 得到的电量百分比。
 * 入参：无。
 * 出参：0-100 的整数百分比（未读过时为 0）。
 * 引用的变量（定义位置）：s_batteryPercentage → src/battery.cpp 文件内静态量
 */
int battery_percentage(void);

#endif // BATTERY_H
