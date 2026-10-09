#include "app.h"

#include <BLE2902.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>

#include "config.h" // Use config.h for all configurations
#include "esp_sleep.h"
#include "mic.h"
#include "ogg_opus.h"
#include "opus_encoder.h"
#include "ota.h"
#include "recorder.h"
#include "speaker_monitor.h"
#include <FS.h>
#include <SD.h>

// Battery state
float batteryVoltage = 0.0f;
int batteryPercentage = 0;
unsigned long lastBatteryCheck = 0;

// Device power state
bool deviceActive = true;
device_state_t deviceState = DEVICE_BOOTING;

// Button and LED state
volatile bool buttonPressed = false;
unsigned long buttonPressTime = 0;
led_status_t ledMode = LED_BOOT_SEQUENCE;

// Gentle power optimization
unsigned long lastActivity = 0;
bool powerSaveMode = false;

// ---------------------------------------------------------------------------------
// BLE - Using config.h definitions
// ---------------------------------------------------------------------------------

// Device Information Service UUIDs
#define DEVICE_INFORMATION_SERVICE_UUID (uint16_t) 0x180A
#define MANUFACTURER_NAME_STRING_CHAR_UUID (uint16_t) 0x2A29
#define MODEL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A24
#define FIRMWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A26
#define HARDWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A27
#define SERIAL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A25

// Main Friend Service - using config.h UUIDs
static BLEUUID serviceUUID(OMI_SERVICE_UUID);
static BLEUUID audioDataUUID(AUDIO_DATA_UUID);
static BLEUUID audioCodecUUID(AUDIO_CODEC_UUID);
static BLEUUID voiceprintControlUUID(VOICEPRINT_CONTROL_UUID);

// OTA Service UUIDs
static BLEUUID otaServiceUUID(OTA_SERVICE_UUID);
static BLEUUID otaControlUUID(OTA_CONTROL_UUID);
static BLEUUID otaDataUUID(OTA_DATA_UUID);

// Characteristics
BLECharacteristic *batteryLevelCharacteristic;
BLECharacteristic *audioDataCharacteristic;
BLECharacteristic *audioCodecCharacteristic;
BLECharacteristic *otaControlCharacteristic;
BLECharacteristic *otaDataCharacteristic;
BLECharacteristic *voiceprintControlCharacteristic;

// Audio state
bool audioEnabled = true;
volatile bool audioSubscribed = false;
uint16_t audioPacketIndex = 0;

// State
bool connected = false;

// Audio ring buffer for encoded packets
#define AUDIO_TX_BUFFER_SIZE (AUDIO_TX_RING_BUFFER_SIZE * (OPUS_OUTPUT_MAX_BYTES + 2))
static uint8_t audio_tx_buffer[AUDIO_TX_BUFFER_SIZE];
static volatile size_t audio_tx_write_pos = 0;
static volatile size_t audio_tx_read_pos = 0;
static uint8_t audio_packet_buffer[OPUS_OUTPUT_MAX_BYTES + AUDIO_PACKET_HEADER_SIZE];

// Forward declarations
void readBatteryLevel();
void updateBatteryService();
void IRAM_ATTR buttonISR();
void handleButton();
void updateLED();
void blinkLED(int count, int delayMs);
void enterPowerSave();
void exitPowerSave();
void shutdownDevice();

// Audio forward declarations
void onMicData(int16_t *data, size_t samples);
void onMicAnalysis(int16_t *data, size_t samples);
void onMicRecord(int16_t *data, size_t samples);
void onOpusEncoded(uint8_t *data, size_t len);
void processAudioTx();
void broadcastAudioPacket(uint8_t *data, size_t len);

// -------------------------------------------------------------------------
// Button ISR
// -------------------------------------------------------------------------
void IRAM_ATTR buttonISR()
{
    buttonPressed = true;
}

// -------------------------------------------------------------------------
// LED Functions
// -------------------------------------------------------------------------
void updateLED()
{
#if VOICEPRINT_ENABLE
    // GPIO21 与扩展板 SD 片选共用。启用录音模块后，该引脚归 SD 独占，
    // 固件不再驱动 LED。
    //
    // 关键教训：早期版本只在「SD 已挂载」时才停止驱动 GPIO21。结果在
    // 恰好「未有卡/挂载失败」的情况下，本函数会把 CS 拉低、并每 1 秒翻转一次，
    // SD 卡收不到合法的复位时序（CMD0 失败），于是永远挂不上卡、也无法重试。
    // 代价：本构建没有状态 LED。
    return;
#endif

    // GPIO21 doubles as the expansion board's SD chip select. Once the card is
    // mounted, driving this pin would corrupt SD transactions, so the status LED
    // goes dark for good - the accepted cost of using the SD slot.
    if (recorder_sd_ok()) {
        return;
    }

    unsigned long now = millis();
    static unsigned long bootStartTime = 0;
    static unsigned long powerOffStartTime = 0;

    switch (ledMode) {
    case LED_BOOT_SEQUENCE:
        if (bootStartTime == 0)
            bootStartTime = now;

        // 5 quick blinks over 1.5 seconds total (inverted logic: HIGH=OFF, LOW=ON)
        if (now - bootStartTime < 1500) {
            int blinkPhase = ((now - bootStartTime) / 150) % 2;
            digitalWrite(STATUS_LED_PIN, !blinkPhase);
        } else {
            digitalWrite(STATUS_LED_PIN, HIGH); // OFF
            ledMode = LED_NORMAL_OPERATION;
            bootStartTime = 0;
        }
        break;

    case LED_POWER_OFF_SEQUENCE:
        if (powerOffStartTime == 0)
            powerOffStartTime = now;

        // 2 quick blinks over 800ms total (inverted logic: HIGH=OFF, LOW=ON)
        if (now - powerOffStartTime < 800) {
            int blinkPhase = ((now - powerOffStartTime) / 200) % 2;
            digitalWrite(STATUS_LED_PIN, !blinkPhase);
        } else {
            digitalWrite(STATUS_LED_PIN, HIGH); // OFF
            delay(100);
            shutdownDevice();
        }
        break;

    case LED_NORMAL_OPERATION:
    default:
        if (connected) {
            // Connected - LED solid ON
            digitalWrite(STATUS_LED_PIN, LOW);
        } else {
            // Disconnected - LED slow blink (1 sec on, 1 sec off)
            int blinkPhase = (now / 1000) % 2;
            digitalWrite(STATUS_LED_PIN, blinkPhase ? HIGH : LOW);
        }
        break;
    }
}

void blinkLED(int count, int delayMs)
{
    for (int i = 0; i < count; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(delayMs);
        digitalWrite(STATUS_LED_PIN, LOW);
        delay(delayMs);
    }
}

// -------------------------------------------------------------------------
// Button Handling
// -------------------------------------------------------------------------
void handleButton()
{
    unsigned long now = millis();
    static unsigned long lastDebounceTime = 0;
    static bool buttonDown = false;
    static bool longPressTriggered = false;

    bool currentButtonState = !digitalRead(POWER_BUTTON_PIN); // Active low (pressed = true)

    if (currentButtonState && !buttonDown) {
        // Button just pressed - debounce
        if (now - lastDebounceTime < 50) {
            return;
        }
        buttonPressTime = now;
        buttonDown = true;
        longPressTriggered = false;
        lastDebounceTime = now;

    } else if (currentButtonState && buttonDown && !longPressTriggered) {
        // Button still held - check for long press
        unsigned long pressDuration = now - buttonPressTime;
        if (pressDuration >= 2000) {
            // Long press threshold reached - trigger power off immediately
            longPressTriggered = true;
            ledMode = LED_POWER_OFF_SEQUENCE;
        }

    } else if (!currentButtonState && buttonDown) {
        // Button just released - debounce
        if (now - lastDebounceTime < 50) {
            return;
        }
        buttonDown = false;
        unsigned long pressDuration = now - buttonPressTime;
        lastDebounceTime = now;

        // Only handle short press if long press wasn't already triggered
        if (!longPressTriggered && pressDuration >= 50) {
            // Short press - register activity
            lastActivity = now;
            if (powerSaveMode) {
                exitPowerSave();
            }
        }
        longPressTriggered = false;
    }

    buttonPressed = false;
}

// -------------------------------------------------------------------------
// Power Management
// -------------------------------------------------------------------------
void enterPowerSave()
{
    if (!powerSaveMode) {
        setCpuFrequencyMhz(MIN_CPU_FREQ_MHZ); // 40MHz for idle
        powerSaveMode = true;
    }
}

void exitPowerSave()
{
    if (powerSaveMode) {
        setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ); // Back to 80MHz
        powerSaveMode = false;
    }
}

void shutdownDevice()
{
    Serial.println("Shutting down device...");

    // Stop audio
    mic_stop();

    // Disconnect BLE gracefully
    if (connected) {
        Serial.println("Disconnecting BLE...");
    }

    // Turn off LED (inverted logic)
    digitalWrite(STATUS_LED_PIN, HIGH);

    // Enter deep sleep
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0); // Wake on button press
    Serial.println("Entering deep sleep...");
    delay(100);
    esp_deep_sleep_start();
}

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

// -------------------------------------------------------------------------
// 0x20 录音回传状态机
//
// 把 SD 上的 .opus 逐帧解出来，按现有实时音频包格式推给手机：
//   [2B 序号][1B 子序号=0][6B BCD 北京时间(YY MM DD HH MM SS)][opus 数据]
// 同秒内的多帧靠递增的 2 字节序号区分顺序。
//
// 按 granule 差值节流（非阻塞），还原原始时间轴——被动录音预缓冲里被删掉
// 的静音段，回传时表现为对应的等待间隔。回传期间实时音频推送暂停，
// 避免两路包在同一个特征上混淆。
// -------------------------------------------------------------------------
// 默认「暂停」：手机订阅音频通知（写 CCCD）时自动切到 LIVE，
// 见 AudioCCCDCallback::onWrite()。0x31 可手动暂停，0x30 手动恢复。
static volatile ble_audio_tx_state_t bleAudioTxState = BLE_AUDIO_TX_PAUSED; // 当前蓝牙推送状态（开机默认暂停）
static ble_audio_tx_state_t replayPrevState = BLE_AUDIO_TX_PAUSED;          // 进入回放前的状态，回放结束后恢复
static char replayFiles[REPLAY_MAX_FILES][64];    // 按时间升序的文件名列表
static int replayFileCount = 0;
static int replayFileIdx = 0;
static char replayCurName[64];                    // 当前正在回传的文件名
static File replayFile;
static OggOpusReader replayReader;
static bool replayReaderOpen = false;
static int64_t replayLastGranule = -1;            // 上一帧 granule（节流基准）
static uint32_t replayNextSendMs = 0;             // 下一帧允许发送的时刻
static uint16_t replaySeq = 0;                    // 回传帧序号（同秒内区分顺序）
static uint32_t replayFrameCount = 0;             // 已回传帧数（日志用）

static void openReplayFileCleanup()
{
    if (replayReaderOpen) {
        ogg_opus_close_read(&replayReader);
        replayReaderOpen = false;
    }
    if (replayFile) {
        replayFile.close();
    }
}

// 打开列表中的下一个文件；全部打开过则返回 false。
static bool openNextReplayFile()
{
    openReplayFileCleanup();
    while (replayFileIdx < replayFileCount) {
        char path[72];
        snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR,
                 replayFiles[replayFileIdx]);
        replayFile = SD.open(path, FILE_READ);
        strcpy(replayCurName, replayFiles[replayFileIdx]);
        replayFileIdx++;
        if (!replayFile) {
            Serial.printf("【回传】无法打开 %s，跳过\n", path);
            continue;
        }
        if (!ogg_opus_open_read(&replayReader)) {
            Serial.printf("【回传】%s 不是有效的 .opus，跳过\n", path);
            replayFile.close();
            continue;
        }
        replayReaderOpen = true;
        replayLastGranule = -1;   // 换文件重置节流基准
        Serial.printf("【回传】正在回传 %s\n", path);
        return true;
    }
    return false;
}

// 蓝牙推送状态 → 中文名（仅用于串口日志，仿 voiceprintCmdName）。
// 入参 s：要查询的状态（ble_audio_tx_state_t，定义在 config.h）。
// 返回：静态字符串，调用方不得释放。
static const char *bleAudioTxStateName(ble_audio_tx_state_t s)
{
    switch (s) {                                          // 按状态枚举分派
    case BLE_AUDIO_TX_PAUSED: return "暂停";               // 不推送
    case BLE_AUDIO_TX_LIVE:   return "实时发送";           // 持续推送
    case BLE_AUDIO_TX_REPLAY: return "回放中";             // 回传抢占
    default:                  return "未知";               // 兜底
    }
}

// 统一的推送状态切换出口：写全局状态（bleAudioTxState，定义在本文件 321 行附近）
// 并打印一行日志。所有状态变更都应经此函数，避免漏打印/漏改。
// 入参 s：目标状态（ble_audio_tx_state_t，定义在 config.h）。无返回值。
static void bleAudioTxSetState(ble_audio_tx_state_t s)
{
    if (bleAudioTxState == s) {                           // 状态未变化则不重复打印
        return;
    }
    bleAudioTxState = s;                                  // 更新全局状态
    Serial.printf("【音频】推送状态 → %s\n", bleAudioTxStateName(s)); // 串口回显
}

static void startReplay()
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 已在回放则忽略重复请求
        Serial.println("【回传】已在回传中");
        return;
    }
    replayFileCount = recorder_collect_files(replayFiles, REPLAY_MAX_FILES);
    if (replayFileCount == 0) {
        Serial.println("【回传】SD 上没有录音");
        return;
    }
    replayFileIdx = 0;
    replaySeq = 0;
    replayFrameCount = 0;
    replayNextSendMs = 0;
    replayPrevState = bleAudioTxState;                    // 记住回放前的状态，回放结束恢复
    bleAudioTxSetState(BLE_AUDIO_TX_REPLAY);              // 切入回放态（抢占实时发送）
    Serial.printf("【回传】开始回传 %d 条录音\n", replayFileCount);
}

static void stopReplay()
{
    if (bleAudioTxState != BLE_AUDIO_TX_REPLAY) {         // 未在回放则无需处理
        return;
    }
    openReplayFileCleanup();                              // 关闭当前回传文件/解复用器
    bleAudioTxSetState(replayPrevState);                  // 恢复到进入回放前的状态
    Serial.printf("【回传】已停止（共回传 %lu 帧）\n",
                  (unsigned long)replayFrameCount);
}

// 0x30：请求切到「实时发送」。若正在回放，先停回放（实时优先）。
// 无入参、无返回值；供 voiceprintHandleCommand() 的命令分支调用。
static void bleAudioTxRequestLive()
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 回放中：先收尾回放
        stopReplay();
    }
    bleAudioTxSetState(BLE_AUDIO_TX_LIVE);                // 切到实时发送（幂等）
}

// 0x31：请求「暂停推送」。若正在回放，先停回放。
// 无入参、无返回值；供 voiceprintHandleCommand() 的命令分支调用。
static void bleAudioTxRequestPause()
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 回放中：先收尾回放
        stopReplay();
    }
    bleAudioTxSetState(BLE_AUDIO_TX_PAUSED);              // 切到暂停（幂等）
}

// 回传一帧（由 loop_app 在回传期间反复调用，非阻塞）。
static void pumpReplay()
{
    // 手机断开或未订阅时暂停推送（数据不丢弃，恢复后继续）。
    if (!connected || !audioSubscribed || audioDataCharacteristic == nullptr) {
        return;
    }

    if (!replayReaderOpen && !openNextReplayFile()) {
        Serial.printf("【回传】回传完成（共 %lu 帧）\n",
                      (unsigned long)replayFrameCount);
        bleAudioTxSetState(replayPrevState);              // 回放自然结束，恢复回放前的状态
        return;
    }

    uint8_t opus_buf[OPUS_OUTPUT_MAX_BYTES];
    size_t len = 0;
    int64_t granule = 0;
    if (!ogg_opus_read_packet(&replayReader, replayFile, opus_buf, sizeof(opus_buf),
                              &len, &granule)) {
        // 当前文件读完，下一个由下一轮 pump 打开。
        openReplayFileCleanup();
        return;
    }

    // granule 差值节流：granule 单位是 48kHz 刻度，/48 即毫秒。
    // 非阻塞写法——绝不在 loop 里 delay，避免饿死 mic/Opus。
    if (replayLastGranule >= 0) {
        int64_t d = (granule - replayLastGranule) / 48;
        if (d > REPLAY_MAX_FRAME_DELAY_MS) {
            d = REPLAY_MAX_FRAME_DELAY_MS;
        }
        if (d < REPLAY_MIN_FRAME_DELAY_MS) {
            d = REPLAY_MIN_FRAME_DELAY_MS;
        }
        uint32_t now = millis();
        if ((int32_t)(now - replayNextSendMs) < 0) {
            return;   // 还没到发送时刻，帧暂存等待下轮
        }
        replayNextSendMs = now + (uint32_t)d;
    }
    replayLastGranule = granule;

    // 组包：[2B 序号][1B 子序号][6B BCD 北京时间][opus]
    uint8_t bcd[6];
    recorder_frame_time(replayCurName, granule, bcd);

    uint8_t pkt[REPLAY_PACKET_MAX_BYTES];
    pkt[0] = replaySeq & 0xFF;
    pkt[1] = (replaySeq >> 8) & 0xFF;
    pkt[2] = 0;   // 子序号（实时链路留给分片用，回传固定 0）
    memcpy(pkt + AUDIO_PACKET_HEADER_SIZE, bcd, 6);
    memcpy(pkt + AUDIO_PACKET_HEADER_SIZE + 6, opus_buf, len);

    audioDataCharacteristic->setValue(pkt, AUDIO_PACKET_HEADER_SIZE + 6 + len);
    audioDataCharacteristic->notify();
    replaySeq++;
    replayFrameCount++;
}

// -------------------------------------------------------------------------
// Voiceprint enrollment control (shared by the serial and BLE triggers)
// -------------------------------------------------------------------------
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

static void voiceprintHandleCommand(uint8_t cmd)
{
    switch (cmd) {
    case VP_CMD_ENROLL:
        if (!speaker_monitor_enroll_start()) {
            Serial.println("【命令】无法开始录入（正在录入，或声纹模块未启用）");
        }
        break;
    case VP_CMD_ABORT:
        speaker_monitor_enroll_abort();
        Serial.println("【命令】已放弃录入");
        break;
    case VP_CMD_FINISH:
        // 结果行由 voiceprint_enroll_finish() 自己打印。
        if (!speaker_monitor_enroll_finish_now()) {
            Serial.println("【命令】当前不在录入，无需结束");
        }
        break;
    case VP_CMD_ERASE:
        speaker_monitor_erase_template();
        break;
    case VP_CMD_STATUS:
        Serial.printf("【命令】状态=%s 模板=%s 已录取段数=%d 音频推送=%s\n",
                      speaker_monitor_state_name(),
                      speaker_monitor_has_template() ? "有" : "无",
                      speaker_monitor_enroll_segments(),
                      bleAudioTxStateName(bleAudioTxState));   // 追加当前蓝牙推送状态
        break;
    case VP_CMD_REC_START:
        // 主动录音优先级更高：先停掉被动录音，再开始连续录制。
        speaker_monitor_force_idle();
        if (!recorder_active_start()) {
            Serial.println("【命令】主动录音启动失败（无SD卡？）");
        }
        break;
    case VP_CMD_REC_STOP:
        recorder_active_stop();
        break;
    case VP_CMD_DEL_LAST:
        // 先保证状态机回待机（被动录音落盘），再删文件。
        speaker_monitor_force_idle();
        recorder_delete_last_active();
        break;
    case VP_CMD_DEL_ALL:
        speaker_monitor_force_idle();
        recorder_delete_all();
        break;
    case VP_CMD_REPLAY:
        startReplay();
        break;
    case VP_CMD_REPLAY_STOP:
        stopReplay();
        break;
    case VP_CMD_AUDIO_SEND_START:
        bleAudioTxRequestLive();                          // 0x30：开始/恢复实时推送
        break;
    case VP_CMD_AUDIO_SEND_PAUSE:
        bleAudioTxRequestPause();                         // 0x31：暂停实时推送
        break;
    case VP_CMD_SET_TIME:
        // 0x21 的负载（4 字节 unix）由 pollVoiceprintCommands() 解析，
        // 不会走到这里。
        Serial.println("【命令】对时命令缺少时间负载");
        break;
    default:
        Serial.printf("【命令】未知命令 0x%02X\n", cmd);
        break;
    }
}

// BLE writes arrive on the Bluedroid task, which must not drive the monitor's
// shared window buffer. Queue the command and let the main loop run it.
// 0x21（对时）需要带 4 字节 unix 负载，所以把整个 value（≤5 字节）一起存下来。
static volatile uint8_t voiceprintPendingPayload[5];
static volatile size_t voiceprintPendingLen = 0;
static volatile uint8_t voiceprintPendingCmd = 0;

// 串口触发：一行一条命令 - enroll / abort / finish / erase / status /
// recstart / recstop。
static void pollVoiceprintCommands()
{
    static char line[32];
    static size_t len = 0;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue;
            }
            line[len] = '\0';
            len = 0;

            if (!strcasecmp(line, "enroll")) {
                voiceprintHandleCommand(VP_CMD_ENROLL);
            } else if (!strcasecmp(line, "abort")) {
                voiceprintHandleCommand(VP_CMD_ABORT);
            } else if (!strcasecmp(line, "finish")) {
                voiceprintHandleCommand(VP_CMD_FINISH);
            } else if (!strcasecmp(line, "erase")) {
                voiceprintHandleCommand(VP_CMD_ERASE);
            } else if (!strcasecmp(line, "status")) {
                voiceprintHandleCommand(VP_CMD_STATUS);
            } else if (!strcasecmp(line, "recstart")) {
                voiceprintHandleCommand(VP_CMD_REC_START);
            } else if (!strcasecmp(line, "recstop")) {
                voiceprintHandleCommand(VP_CMD_REC_STOP);
            } else if (!strcasecmp(line, "dellast")) {
                voiceprintHandleCommand(VP_CMD_DEL_LAST);
            } else if (!strcasecmp(line, "delall")) {
                voiceprintHandleCommand(VP_CMD_DEL_ALL);
            } else if (!strcasecmp(line, "replay")) {
                voiceprintHandleCommand(VP_CMD_REPLAY);
            } else if (!strcasecmp(line, "replaystop")) {
                voiceprintHandleCommand(VP_CMD_REPLAY_STOP);
            } else if (!strcasecmp(line, "sendstart")) {
                voiceprintHandleCommand(VP_CMD_AUDIO_SEND_START);   // 串口别名：开始推送
            } else if (!strcasecmp(line, "sendstop")) {
                voiceprintHandleCommand(VP_CMD_AUDIO_SEND_PAUSE);   // 串口别名：暂停推送
            } else if (!strcasecmp(line, "sdcheck")) {
                recorder_sd_check();
            } else if (!strcasecmp(line, "sdprobe")) {
                recorder_sd_probe();
            } else if (!strcasecmp(line, "sdpins")) {
                recorder_sd_pins();
            } else if (!strcasecmp(line, "sdmmc")) {
                recorder_sd_mmc();
            } else if (!strncasecmp(line, "settime", 7)) {
                // 串口对时：settime <unix 秒>
                uint32_t unix = (uint32_t)atol(line + 7);
                recorder_sync_time(unix);
            } else {
                Serial.printf("【命令】无法识别的命令 '%s'\n", line);
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }

    uint8_t pending = voiceprintPendingCmd;
    if (pending != 0) {
        voiceprintPendingCmd = 0;
        Serial.printf("【命令】手机写入 0x%02X（%s）\n", pending,
                      voiceprintCmdName(pending));
        if (pending == VP_CMD_SET_TIME && voiceprintPendingLen >= 5) {
            // 0x21 负载 = 4 字节小端 unix 时间戳（秒）。
            uint32_t unix = (uint32_t)voiceprintPendingPayload[1]
                          | ((uint32_t)voiceprintPendingPayload[2] << 8)
                          | ((uint32_t)voiceprintPendingPayload[3] << 16)
                          | ((uint32_t)voiceprintPendingPayload[4] << 24);
            recorder_sync_time(unix);
        } else {
            voiceprintHandleCommand(pending);
        }
    }
}

class VoiceprintControlCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        size_t len = pCharacteristic->getLength();
        if (len < 1) {
            return;
        }
        // 先写负载字节，最后写命令字节：poll 一看到 cmd 非零，负载必已就位。
        std::string val = pCharacteristic->getValue();
        size_t n = (val.length() > sizeof(voiceprintPendingPayload))
                       ? sizeof(voiceprintPendingPayload) : val.length();
        const uint8_t *v = (const uint8_t *)val.data();
        for (size_t i = 0; i < n; i++) {
            voiceprintPendingPayload[i] = v[i];
        }
        voiceprintPendingLen = n;
        voiceprintPendingCmd = voiceprintPendingPayload[0];
    }
};

void onOpusEncoded(uint8_t *data, size_t len)
{
    // Store encoded data in TX ring buffer
    if (len > OPUS_OUTPUT_MAX_BYTES) {
        return;
    }

    // Write length (2 bytes) + data
    size_t packet_size = len + 2;
    size_t next_write = (audio_tx_write_pos + packet_size) % AUDIO_TX_BUFFER_SIZE;

    // Check for buffer overflow
    if ((audio_tx_write_pos < audio_tx_read_pos && next_write >= audio_tx_read_pos) ||
        (audio_tx_write_pos >= audio_tx_read_pos && next_write < audio_tx_write_pos &&
         next_write >= audio_tx_read_pos)) {
        // Buffer full, skip this packet
        return;
    }

    // Write length
    audio_tx_buffer[audio_tx_write_pos] = len & 0xFF;
    audio_tx_buffer[(audio_tx_write_pos + 1) % AUDIO_TX_BUFFER_SIZE] = (len >> 8) & 0xFF;

    // Write data
    for (size_t i = 0; i < len; i++) {
        audio_tx_buffer[(audio_tx_write_pos + 2 + i) % AUDIO_TX_BUFFER_SIZE] = data[i];
    }

    audio_tx_write_pos = next_write;
}

void broadcastAudioPacket(uint8_t *data, size_t len)
{
    if (!connected || !audioSubscribed || audioDataCharacteristic == nullptr) {
        return;
    }

    // Build packet: 2 bytes index + 1 byte sub-index + data
    audio_packet_buffer[0] = audioPacketIndex & 0xFF;
    audio_packet_buffer[1] = (audioPacketIndex >> 8) & 0xFF;
    audio_packet_buffer[2] = 0; // Sub-index (for fragmentation if needed)

    memcpy(audio_packet_buffer + AUDIO_PACKET_HEADER_SIZE, data, len);

    audioDataCharacteristic->setValue(audio_packet_buffer, len + AUDIO_PACKET_HEADER_SIZE);
    audioDataCharacteristic->notify();
    audioPacketIndex++;
}

void processAudioTx()
{
    if (!connected || !audioSubscribed) {
        return;
    }

    if (audioDataCharacteristic == nullptr) {
        return;
    }

    // 只有「实时发送」态才真正推给手机；暂停/回放态只排空缓冲、不发送。
    // 这样暂停期不会积压陈旧音频，恢复后从当下开始，无突发补发。
    const bool send = (bleAudioTxState == BLE_AUDIO_TX_LIVE);   // 本轮是否真正推送

    // Check if we have data in the ring buffer
    while (audio_tx_read_pos != audio_tx_write_pos) {
        // Read length
        uint16_t len =
            audio_tx_buffer[audio_tx_read_pos] | (audio_tx_buffer[(audio_tx_read_pos + 1) % AUDIO_TX_BUFFER_SIZE] << 8);

        if (len == 0 || len > OPUS_OUTPUT_MAX_BYTES) {
            // Invalid packet, skip
            audio_tx_read_pos = (audio_tx_read_pos + 2) % AUDIO_TX_BUFFER_SIZE;
            continue;
        }

        // Update read position（无论推不推送都推进读指针 → 暂停期的帧被丢弃）
        size_t saved_read = audio_tx_read_pos;                  // 暂存本帧起始，供拷贝用
        audio_tx_read_pos = (audio_tx_read_pos + 2 + len) % AUDIO_TX_BUFFER_SIZE;

        if (send) {
            // Read data
            static uint8_t temp_data[OPUS_OUTPUT_MAX_BYTES];    // 单帧临时缓冲（静态，避免占栈）
            for (size_t i = 0; i < len; i++) {                  // 逐个字节拷出（环形，需取模）
                temp_data[i] = audio_tx_buffer[(saved_read + 2 + i) % AUDIO_TX_BUFFER_SIZE];
            }

            // Send packet
            broadcastAudioPacket(temp_data, len);               // 真正 notify 给手机

            // Small delay to prevent BLE congestion
            delay(1);                                           // 仅在推送时限速，防 BLE 拥塞
        }
    }
}

// -------------------------------------------------------------------------
// BLE Callbacks
// -------------------------------------------------------------------------
class ServerHandler : public BLEServerCallbacks
{
    void onConnect(BLEServer *server) override
    {
        connected = true;
        audioSubscribed = false;
        lastActivity = millis(); // Register activity - prevents sleep
        Serial.println(">>> BLE Client connected.");
        // Send current battery level on connect
        updateBatteryService();
    }
    void onMtuChanged(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
    {
        Serial.printf(">>> MTU negotiated: %d\n", param->mtu.mtu);
    }
    void onDisconnect(BLEServer *server) override
    {
        connected = false;
        audioSubscribed = false;
        Serial.println("<<< BLE Client disconnected. Restarting advertising.");
        BLEDevice::startAdvertising();
    }
};

// Callback for Audio Data CCCD (Client Characteristic Configuration Descriptor)
class AudioCCCDCallback : public BLEDescriptorCallbacks
{
    void onWrite(BLEDescriptor *pDescriptor)
    {
        uint8_t *value = pDescriptor->getValue();
        if (value && pDescriptor->getLength() >= 2) {
            // Check notification bit (bit 0)
            if (value[0] & 0x01) {
                audioSubscribed = true;
                Serial.println("Audio notifications enabled");
                // 订阅即开始：手机开启音频通知 = 明确「要音频」，自动切到实时发送。
                // 若正在回放（REPLAY）则不打断，由回放结束后的状态恢复逻辑接管。
                if (bleAudioTxState == BLE_AUDIO_TX_PAUSED) {   // 仅从暂停态自动开始
                    bleAudioTxSetState(BLE_AUDIO_TX_LIVE);      // 切到实时发送
                }
            } else {
                audioSubscribed = false;
                Serial.println("Audio notifications disabled");
            }
        }
    }
};

class AudioDataCallback : public BLECharacteristicCallbacks
{
    void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code)
    {
        if (s == Status::SUCCESS_NOTIFY || s == Status::SUCCESS_INDICATE) {
            // Notification sent successfully
        }
    }

    void onRead(BLECharacteristic *pCharacteristic)
    {
        // Client read the characteristic
    }
};

class OTAControlCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pChar) override
    {
        std::string value = pChar->getValue();
        if (value.length() > 0) {
            ota_handle_command((uint8_t *) value.data(), value.length());
        }
    }

    void onRead(BLECharacteristic *pChar) override
    {
        uint8_t status[2] = {ota_get_status(), 0};
        pChar->setValue(status, 2);
    }
};

// -------------------------------------------------------------------------
// Battery Functions
// -------------------------------------------------------------------------
void readBatteryLevel()
{
    // Take multiple ADC readings for stability
    int adcSum = 0;
    for (int i = 0; i < 10; i++) {
        int value = analogRead(BATTERY_ADC_PIN);
        adcSum += value;
        delay(10);
    }
    int adcValue = adcSum / 10;

    // ESP32-S3 ADC: 12-bit (0-4095), reference voltage ~3.3V
    float adcVoltage = (adcValue / 4095.0f) * 3.3f;

    // Apply voltage divider ratio to get actual battery voltage
    batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO;

    // Clamp voltage to reasonable range
    if (batteryVoltage > 5.0f)
        batteryVoltage = 5.0f;
    if (batteryVoltage < 2.5f)
        batteryVoltage = 2.5f;

    // Load-compensated battery calculation (accounts for voltage sag under load)
    float loadCompensatedMax = BATTERY_MAX_VOLTAGE;
    float loadCompensatedMin = BATTERY_MIN_VOLTAGE;

    // More accurate percentage calculation for load conditions
    if (batteryVoltage >= loadCompensatedMax) {
        batteryPercentage = 100;
    } else if (batteryVoltage <= loadCompensatedMin) {
        batteryPercentage = 0;
    } else {
        float range = loadCompensatedMax - loadCompensatedMin;
        batteryPercentage = (int) (((batteryVoltage - loadCompensatedMin) / range) * 100.0f);
    }

    // Smooth percentage changes to avoid jumpy readings
    static int lastBatteryPercentage = batteryPercentage;
    if (abs(batteryPercentage - lastBatteryPercentage) > 5) {
        batteryPercentage = lastBatteryPercentage + (batteryPercentage > lastBatteryPercentage ? 2 : -2);
    }
    lastBatteryPercentage = batteryPercentage;

    // Clamp percentage
    if (batteryPercentage > 100)
        batteryPercentage = 100;
    if (batteryPercentage < 0)
        batteryPercentage = 0;

    // Battery status with load info
    Serial.print("Battery: ");
    Serial.print(batteryVoltage);
    Serial.print("V (");
    Serial.print(batteryPercentage);
    Serial.print("%) [Load-compensated: ");
    Serial.print(loadCompensatedMin);
    Serial.print("V-");
    Serial.print(loadCompensatedMax);
    Serial.println("V]");
}

void updateBatteryService()
{
    if (batteryLevelCharacteristic) {
        uint8_t batteryLevel = (uint8_t) batteryPercentage;
        batteryLevelCharacteristic->setValue(&batteryLevel, 1);

        if (connected) {
            batteryLevelCharacteristic->notify();
        }
    }
}

// -------------------------------------------------------------------------
// configure_ble()
// -------------------------------------------------------------------------
void configure_ble()
{
    Serial.println("Initializing BLE...");
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(BLE_MTU_SIZE); // Set local MTU (BLE_MTU_SIZE from config.h)
    Serial.printf("Local MTU set to %d\n", BLEDevice::getMTU());
    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(new ServerHandler());

    // Main service
    BLEService *service = server->createService(serviceUUID);

    // Audio Data characteristic (for streaming audio to app)
    audioDataCharacteristic = service->createCharacteristic(
        audioDataUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *audioCcc = new BLE2902();
    audioCcc->setNotifications(true);
    audioCcc->setCallbacks(new AudioCCCDCallback());
    audioDataCharacteristic->addDescriptor(audioCcc);
    audioDataCharacteristic->setCallbacks(new AudioDataCallback());

    // Audio Codec characteristic (tells app which codec we're using)
    audioCodecCharacteristic = service->createCharacteristic(audioCodecUUID, BLECharacteristic::PROPERTY_READ);
    uint8_t codecId = opus_get_codec_id();
    audioCodecCharacteristic->setValue(&codecId, 1);

    // Voiceprint control characteristic: 1-byte commands (enroll/abort/finish/erase/status)
    // 同时支持「有响应写」与「无响应写」：不少客户端/调试工具默认用 Write Without Response，
    // 若只声明 PROPERTY_WRITE，该写入会在 GATT 层被直接拒收，onWrite() 不会被调用，
    // 表现为「发了命令没反应、也不报错」。
    voiceprintControlCharacteristic = service->createCharacteristic(
        voiceprintControlUUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    voiceprintControlCharacteristic->setCallbacks(new VoiceprintControlCallback());

    // Battery Service
    BLEService *batteryService = server->createService(BATTERY_SERVICE_UUID);
    batteryLevelCharacteristic = batteryService->createCharacteristic(
        BATTERY_LEVEL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *batteryCcc = new BLE2902();
    batteryCcc->setNotifications(true);
    batteryLevelCharacteristic->addDescriptor(batteryCcc);

    // Set initial battery level
    readBatteryLevel();
    uint8_t initialBatteryLevel = (uint8_t) batteryPercentage;
    batteryLevelCharacteristic->setValue(&initialBatteryLevel, 1);

    // Device Information Service
    BLEService *deviceInfoService = server->createService(DEVICE_INFORMATION_SERVICE_UUID);
    BLECharacteristic *manufacturerNameCharacteristic =
        deviceInfoService->createCharacteristic(MANUFACTURER_NAME_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    BLECharacteristic *modelNumberCharacteristic =
        deviceInfoService->createCharacteristic(MODEL_NUMBER_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    BLECharacteristic *firmwareRevisionCharacteristic =
        deviceInfoService->createCharacteristic(FIRMWARE_REVISION_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    BLECharacteristic *hardwareRevisionCharacteristic =
        deviceInfoService->createCharacteristic(HARDWARE_REVISION_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    BLECharacteristic *serialNumberCharacteristic =
        deviceInfoService->createCharacteristic(SERIAL_NUMBER_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ);

    manufacturerNameCharacteristic->setValue(MANUFACTURER_NAME);
    modelNumberCharacteristic->setValue(BLE_DEVICE_NAME);
    firmwareRevisionCharacteristic->setValue(FIRMWARE_VERSION_STRING);
    hardwareRevisionCharacteristic->setValue(HARDWARE_REVISION);

    // Generate serial number from ESP32 chip ID
    uint64_t chipId = ESP.getEfuseMac();
    char serialNumber[17];
    snprintf(serialNumber, sizeof(serialNumber), "%04X%08X", (uint16_t) (chipId >> 32), (uint32_t) chipId);
    serialNumberCharacteristic->setValue(serialNumber);

    // OTA Service
    BLEService *otaService = server->createService(otaServiceUUID);

    // OTA Control characteristic (for receiving commands and reading status)
    otaControlCharacteristic = otaService->createCharacteristic(
        otaControlUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    otaControlCharacteristic->setCallbacks(new OTAControlCallback());

    // OTA Data characteristic (for progress notifications)
    otaDataCharacteristic = otaService->createCharacteristic(
        otaDataUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *otaCcc = new BLE2902();
    otaCcc->setNotifications(true);
    otaDataCharacteristic->addDescriptor(otaCcc);

    // Set OTA characteristics for the OTA module
    ota_set_characteristics(otaControlCharacteristic, otaDataCharacteristic);

    // Start services
    service->start();
    batteryService->start();
    deviceInfoService->start();
    otaService->start();

    // Start advertising
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(service->getUUID()); // Main service (fits in 31 bytes)
    advertising->setScanResponse(true);
    advertising->setMinPreferred(BLE_ADV_MIN_INTERVAL);
    advertising->setMaxPreferred(BLE_ADV_MAX_INTERVAL);
    BLEDevice::startAdvertising();

    Serial.println("BLE initialized and advertising started.");
}

// -------------------------------------------------------------------------
// Setup & Loop
// -------------------------------------------------------------------------

void setup_app()
{
    Serial.begin(115200);
    Serial.println("Setup started...");

    // Initialize GPIO
    pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED_PIN, OUTPUT);

    // LED uses inverted logic: HIGH = OFF, LOW = ON
    digitalWrite(STATUS_LED_PIN, HIGH);

    // Setup button interrupt
    attachInterrupt(digitalPinToInterrupt(POWER_BUTTON_PIN), buttonISR, CHANGE);

    // Start LED boot sequence
    ledMode = LED_BOOT_SEQUENCE;

    // Power optimization from config.h
    setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ);
    lastActivity = millis();

    configure_ble();

    // Initial battery reading
    // Battery voltage divider
    analogReadResolution(12);                           // optional: set 12-bit resolution
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db); // set attenuation for full 3.3V range

    readBatteryLevel();
    deviceState = DEVICE_ACTIVE;

    // Initialize audio subsystem
    Serial.println("Initializing audio subsystem...");

    // 开机初始推送状态：默认「暂停」，手机订阅音频通知后自动开始推送。
    Serial.printf("【音频】推送状态 → %s（手机订阅音频特征后自动开始；0x31 暂停，0x30 恢复）\n",
                  bleAudioTxStateName(bleAudioTxState));

    if (opus_encoder_init()) {
        opus_set_callback(onOpusEncoded);

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

    // Handle button presses
    handleButton();

    // Update LED
    updateLED();

    // Process OTA updates
    ota_loop();

    // Voiceprint enrollment commands from the serial console or BLE
    pollVoiceprintCommands();

    // Process microphone data - always run to keep audio realtime
    if (audioEnabled && mic_is_running()) {
        mic_process();
        opus_process();
    }

    // Send audio packets over BLE. During replay the live stream pauses so
    // the replayed frames (with timestamps) don't interleave with live ones.
    // 三态状态机：回放走 pumpReplay；实时/暂停都走 processAudioTx（由状态决定是否真发）。
    if (connected && audioSubscribed) {
        if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {       // 回放态：推回传帧
            pumpReplay();
        } else {
            processAudioTx();                               // 实时态=发送，暂停态=只排空缓冲
        }
    }

    // Check for power save mode (gentle optimization)
    if (!connected && (now - lastActivity > IDLE_THRESHOLD_MS)) {
        enterPowerSave();
    } else if (connected) {
        if (powerSaveMode)
            exitPowerSave();
        lastActivity = now;
    }

    // Check battery level periodically
    if (now - lastBatteryCheck >= BATTERY_TASK_INTERVAL_MS) {
        readBatteryLevel();
        updateBatteryService();
        lastBatteryCheck = now;
    }

    // Force battery update on first connection
    static bool firstBatteryUpdate = true;
    if (connected && firstBatteryUpdate) {
        readBatteryLevel();
        updateBatteryService();
        firstBatteryUpdate = false;
    }

    // Adaptive delays for power saving (gentle optimization)
    if (audioSubscribed) {
        delay(5); // Fast during audio streaming
    } else {
        delay(50); // Reduced delay
    }
}
