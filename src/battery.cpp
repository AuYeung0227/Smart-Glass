#include "battery.h"

// =============================================================================
// battery.cpp —— 电池电量采集模块（底层）实现
//
// 【本文件的作用】
// 读电池分压 ADC → 换算成电压 → 换算成百分比（含负载补偿、平滑、钳位），并打印日志。
// 这些代码原先是 src/app.cpp 的一部分（原 :895-955 的 readBatteryLevel，
// 以及原 :1108-1109 的 ADC 配置），按分层重构搬迁至此，算法逐行未改。
//
// 【分工说明】
// 原 app.cpp 里的 updateBatteryService()（把电量写进 BLE 特征并 notify）**不属于本模块**，
// 已移交协议层 ble_transport_update_battery()。本模块只负责「测出来」，不负责「发出去」。
//
// 【分层】底层。不 include 任何业务模块或协议层的头文件。
// 【配套头文件】battery.h（对外接口说明）
// =============================================================================

#include "config.h" // BATTERY_ADC_PIN、VOLTAGE_DIVIDER_RATIO、BATTERY_MAX/MIN_VOLTAGE 等

// -------------------------------------------------------------------------
// 文件内状态量（原为 app.cpp 的全局量，现收进本模块，均为文件静态）
// 定义位置：本文件；原来的定义位置是 src/app.cpp:21-22
// -------------------------------------------------------------------------
static float s_batteryVoltage = 0.0f; // 最近一次测得的电池电压（V）（原 app.cpp:21 的 batteryVoltage）
static int s_batteryPercentage = 0;   // 最近一次换算出的电量百分比（原 app.cpp:22 的 batteryPercentage）

/**
 * @brief 配置电池 ADC 的分辨率与衰减档位。
 * 入参/出参：无。
 * 说明：逐行等价于原 setup_app() 的 app.cpp:1108-1109。
 */
void battery_init(void)
{
    analogReadResolution(12);                           // optional: set 12-bit resolution 12 位分辨率
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db); // set attenuation for full 3.3V range 满量程 3.3V
}

/**
 * @brief 采一次电量并更新内部电压/百分比。
 * 入参/出参：无。
 * 说明：逐行等价于原 app.cpp 的 readBatteryLevel()（:895-955），仅变量改名为模块内静态量。
 */
void battery_read(void)
{
    // Take multiple ADC readings for stability
    int adcSum = 0;                          // 10 次采样累加
    for (int i = 0; i < 10; i++) {           // 采 10 次取平均，抑制抖动
        int value = analogRead(BATTERY_ADC_PIN); // 读一次 ADC
        adcSum += value;                     // 累加
        delay(10);                           // 每次间隔 10ms（共约 100ms 阻塞）
    }
    int adcValue = adcSum / 10;              // 平均值

    // ESP32-S3 ADC: 12-bit (0-4095), reference voltage ~3.3V
    float adcVoltage = (adcValue / 4095.0f) * 3.3f; // ADC 码值 → 引脚电压

    // Apply voltage divider ratio to get actual battery voltage
    s_batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO; // 乘分压比还原电池真实电压

    // Clamp voltage to reasonable range
    if (s_batteryVoltage > 5.0f)
        s_batteryVoltage = 5.0f; // 上限钳位
    if (s_batteryVoltage < 2.5f)
        s_batteryVoltage = 2.5f; // 下限钳位

    // Load-compensated battery calculation (accounts for voltage sag under load)
    float loadCompensatedMax = BATTERY_MAX_VOLTAGE; // 满电电压（带载）
    float loadCompensatedMin = BATTERY_MIN_VOLTAGE; // 空电电压（带载）

    // More accurate percentage calculation for load conditions
    if (s_batteryVoltage >= loadCompensatedMax) {
        s_batteryPercentage = 100; // 高于满电 → 100%
    } else if (s_batteryVoltage <= loadCompensatedMin) {
        s_batteryPercentage = 0; // 低于空电 → 0%
    } else {
        float range = loadCompensatedMax - loadCompensatedMin;                              // 电压区间跨度
        s_batteryPercentage = (int) (((s_batteryVoltage - loadCompensatedMin) / range) * 100.0f); // 线性插值
    }

    // Smooth percentage changes to avoid jumpy readings
    static int lastBatteryPercentage = s_batteryPercentage; // 上一次的百分比（首次即当前值）
    if (abs(s_batteryPercentage - lastBatteryPercentage) > 5) { // 跳变超过 5% 时限制步长
        s_batteryPercentage = lastBatteryPercentage + (s_batteryPercentage > lastBatteryPercentage ? 2 : -2);
    }
    lastBatteryPercentage = s_batteryPercentage; // 记录本次结果

    // Clamp percentage
    if (s_batteryPercentage > 100)
        s_batteryPercentage = 100; // 百分比上限钳位
    if (s_batteryPercentage < 0)
        s_batteryPercentage = 0; // 百分比下限钳位

    // Battery status with load info
    Serial.print("Battery: ");                    // 打印电压
    Serial.print(s_batteryVoltage);
    Serial.print("V (");
    Serial.print(s_batteryPercentage);            // 打印百分比
    Serial.print("%) [Load-compensated: ");
    Serial.print(loadCompensatedMin);             // 打印补偿区间下限
    Serial.print("V-");
    Serial.print(loadCompensatedMax);             // 打印补偿区间上限
    Serial.println("V]");
}

/**
 * @brief 取最近一次读到的电量百分比。
 * 入参/出参：无；返回 0-100 的整数。
 */
int battery_percentage(void)
{
    return s_batteryPercentage; // 直接返回模块内部静态量
}
