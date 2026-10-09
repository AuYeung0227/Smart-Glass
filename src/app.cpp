#include "app.h"

// =============================================================================
// app.cpp —— 应用层：编排与接线
//
// 【本文件的作用】
// 1) setup_app()：按正确次序初始化各模块，并把模块之间的回调「接线」连好；
// 2) loop_app() ：每轮主循环依次调用各模块的 tick；
// 3) 三个麦克风 tap 回调（onMicData / onMicAnalysis / onMicRecord）—— 纯转发胶水；
// 4) onBleConnectionChanged()：BLE 连接事件里「记一次活动 + 上报电量」的协调。
//
// 【分层】应用层是唯一被允许同时认识所有层的地方：
//   - 它读协议层状态（ble_transport_*）后，作为参数传给底层/业务层；
//   - 它把业务层需要的发送出口（audio_tx_set_sink）与协议层的回调槽
//     （ble_transport_set_*_callback）对接起来；
//   - 它把手机命令的落点（cmd_router_dispatch）注册给协议层。
// 分层全貌与各文件职责见 WorkFlow_Files/Overview.md 的「文件与模块功能」表。
//
// 【历史】本文件原为 1218 行的单体，2026-10-09 按分层重构拆出
//   power_mgmt / battery / ble_transport / audio_tx / cmd_router 五个模块。
// =============================================================================

#include "audio_tx.h"      // 蓝牙音频推送（业务层）
#include "battery.h"       // 电池采集（底层）
#include "ble_transport.h" // BLE 协议层
#include "cmd_router.h"    // 命令路由（挂在应用层下）
#include "config.h" // Use config.h for all configurations
#include "mic.h"
#include "opus_encoder.h"
#include "ota.h"
#include "power_mgmt.h" // 按键 / LED / 电源 / 深睡（底层）
#include "recorder.h"
#include "speaker_monitor.h"

// Device power state
// 备注：deviceActive 目前只写不读、deviceState 只写不读（既有状态），
//       按「用户没让改的一律不改」原则原样保留在本文件。
bool deviceActive = true;                    // 设备是否处于活动状态（无外部引用）
device_state_t deviceState = DEVICE_BOOTING; // 设备电源状态（setup_app 里置为 ACTIVE）

// 电池检查节拍：loop_app() 用它决定多久读一次电量
unsigned long lastBatteryCheck = 0;

// Audio state
bool audioEnabled = true; // 音频总开关（只读，既有状态）

// Forward declarations
// 麦克风 tap 回调（纯转发胶水，实现见下方）
void onMicData(int16_t *data, size_t samples);
void onMicAnalysis(int16_t *data, size_t samples);
void onMicRecord(int16_t *data, size_t samples);

// 应用层内部的接线用回调
static void onBleConnectionChanged(bool isConnected);         // 协议层连接状态变化 → 本函数

// -------------------------------------------------------------------------
// Audio Functions
// -------------------------------------------------------------------------
void onMicData(int16_t *data, size_t samples)
{
    // Feed PCM data to Opus encoder
    opus_receive_pcm(data, samples);
}

void onMicAnalysis(int16_t *data, size_t samples)
{
    // Bypass tap: raw pre-gain samples for the voiceprint monitor. Read-only.
    speaker_monitor_feed(data, samples);
}

void onMicRecord(int16_t *data, size_t samples)
{
    // Bypass tap: processed (post gain/highpass) samples for the recorder, i.e.
    // the exact PCM that goes to Opus/BLE. Read-only, memcpy-only inside.
    recorder_feed(data, samples);
}

/**
 * @brief BLE 连接状态变化的处理（应用层接线用）。
 *
 * 功能：等价于重构前 ServerHandler::onConnect() 里的两件事
 *       （原 app.cpp:818 与 :821）——
 *       ① 记录一次用户活动（推迟进入省电模式）；
 *       ② 立即把当前电量上报给刚连上的手机。
 *       这两件事分别属于底层（power_mgmt）与协议层（ble_transport），
 *       由应用层在这里协调，而不是让协议层自己去调 —— 对应 Guideline 的分层要求。
 * 入参：isConnected = true 已连接 / false 已断开。
 * 出参：无。
 * 引用的变量（定义位置）：
 *   - power_mgmt_note_activity()   → src/power_mgmt.h
 *   - battery_percentage()         → src/battery.h
 *   - ble_transport_update_battery() → src/ble_transport.h
 */
static void onBleConnectionChanged(bool isConnected)
{
    if (!isConnected) {
        return; // 断开时重构前不做任何额外动作
    }
    power_mgmt_note_activity();                                   // ≡ 原 :818  lastActivity = millis()
    ble_transport_update_battery((uint8_t) battery_percentage()); // ≡ 原 :821  updateBatteryService()
}

// -------------------------------------------------------------------------
// Setup & Loop
// -------------------------------------------------------------------------

void setup_app()
{
    Serial.begin(115200);
    Serial.println("Setup started...");

    // 初始化电源管理：GPIO、按键中断、LED 初值、CPU 频率、活动时间戳
    // （逐行等价于原 app.cpp:1088-1102，实现已搬到 src/power_mgmt.cpp）
    power_mgmt_init();

    // ---- 应用层接线：把模块之间的回调连起来（这是应用层独有的职责）----
    ble_transport_set_rx_callback(cmd_router_dispatch);          // 协议层收到手机命令 → 命令路由
    ble_transport_set_conn_callback(onBleConnectionChanged);     // 协议层连接状态变化 → 本文件
    ble_transport_set_subscribe_callback(audio_tx_on_subscribe); // 协议层音频订阅变化 → 业务层
    audio_tx_set_sink(ble_transport_send_audio_frame);           // 业务层要发帧 → 协议层（经应用层转接）

    // ---- BLE 初始化 ----
    // 电量初值与 codec 标识由应用层读出后作为参数传入：协议层不得反向依赖底层/业务模块。
    // ⚠️ 保持原有次序：这里读电量时 battery_init()（ADC 配置）还没执行 ——
    //    重构前 configure_ble() 内部就是这个次序，属纯搬迁，逐位一致。
    battery_read();
    ble_transport_init((uint8_t) battery_percentage(), opus_get_codec_id());

    // Initial battery reading
    // Battery voltage divider
    battery_init(); // ≡ 原 analogReadResolution(12) + analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db)

    battery_read(); // ≡ 原 readBatteryLevel()
    deviceState = DEVICE_ACTIVE;

    // Initialize audio subsystem
    Serial.println("Initializing audio subsystem...");

    audio_tx_init(); // 打印开机初始推送状态（≡ 原 :1117-1119）

    if (opus_encoder_init()) {
        opus_set_callback(audio_tx_on_opus); // 编码结果 → 业务层 audio_tx（原为 onOpusEncoded）

        if (mic_start()) {
            mic_set_callback(onMicData);

#if VOICEPRINT_ENABLE
            // 录音 tap：喂入「处理后音频」（高通+增益，与 BLE 一致）。
            // 初始化失败不影响主音频链路。
            if (recorder_init()) {
                mic_set_recording_callback(onMicRecord);
            } else {
                Serial.println("【录音】录音模块初始化失败，音频继续。");
            }

            // 声纹监控也是纯旁路：初始化失败时 mic/Opus/BLE 完全不受影响。
            if (speaker_monitor_init()) {
                mic_set_analysis_callback(onMicAnalysis);
            } else {
                Serial.println("【声纹】声纹监控已禁用，音频继续。");
            }
#endif

            Serial.println("Audio subsystem initialized successfully.");
        } else {
            Serial.println("Failed to start microphone!");
        }
    } else {
        Serial.println("Failed to initialize Opus encoder!");
    }

    Serial.println("Setup complete.");
}

void loop_app()
{
    unsigned long now = millis();

    // Handle button presses + Update LED
    // （≡ 原 loop_app() 里先后调用的 handleButton() 与 updateLED()，已搬到 src/power_mgmt.cpp。
    //   连接状态由应用层实时读出后作为参数传入 —— 底层不得反向查询协议层。）
    power_mgmt_poll(ble_transport_is_connected(), recorder_sd_ok());

    // Process OTA updates
    ota_loop();

    // Voiceprint enrollment commands from the serial console or BLE
    // （串口命令行 + 手机写入命令两条来路，统一由 src/cmd_router.cpp 分发；
    //   原 pollVoiceprintCommands()，已搬到 src/cmd_router.cpp → cmd_router_tick()）
    cmd_router_tick();

    // Process microphone data - always run to keep audio realtime
    if (audioEnabled && mic_is_running()) {
        mic_process();
        opus_process();
    }

    // Send audio packets over BLE. During replay the live stream pauses so
    // the replayed frames (with timestamps) don't interleave with live ones.
    // 三态状态机与「本轮是否可发」的判断已移入 src/audio_tx.cpp 的 audio_tx_tick()；
    // 连接/订阅状态由应用层读出后作为参数传入（业务层不得反向查询协议层）。
    audio_tx_tick(ble_transport_is_connected() && ble_transport_audio_subscribed());

    // Check for power save mode (gentle optimization)
    // ⚠️ 传入的是循环顶部那个「陈旧的 now」，不是重新取时间：
    //    原代码正是用它去算 now - lastActivity 的（短按后 lastActivity 会大于 now
    //    → unsigned long 下溢 → 立即省电）。这是既存缺陷，纯搬迁必须逐位复现。
    //    详见 src/power_mgmt.cpp 中 power_mgmt_idle_check() 的注释。
    power_mgmt_idle_check(now, ble_transport_is_connected());

    // Check battery level periodically
    if (now - lastBatteryCheck >= BATTERY_TASK_INTERVAL_MS) {
        battery_read(); // ≡ 原 readBatteryLevel()（已搬到 src/battery.cpp）
        // ≡ 原 updateBatteryService()（已搬到 src/ble_transport.cpp）；
        //   电量由应用层取出后传入，协议层不反向依赖底层。
        ble_transport_update_battery((uint8_t) battery_percentage());
        lastBatteryCheck = now;
    }

    // Force battery update on first connection
    static bool firstBatteryUpdate = true;
    if (ble_transport_is_connected() && firstBatteryUpdate) {
        battery_read(); // ≡ 原 readBatteryLevel()
        ble_transport_update_battery((uint8_t) battery_percentage());
        firstBatteryUpdate = false;
    }

    // Adaptive delays for power saving (gentle optimization)
    if (ble_transport_audio_subscribed()) {
        delay(5); // Fast during audio streaming
    } else {
        delay(50); // Reduced delay
    }
}
