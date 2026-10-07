#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// BOARD CONFIGURATION
// =============================================================================
#define BOARD_HAS_PSRAM           // Enable PSRAM support
#define CONFIG_ARDUHAL_ESP_LOG    // Enable Arduino HAL logging

// =============================================================================
// DEVICE CONFIGURATION
// =============================================================================
#define BLE_DEVICE_NAME "OMI Glass"
#define FIRMWARE_VERSION_STRING "2.3.2"
#define HARDWARE_REVISION "ESP32-S3-v1.0"
#define MANUFACTURER_NAME "Based Hardware"

// =============================================================================
// POWER MANAGEMENT - Optimized for MINIMUM 6-8 hours, targeting 10+ hours
// =============================================================================
// CPU Frequency Management - Aggressive power optimization
#define MAX_CPU_FREQ_MHZ 100   // Further reduced from 120MHz - still sufficient
#define MIN_CPU_FREQ_MHZ 40    // Ultra-low power for idle states
#define NORMAL_CPU_FREQ_MHZ 80 // Normal operation frequency (good balance)

// Sleep Management
#define LIGHT_SLEEP_DURATION_US 50000  // 50ms light sleep intervals
#define DEEP_SLEEP_THRESHOLD_MS 300000 // 5 minutes of inactivity triggers deep sleep
#define IDLE_THRESHOLD_MS 45000        // 45 seconds to enter power save mode (was 30s)

// Battery Configuration - Dual 250mAh @ 3.5V-4.1V under load (500mAh total)
#define BATTERY_MAX_VOLTAGE 4.2f      // 4.2V fully charged (under load)
#define BATTERY_MIN_VOLTAGE 3.2f      // 3.2V empty (under load)
#define BATTERY_CRITICAL_VOLTAGE 3.3f // Emergency shutdown voltage
#define BATTERY_LOW_VOLTAGE 3.4f      // Low battery warning
#define VOLTAGE_DIVIDER_RATIO 6.086f  // Calibrated to match multimeter readings (load-compensated)

// Battery Monitoring - Extended intervals for power savings
#define BATTERY_REPORT_INTERVAL_MS 90000 // 1.5 minute reporting (was 60s)
#define BATTERY_TASK_INTERVAL_MS 20000   // 20 second internal checks (was 15s)
#define BATTERY_ADC_PIN 2                // GPIO2 (A1) - voltage divider connection

// =============================================================================
// BLE CONFIGURATION - Power optimized for extended battery life
// =============================================================================
#define BLE_MTU_SIZE 517            // Maximum MTU for efficiency
#define BLE_TX_POWER ESP_PWR_LVL_N0 // Low power for 6+ hour battery life

// Power-optimized BLE Advertising - Longer intervals for power savings
#define BLE_ADV_MIN_INTERVAL 0x0140  // 200ms minimum (was 160ms)
#define BLE_ADV_MAX_INTERVAL 0x0280  // 400ms maximum (was 320ms)
#define BLE_ADV_TIMEOUT_MS 0         // Never stop advertising (always discoverable)
#define BLE_SLEEP_ADV_INTERVAL 45000 // Re-advertise every 45 seconds when not connected (was 30s)

// Connection Management - Stable connections with power optimization
#define BLE_CONNECTION_TIMEOUT_MS 0 // Never timeout connections (disable auto-disconnect)
#define BLE_TASK_INTERVAL_MS 20000  // 20 second connection check (was 15s)
#define BLE_TASK_STACK_SIZE 2048
#define BLE_TASK_PRIORITY 1

// Connection Parameters for Stable Connections with Power Optimization
#define BLE_CONN_MIN_INTERVAL 20 // 25ms minimum connection interval (was 20ms)
#define BLE_CONN_MAX_INTERVAL 40 // 50ms maximum connection interval (was 40ms)
#define BLE_CONN_LATENCY 0       // No latency for immediate response
#define BLE_CONN_TIMEOUT 800     // 8 second supervision timeout

// =============================================================================
// POWER STATES
// =============================================================================
typedef enum {
    POWER_STATE_ACTIVE,      // Normal operation - BLE active
    POWER_STATE_POWER_SAVE,  // Reduced frequency, longer intervals
    POWER_STATE_LOW_BATTERY, // Minimal operation
    POWER_STATE_SLEEP        // Deep sleep mode
} power_state_t;

// =============================================================================
// TASK CONFIGURATION - Optimized stack sizes
// =============================================================================
#define BATTERY_TASK_STACK_SIZE 2048
#define BATTERY_TASK_PRIORITY 1
#define POWER_MANAGEMENT_TASK_STACK_SIZE 2048
#define POWER_MANAGEMENT_TASK_PRIORITY 0

// Status Reporting - Power optimized
#define STATUS_REPORT_INTERVAL_MS 120000 // 2 minutes (was 30 seconds)

// =============================================================================
// MICROPHONE CONFIGURATION - I2S PDM (XIAO ESP32S3 Sense built-in mic)
// =============================================================================
// XIAO ESP32S3 Sense has built-in PDM microphone
#define MIC_CLK_PIN 42  // PDM Clock pin (GPIO42)
#define MIC_DATA_PIN 41 // PDM Data pin (GPIO41)

#define MIC_SAMPLE_RATE 16000          // 16kHz sample rate
#define MIC_BUFFER_SAMPLES 1600        // 100ms buffer (16000 * 0.1)
#define MIC_GAIN 24                     // Microphone gain multiplier
#define DESPIKE_BASELINE_FLOOR 200      // 自适应去尖峰：基线幅值下限
#define DESPIKE_RATIO 2.5                 // 自适应去尖峰：尖刺判定倍数
#define MAX_SPIKE_SAMPLES 48            // 最大尖刺宽度(采样点)，超过判为正常语音
#define DIFF_THRESHOLD_RATIO 1.3          // 尖刺起点相邻差 > baseline*该值
#define EMA_SHIFT 8                     // EMA 平滑 1/(2^6)≈4ms 跟踪速度
#define AUDIO_RING_BUFFER_SAMPLES 8000 // 500ms of audio data

// 门控去脉冲（仅在停顿/静音时剔除孤立咔哒声；语音段旁路，不伤辅音）
#define DECLICK_GATE_LEVEL 700   // 背景电平(mean-abs)低于此才开启门控（增益后域）
#define DECLICK_MIN_JUMP  500   // 样本偏离局部中值至少此值才判为脉冲并替换

// =============================================================================
// OPUS CODEC CONFIGURATION
// =============================================================================
#define AUDIO_CODEC_ID 21              // Opus codec ID (matches Omi protocol)
#define OPUS_FRAME_SAMPLES 320         // 20ms frame @ 16kHz
#define OPUS_OUTPUT_MAX_BYTES 160      // Max encoded frame size
#define OPUS_BITRATE 32000             // 32kbps
#define OPUS_COMPLEXITY 3              // Encoding complexity (1-10)
#define OPUS_VBR 1                     // Variable bitrate enabled

// Audio BLE packet configuration
#define AUDIO_PACKET_HEADER_SIZE 3     // 2 bytes index + 1 byte sub-index
#define AUDIO_TX_RING_BUFFER_SIZE 16   // Number of encoded frames to buffer

// =============================================================================
// SPEAKER VOICEPRINT + SMART RECORDING CONFIGURATION
// =============================================================================
// Bypass module: taps the raw pre-gain I2S stream, never touches the Opus path.
// MFCC parameters MUST match utils/mfcc.py in the training repo, or the model
// will see features it was not trained on.
#define VOICEPRINT_ENABLE 1

// --- VAD (energy gate, idle state only) ---
#define VAD_FRAME_SAMPLES 320              // 20ms @ 16kHz
#define VAD_ENERGY_THRESHOLD 0.0025f       // mean-abs of normalized [-1,1]; matches reference
#define VAD_TRIGGER_FRAMES 3               // consecutive speech frames to leave idle state

// --- MFCC front-end (must match utils/mfcc.py) ---
#define MFCC_WIN_SIZE 512                  // 32ms window
#define MFCC_HOP_SIZE 256                  // 16ms hop (50% overlap)
#define MFCC_MEL_BANDS 20
#define MFCC_NUM_FRAMES 63                 // frames kept after centre-crop/pad
#define MFCC_FREC_MIN 20
#define MFCC_FREC_MAX 8000                 // = sample rate / 2
#define MFCC_PREEMPH 0.97f
#define MFCC_EPSILON 1e-6f
#define MFCC_SEGMENT_SAMPLES (MIC_SAMPLE_RATE * 2)                 // 2s analysis window
#define MFCC_SEGMENT_HOP_SAMPLES (MFCC_SEGMENT_SAMPLES / 4)        // 0.5s sliding hop
#define MFCC_XI_DIM (4 * MFCC_MEL_BANDS)                           // 80 = 4 x 20

// --- Voiceprint matching (template-based, no neural network) ---
// Xi-Vector is a deterministic feature, so enrollment is pure averaging - no
// gradient training, no PC. One template = 640 B in SPIFFS.
#define VOICEPRINT_ENROLL_SECONDS 20        // how long enrollment records
#define VOICEPRINT_ENROLL_MIN_SEGMENTS 6    // reject enrollment with fewer than this
#define VOICEPRINT_MATCH_THRESHOLD 1.2f     // normalized distance; below = same speaker
#define VOICEPRINT_TEMPLATE_PATH "/speaker_template.bin"  // 2 x 80 float32 (mean, sigma)

// --- State machine timeouts ---
#define VERIFY_TIMEOUT_MS 30000            // 待验证 -> 待机 if no match within 30s

// 被动录音的两个停止条件（任一满足即停录）
#define PASSIVE_SILENCE_TIMEOUT_MS 10000   // 有人说话后连续 10s 安静 -> 停止被动录音
#define PASSIVE_NOMATCH_TIMEOUT_MS 30000   // 上次匹配用户后 30s 内无再次匹配 -> 停止被动录音

// --- Recording ---
#define RECORD_PREBUFFER_SECONDS 30        // 滚动 PSRAM 预缓存秒数（处理后 PCM）
#define RECORD_SD_CS_PIN 21                // 与 STATUS_LED_PIN 共用（扩展板 SD 片选）
#define RECORD_SD_DIR "/omi"
// 被动录音的预缓冲语音门控：只保留触发前「有人说话」的片段（说话人不必是用户本人）。
// 门限作用在「处理后音频」（已高通 + MIC_GAIN 增益），故数值远高于原始域。
#define RECORD_VAD_ENERGY_THRESHOLD 0.0025f // 处理后音频的语音门限（mean-abs，需实测调校）
#define PREBUFFER_SPEECH_HANGOVER_FRAMES 5  // 语音段尾部多留 5 帧(100ms)，防截尾
// 录音任务每个 tick 最多编码/写出的 Opus 帧数：限制单次占用时长，
// 既保证预缓冲回填能追平，又不长时间阻塞其它任务。
#define RECORD_FRAMES_PER_TICK 8
// SD 容量检测间隔
#define SD_CAPACITY_CHECK_INTERVAL_MS 60000 // 每 60s 检测一次剩余容量

// --- Task ---
#define VOICEPRINT_TASK_STACK_SIZE 8192
#define VOICEPRINT_TASK_PRIORITY 1

// =============================================================================
// BLE UUID DEFINITIONS - OMI Protocol
// =============================================================================
#define OMI_SERVICE_UUID "19B10000-E8F2-537E-4F6C-D104768A1214"
#define AUDIO_DATA_UUID "19B10001-E8F2-537E-4F6C-D104768A1214"
#define AUDIO_CODEC_UUID "19B10002-E8F2-537E-4F6C-D104768A1214"
#define VOICEPRINT_CONTROL_UUID "19B10003-E8F2-537E-4F6C-D104768A1214"  // write commands

// Voiceprint control commands (BLE 19B10003 write, or serial line commands)
#define VP_CMD_ENROLL 0x01   // start recording a template
#define VP_CMD_FINISH 0x02   // end enrollment early and save
#define VP_CMD_ABORT 0x03    // discard the in-progress enrollment
#define VP_CMD_ERASE 0x04    // delete the stored template
#define VP_CMD_STATUS 0x05   // print state (serial only)
#define VP_CMD_REC_START 0x10 // 开始主动录音（从收到命令此刻起连续录）
#define VP_CMD_REC_STOP 0x11  // 停止主动录音
#define VP_CMD_DEL_LAST 0x12  // 删除最近一条主动录音（连同被其打断、时间相连的被动录音）
#define VP_CMD_DEL_ALL 0x13   // 删除 SD 卡所有录音
#define VP_CMD_REPLAY 0x20    // 开始回传全部录音（按时间顺序，逐帧带北京时间戳）
#define VP_CMD_SET_TIME 0x21  // 手机对时：负载 = 4 字节小端 unix 时间戳（秒）
#define VP_CMD_REPLAY_STOP 0x22 // 停止回传

// --- 录音回传 ---
// 回传按 granule 差值还原原始时间轴（含被删静音的间隔）；每帧间隔截断到该范围。
#define REPLAY_MIN_FRAME_DELAY_MS 10
#define REPLAY_MAX_FRAME_DELAY_MS 1000
#define REPLAY_MAX_FILES 64          // 单次回传最多处理的录音文件数
#define REPLAY_PACKET_MAX_BYTES (AUDIO_PACKET_HEADER_SIZE + 6 + OPUS_OUTPUT_MAX_BYTES) // [3B头][6B BCD][opus]

// Battery Service UUID - Cast to uint16_t for BLE compatibility
#define BATTERY_SERVICE_UUID (uint16_t) 0x180F
#define BATTERY_LEVEL_UUID (uint16_t) 0x2A19

// OTA Service UUIDs
#define OTA_SERVICE_UUID "19B10010-E8F2-537E-4F6C-D104768A1214"
#define OTA_CONTROL_UUID "19B10011-E8F2-537E-4F6C-D104768A1214"  // Write commands, read status
#define OTA_DATA_UUID "19B10012-E8F2-537E-4F6C-D104768A1214"     // Notifications for progress

// OTA Commands (written to OTA_CONTROL_UUID)
#define OTA_CMD_SET_WIFI 0x01       // Set WiFi credentials: [cmd, ssid_len, ssid..., pass_len, pass...]
#define OTA_CMD_START_OTA 0x02      // Start OTA update: [cmd, url_len, url...]
#define OTA_CMD_CANCEL_OTA 0x03     // Cancel ongoing OTA
#define OTA_CMD_GET_STATUS 0x04     // Request current status
#define OTA_CMD_SET_URL 0x05        // Set firmware URL: [cmd, url_len, url...]

// OTA Status codes (notified via OTA_DATA_UUID)
#define OTA_STATUS_IDLE 0x00
#define OTA_STATUS_WIFI_CONNECTING 0x10
#define OTA_STATUS_WIFI_CONNECTED 0x11
#define OTA_STATUS_WIFI_FAILED 0x12
#define OTA_STATUS_DOWNLOADING 0x20      // Followed by progress byte (0-100)
#define OTA_STATUS_DOWNLOAD_COMPLETE 0x21
#define OTA_STATUS_DOWNLOAD_FAILED 0x22
#define OTA_STATUS_INSTALLING 0x30       // Followed by progress byte (0-100)
#define OTA_STATUS_INSTALL_COMPLETE 0x31
#define OTA_STATUS_INSTALL_FAILED 0x32
#define OTA_STATUS_REBOOTING 0x40
#define OTA_STATUS_ERROR 0xFF

// WiFi Configuration
#define WIFI_CONNECT_TIMEOUT_MS 15000    // 15 seconds to connect
#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASS_LEN 64
#define OTA_MAX_URL_LEN 256

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
// Power Button and LED Control
#define POWER_BUTTON_PIN 1 // Custom button (GPIO1/A0) - power on/off
#define STATUS_LED_PIN 21  // User LED (GPIO21) - status indicator

// =============================================================================
// POWER BUTTON & LED CONFIGURATION
// =============================================================================
// Button Configuration
#define BUTTON_DEBOUNCE_MS 50       // Button debounce time
#define POWER_OFF_PRESS_MS 2000     // Long press duration for power off (2 seconds)
#define BOOT_COMPLETE_DELAY_MS 3000 // LED indication during boot

// LED Status Patterns (in milliseconds)
#define LED_BOOT_BLINK_FAST 200     // Fast blink during boot
#define LED_BATTERY_LOW_BLINK 1000  // Slow blink for low battery
#define LED_SLEEP_BLINK 5000        // Very slow blink in deep sleep mode

// Deep Sleep Configuration
#define DEEP_SLEEP_BUTTON_WAKEUP 1    // Enable button wake-up from deep sleep
#define POWER_OFF_SLEEP_DELAY_MS 1000 // Delay before entering deep sleep after power off

// Power Button States
typedef enum { BUTTON_IDLE, BUTTON_PRESSED, BUTTON_LONG_PRESS, BUTTON_RELEASED } button_state_t;

// LED Status Modes
typedef enum {
    LED_OFF,
    LED_ON,
    LED_BOOT_SEQUENCE,
    LED_NORMAL_OPERATION,
    LED_LOW_BATTERY,
    LED_POWER_OFF_SEQUENCE,
    LED_SLEEP_MODE
} led_status_t;

// Device Power States
typedef enum {
    DEVICE_BOOTING,
    DEVICE_ACTIVE,
    DEVICE_POWER_SAVE,
    DEVICE_LOW_BATTERY,
    DEVICE_POWERING_OFF,
    DEVICE_SLEEP
} device_state_t;

#endif // CONFIG_H
