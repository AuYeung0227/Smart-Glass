#include "audio_tx.h"

// =============================================================================
// audio_tx.cpp —— 蓝牙音频推送模块（业务层）实现
//
// 【本文件的作用】
// 1) 维护「推送三态状态机」：暂停 / 实时发送 / 回放；
// 2) 把 Opus 编码器产出的帧存进一条环形缓冲，再按状态决定发不发；
// 3) 实现 0x20「录音回传」：从 SD 读 .opus、按 granule 差值节流还原原始时间轴、
//    逐帧推给手机。
// 这些代码原先是 src/app.cpp 的一部分（原 :78-81、:310-511、:714-743、:763-807），
// 按分层重构搬迁至此，算法逐行未改。
//
// 【本次搬迁做的唯一改动：把「发送」这件事委托出去】
// 重构前本模块的代码直接操作 BLE 特征（audioDataCharacteristic->setValue/notify），
// 属「业务层调协议层」。现在改为调用应用层注册进来的 s_sink 函数指针；
// 「BLE 是否可发」也由应用层作为参数传进 audio_tx_tick()，而不是自己去查。
//
// 【分层】业务层。不 include ble_transport.h。
// 【配套头文件】audio_tx.h（对外接口说明）
// =============================================================================

#include <Arduino.h>

#include <FS.h> // File
#include <SD.h> // SD.open —— 与 recorder.cpp 同一口径：Arduino 库 API 视为基础设施

#include "config.h"    // 状态枚举、缓冲长度、节流区间、录音目录等宏
#include "ogg_opus.h"  // OggOpusReader / ogg_opus_* 读取接口
#include "recorder.h"  // recorder_collect_files() / recorder_frame_time()

// -------------------------------------------------------------------------
// 发送环形缓冲（原 app.cpp:78-81，逐行搬迁；仅变量名加 s_ 前缀以标明是本模块静态量）
// 存放「已编码但还没发出去」的帧，格式为 [2B 长度][数据...] 连续排列。
// -------------------------------------------------------------------------
#define AUDIO_TX_BUFFER_SIZE (AUDIO_TX_RING_BUFFER_SIZE * (OPUS_OUTPUT_MAX_BYTES + 2))
static uint8_t s_ring[AUDIO_TX_BUFFER_SIZE];  // 环形缓冲本体
static volatile size_t s_ringWrite = 0;       // 写指针（原 audio_tx_write_pos）
static volatile size_t s_ringRead = 0;        // 读指针（原 audio_tx_read_pos）

// 应用层注册进来的发送出口（见 audio_tx.h 的 audio_tx_sink_fn）
static audio_tx_sink_fn s_sink = nullptr;

// 应用层每轮传入的「BLE 是否可发」，供 pumpReplay()/processAudioTx() 使用
static bool s_bleReady = false;

// -------------------------------------------------------------------------
// 推送状态与回传状态（原 app.cpp:323-335，逐行搬迁）
// -------------------------------------------------------------------------
static volatile ble_audio_tx_state_t bleAudioTxState = BLE_AUDIO_TX_PAUSED; // 当前蓝牙推送状态（开机默认暂停）
static ble_audio_tx_state_t replayPrevState = BLE_AUDIO_TX_PAUSED;          // 进入回放前的状态，回放结束后恢复
static char replayFiles[REPLAY_MAX_FILES][64];    // 按时间升序的文件名列表
static int replayFileCount = 0;                   // 列表里的文件总数
static int replayFileIdx = 0;                     // 下一个要打开的文件下标
static char replayCurName[64];                    // 当前正在回传的文件名
static File replayFile;                           // 当前正在读的文件句柄
static OggOpusReader replayReader;                // Ogg 解复用器
static bool replayReaderOpen = false;             // 解复用器是否已打开
static int64_t replayLastGranule = -1;            // 上一帧 granule（节流基准）
static uint32_t replayNextSendMs = 0;             // 下一帧允许发送的时刻
static uint16_t replaySeq = 0;                    // 回传帧序号（同秒内区分顺序）
static uint32_t replayFrameCount = 0;             // 已回传帧数（日志用）

static uint16_t s_audioPacketIndex = 0;           // 实时链路包头序号（原 app.cpp:72 的 audioPacketIndex）

// -------------------------------------------------------------------------
// 0x20 录音回传：文件打开/关闭辅助
// -------------------------------------------------------------------------

/**
 * @brief 关闭当前回传文件与解复用器（幂等，可重复调用）。
 * 入参/出参：无。
 * 引用的变量：replayReaderOpen / replayReader / replayFile（本文件上方）。
 */
static void openReplayFileCleanup()
{
    if (replayReaderOpen) {
        ogg_opus_close_read(&replayReader); // 释放 Ogg 解复用器
        replayReaderOpen = false;
    }
    if (replayFile) {
        replayFile.close();                 // 关闭文件句柄
    }
}

/**
 * @brief 打开列表中的下一个文件；全部打开过则返回 false。
 * 入参/出参：无；返回 true = 已有一个可读文件就绪。
 * 引用的变量：replayFiles / replayFileIdx / replayFileCount / replayCurName /
 *             replayReader / replayReaderOpen / replayLastGranule（本文件上方）；
 *             RECORD_SD_DIR（src/config.h）。
 */
static bool openNextReplayFile()
{
    openReplayFileCleanup(); // 先收尾上一个
    while (replayFileIdx < replayFileCount) {
        char path[72];
        snprintf(path, sizeof(path), "%s/%s", RECORD_SD_DIR,
                 replayFiles[replayFileIdx]);        // 拼出完整路径
        replayFile = SD.open(path, FILE_READ);       // 只读打开
        strcpy(replayCurName, replayFiles[replayFileIdx]); // 记下当前文件名（后面取时间戳要用）
        replayFileIdx++;                             // 下标前移
        if (!replayFile) {
            Serial.printf("【回传】无法打开 %s，跳过\n", path); // 打不开就跳过
            continue;
        }
        if (!ogg_opus_open_read(&replayReader)) {
            Serial.printf("【回传】%s 不是有效的 .opus，跳过\n", path); // 非 Ogg/Opus 文件跳过
            replayFile.close();
            continue;
        }
        replayReaderOpen = true;                     // 标记解复用器已打开
        replayLastGranule = -1;   // 换文件重置节流基准
        Serial.printf("【回传】正在回传 %s\n", path);
        return true;
    }
    return false; // 列表已遍历完
}

// -------------------------------------------------------------------------
// 推送状态机
// -------------------------------------------------------------------------

/**
 * @brief 蓝牙推送状态 → 中文名（仅用于串口日志，仿 voiceprintCmdName）。
 * 入参 s：要查询的状态（ble_audio_tx_state_t，定义在 config.h）。
 * 出参：静态字符串，调用方不得释放。
 */
static const char *bleAudioTxStateName(ble_audio_tx_state_t s)
{
    switch (s) {                                          // 按状态枚举分派
    case BLE_AUDIO_TX_PAUSED: return "暂停";               // 不推送
    case BLE_AUDIO_TX_LIVE:   return "实时发送";           // 持续推送
    case BLE_AUDIO_TX_REPLAY: return "回放中";             // 回传抢占
    default:                  return "未知";               // 兜底
    }
}

/**
 * @brief 统一的推送状态切换出口：写全局状态并打印一行日志。
 * 入参 s：目标状态（ble_audio_tx_state_t，定义在 config.h）。出参：无。
 * 引用的变量：bleAudioTxState（本文件上方）。
 */
static void bleAudioTxSetState(ble_audio_tx_state_t s)
{
    if (bleAudioTxState == s) {                           // 状态未变化则不重复打印
        return;
    }
    bleAudioTxState = s;                                  // 更新全局状态
    Serial.printf("【音频】推送状态 → %s\n", bleAudioTxStateName(s)); // 串口回显
}

/**
 * @brief 取当前推送状态的中文名（对外接口，见 audio_tx.h）。
 * 入参/出参：无；返回静态字符串。
 */
const char *audio_tx_state_name(void)
{
    return bleAudioTxStateName(bleAudioTxState); // 对外只暴露「当前状态」的名字
}

// -------------------------------------------------------------------------
// 回传的启动 / 停止 / 请求（供 cmd_router 调用）
// -------------------------------------------------------------------------

/**
 * @brief 开始 0x20 录音回传（原 startReplay()，:401-419）。
 * 入参/出参：无。
 * 引用的变量：replay* 系列、bleAudioTxState（本文件上方）；REPLAY_MAX_FILES（config.h）；
 *             recorder_collect_files()（src/recorder.h）。
 */
void audio_tx_start_replay(void)
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 已在回放则忽略重复请求
        Serial.println("【回传】已在回传中");
        return;
    }
    replayFileCount = recorder_collect_files(replayFiles, REPLAY_MAX_FILES); // 收集 SD 上的录音
    if (replayFileCount == 0) {
        Serial.println("【回传】SD 上没有录音");            // 没录音直接返回
        return;
    }
    replayFileIdx = 0;                                    // 从头开始
    replaySeq = 0;                                        // 序号清零
    replayFrameCount = 0;                                 // 计数清零
    replayNextSendMs = 0;                                 // 节流基准清零
    replayPrevState = bleAudioTxState;                    // 记住回放前的状态，回放结束恢复
    bleAudioTxSetState(BLE_AUDIO_TX_REPLAY);              // 切入回放态（抢占实时发送）
    Serial.printf("【回传】开始回传 %d 条录音\n", replayFileCount);
}

/**
 * @brief 停止 0x20 录音回传（原 stopReplay()，:421-430）。
 * 入参/出参：无。
 * 引用的变量：replay* 系列、bleAudioTxState、replayPrevState（本文件上方）。
 */
void audio_tx_stop_replay(void)
{
    if (bleAudioTxState != BLE_AUDIO_TX_REPLAY) {         // 未在回放则无需处理
        return;
    }
    openReplayFileCleanup();                              // 关闭当前回传文件/解复用器
    bleAudioTxSetState(replayPrevState);                  // 恢复到进入回放前的状态
    Serial.printf("【回传】已停止（共回传 %lu 帧）\n",
                  (unsigned long)replayFrameCount);
}

/**
 * @brief 0x30：请求切到「实时发送」（原 bleAudioTxRequestLive()，:434-440）。
 * 入参/出参：无。
 * 引用的变量：bleAudioTxState（本文件上方）；audio_tx_stop_replay()（本文件上方）。
 */
void audio_tx_request_live(void)
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 回放中：先收尾回放
        audio_tx_stop_replay();
    }
    bleAudioTxSetState(BLE_AUDIO_TX_LIVE);                // 切到实时发送（幂等）
}

/**
 * @brief 0x31：请求「暂停推送」（原 bleAudioTxRequestPause()，:444-450）。
 * 入参/出参：无。
 * 引用的变量：bleAudioTxState（本文件上方）；audio_tx_stop_replay()（本文件上方）。
 */
void audio_tx_request_pause(void)
{
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {         // 回放中：先收尾回放
        audio_tx_stop_replay();
    }
    bleAudioTxSetState(BLE_AUDIO_TX_PAUSED);              // 切到暂停（幂等）
}

/**
 * @brief 回传一帧（由 audio_tx_tick 在回放期间反复调用，非阻塞）。
 * 入参/出参：无。
 * 引用的变量：
 *   - s_bleReady / s_sink / replay* 系列（本文件上方）
 *   - OPUS_OUTPUT_MAX_BYTES / REPLAY_MIN/MAX_FRAME_DELAY_MS（src/config.h）
 *   - recorder_frame_time()（src/recorder.h）、ogg_opus_read_packet()（src/ogg_opus.h）
 */
static void pumpReplay()
{
    // 手机断开或未订阅时暂停推送（数据不丢弃，恢复后继续）。
    // ≡ 原 app.cpp:456 的 `!connected || !audioSubscribed || audioDataCharacteristic == nullptr`
    if (!s_bleReady || s_sink == nullptr) {
        return;
    }

    if (!replayReaderOpen && !openNextReplayFile()) {     // 当前无文件则打开下一个
        Serial.printf("【回传】回传完成（共 %lu 帧）\n",
                      (unsigned long)replayFrameCount);
        bleAudioTxSetState(replayPrevState);              // 回放自然结束，恢复回放前的状态
        return;
    }

    uint8_t opus_buf[OPUS_OUTPUT_MAX_BYTES];              // 一帧 Opus 数据
    size_t len = 0;                                       // 实际长度
    int64_t granule = 0;                                  // 该帧的时间刻度（48kHz）
    if (!ogg_opus_read_packet(&replayReader, replayFile, opus_buf, sizeof(opus_buf),
                              &len, &granule)) {
        // 当前文件读完，下一个由下一轮 pump 打开。
        openReplayFileCleanup();
        return;
    }

    // granule 差值节流：granule 单位是 48kHz 刻度，/48 即毫秒。
    // 非阻塞写法——绝不在 loop 里 delay，避免饿死 mic/Opus。
    if (replayLastGranule >= 0) {
        int64_t d = (granule - replayLastGranule) / 48;   // 与上一帧的时间差（ms）
        if (d > REPLAY_MAX_FRAME_DELAY_MS) {
            d = REPLAY_MAX_FRAME_DELAY_MS;                // 上限截断
        }
        if (d < REPLAY_MIN_FRAME_DELAY_MS) {
            d = REPLAY_MIN_FRAME_DELAY_MS;                // 下限截断
        }
        uint32_t now = millis();
        if ((int32_t)(now - replayNextSendMs) < 0) {
            return;   // 还没到发送时刻，帧暂存等待下轮
        }
        replayNextSendMs = now + (uint32_t)d;             // 安排下一帧时刻
    }
    replayLastGranule = granule;                          // 记录本帧刻度

    // 组包负载：[6B BCD 北京时间][opus]
    // （3 字节包头 [2B 序号][1B 子序号] 由发送出口负责拼接，与原 wire 格式一致）
    uint8_t bcd[6];
    recorder_frame_time(replayCurName, granule, bcd);     // 由文件名 + granule 反算墙钟时间

    uint8_t payload[6 + OPUS_OUTPUT_MAX_BYTES];           // 负载上限 = 6B 时间戳 + 160B opus
    memcpy(payload, bcd, 6);                              // 前 6 字节：BCD 时间
    memcpy(payload + 6, opus_buf, len);                   // 之后：Opus 数据

    s_sink(replaySeq++, 0, payload, 6 + len);             // ≡ 原 :506-508 的 setValue+notify+replaySeq++
    replayFrameCount++;                                   // ≡ 原 :509
}

// -------------------------------------------------------------------------
// 实时链路：编码结果入缓冲 → 按状态发送
// -------------------------------------------------------------------------

/**
 * @brief 收到一帧已编码 Opus，写入发送环形缓冲（原 onOpusEncoded()，:714-743）。
 * 入参：data = Opus 数据；len = 字节数。出参：无。
 * 引用的变量：s_ring / s_ringWrite / s_ringRead（本文件上方）；
 *             OPUS_OUTPUT_MAX_BYTES / AUDIO_TX_BUFFER_SIZE（src/config.h）。
 */
void audio_tx_on_opus(uint8_t *data, size_t len)
{
    // Store encoded data in TX ring buffer
    if (len > OPUS_OUTPUT_MAX_BYTES) {
        return;                                          // 超长帧直接丢弃
    }

    // Write length (2 bytes) + data
    size_t packet_size = len + 2;                        // 帧总长 = 2B 长度头 + 数据
    size_t next_write = (s_ringWrite + packet_size) % AUDIO_TX_BUFFER_SIZE; // 写完后的写指针

    // Check for buffer overflow
    if ((s_ringWrite < s_ringRead && next_write >= s_ringRead) ||
        (s_ringWrite >= s_ringRead && next_write < s_ringWrite &&
         next_write >= s_ringRead)) {
        // Buffer full, skip this packet
        return;                                          // 缓冲满，静默丢帧（既有行为）
    }

    // Write length
    s_ring[s_ringWrite] = len & 0xFF;                    // 长度低字节
    s_ring[(s_ringWrite + 1) % AUDIO_TX_BUFFER_SIZE] = (len >> 8) & 0xFF; // 长度高字节

    // Write data
    for (size_t i = 0; i < len; i++) {                   // 环形逐字节拷贝
        s_ring[(s_ringWrite + 2 + i) % AUDIO_TX_BUFFER_SIZE] = data[i];
    }

    s_ringWrite = next_write;                            // 提交写指针
}

/**
 * @brief 排空环形缓冲，按状态决定是否真正推送（原 processAudioTx()，:763-807）。
 * 入参/出参：无。
 * 引用的变量：s_ring / s_ringRead / s_ringWrite / s_bleReady / s_sink /
 *             s_audioPacketIndex / bleAudioTxState（本文件上方）；
 *             OPUS_OUTPUT_MAX_BYTES（src/config.h）。
 */
static void processAudioTx()
{
    if (!s_bleReady) {                                   // ≡ 原 !connected || !audioSubscribed
        return;
    }

    if (s_sink == nullptr) {                             // ≡ 原 audioDataCharacteristic == nullptr
        return;
    }

    // 只有「实时发送」态才真正推给手机；暂停/回放态只排空缓冲、不发送。
    // 这样暂停期不会积压陈旧音频，恢复后从当下开始，无突发补发。
    const bool send = (bleAudioTxState == BLE_AUDIO_TX_LIVE);   // 本轮是否真正推送

    // Check if we have data in the ring buffer
    while (s_ringRead != s_ringWrite) {                  // 缓冲非空
        // Read length
        uint16_t len =
            s_ring[s_ringRead] | (s_ring[(s_ringRead + 1) % AUDIO_TX_BUFFER_SIZE] << 8); // 取出帧长度

        if (len == 0 || len > OPUS_OUTPUT_MAX_BYTES) {
            // Invalid packet, skip
            s_ringRead = (s_ringRead + 2) % AUDIO_TX_BUFFER_SIZE; // 跳过坏帧
            continue;
        }

        // Update read position（无论推不推送都推进读指针 → 暂停期的帧被丢弃）
        size_t saved_read = s_ringRead;                  // 暂存本帧起始，供拷贝用
        s_ringRead = (s_ringRead + 2 + len) % AUDIO_TX_BUFFER_SIZE;

        if (send) {
            // Read data
            static uint8_t temp_data[OPUS_OUTPUT_MAX_BYTES];    // 单帧临时缓冲（静态，避免占栈）
            for (size_t i = 0; i < len; i++) {                  // 逐个字节拷出（环形，需取模）
                temp_data[i] = s_ring[(saved_read + 2 + i) % AUDIO_TX_BUFFER_SIZE];
            }

            // Send packet
            if (s_sink(s_audioPacketIndex, 0, temp_data, len)) { // ≡ 原 broadcastAudioPacket()
                s_audioPacketIndex++;                            // ≡ 原 :760：只有真正发出才自增序号
            }

            // Small delay to prevent BLE congestion
            delay(1);                                           // 仅在推送时限速，防 BLE 拥塞
        }
    }
}

// =========================================================================
// 对外接口（说明见 audio_tx.h）
// =========================================================================

/**
 * @brief 初始化推送模块：打印开机初始推送状态。
 * 入参/出参：无。
 * 说明：逐字等价于重构前 setup_app() 里的那段开机日志（原 app.cpp:1117-1119）。
 */
void audio_tx_init(void)
{
    // 开机初始推送状态：默认「暂停」，手机订阅音频通知后自动开始推送。
    Serial.printf("【音频】推送状态 → %s（手机订阅音频特征后自动开始；0x31 暂停，0x30 恢复）\n",
                  bleAudioTxStateName(bleAudioTxState)); // 打印当前（默认暂停）状态
}

/**
 * @brief 注册音频帧发送出口。
 * 入参：fn 发送函数指针。出参：无。
 */
void audio_tx_set_sink(audio_tx_sink_fn fn)
{
    s_sink = fn;
}

/**
 * @brief 每轮主循环：按状态推送音频（原 loop_app():1180-1186 的闸门）。
 * 入参：bleReady = BLE 已连接且手机已订阅音频。出参：无。
 */
void audio_tx_tick(bool bleReady)
{
    s_bleReady = bleReady;                               // 缓存供 pumpReplay/processAudioTx 使用

    if (!bleReady) {                                     // ≡ 原 `if (connected && audioSubscribed)`
        return;                                          // 未连接/未订阅：本轮什么也不做
    }

    // 三态状态机：回放走 pumpReplay；实时/暂停都走 processAudioTx（由状态决定是否真发）。
    if (bleAudioTxState == BLE_AUDIO_TX_REPLAY) {        // 回放态：推回传帧
        pumpReplay();
    } else {
        processAudioTx();                                // 实时态=发送，暂停态=只排空缓冲
    }
}

/**
 * @brief 手机音频订阅状态变化：订阅即开始（原 app.cpp:847-851 的 4 行）。
 * 入参：subscribed = 是否已订阅。出参：无。
 */
void audio_tx_on_subscribe(bool subscribed)
{
    if (!subscribed) {
        return;                                          // 取消订阅：重构前此处不做任何状态处理
    }
    // 订阅即开始：手机开启音频通知 = 明确「要音频」，自动切到实时发送。
    // 若正在回放（REPLAY）则不打断，由回放结束后的状态恢复逻辑接管。
    if (bleAudioTxState == BLE_AUDIO_TX_PAUSED) {        // 仅从暂停态自动开始
        bleAudioTxSetState(BLE_AUDIO_TX_LIVE);           // 切到实时发送
    }
}
