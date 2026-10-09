#include "ble_transport.h"

// =============================================================================
// ble_transport.cpp —— BLE 传输/协议层（GATT）实现
//
// 【本文件的作用】
// 负责 BLE 协议栈的一切：起协议栈、建 GATT 服务与特征、维护连接/订阅状态、
// 向手机 notify 音频与电量、以及把手机写来的命令原样转发给上层。
// 这些代码原先是 src/app.cpp 的一部分（原 :42-82、:693-712、:809-890、:957-967、:972-1076），
// 按分层重构搬迁至此。
//
// 【本次搬迁做的唯一改动：切断协议层对业务层的直接调用】
// 重构前，各个 BLE 回调类里直接写着业务函数名，例如：
//   - VoiceprintControlCallback::onWrite 里写 voiceprintPendingPayload[...] 等业务状态
//   - AudioCCCDCallback::onWrite       里直接读写 bleAudioTxState、调 bleAudioTxSetState()
//   - ServerHandler::onConnect         里直接写 lastActivity、调 updateBatteryService()
// 现在一律改为调用**由应用层注册进来的函数指针**（s_rxFn / s_connFn / s_subFn），
// 本文件不再出现任何业务函数名 —— 这是本次重构的核心解耦点。
//
// 【分层】协议层。允许 include 同层协议 peer（ota.h），不允许 include 业务模块。
// 【配套头文件】ble_transport.h（对外接口说明）
// =============================================================================

#include <Arduino.h> // 必须显式包含：BLEDevice.h 等 BLE 头文件本身不拉入 Arduino.h

#include <BLE2902.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>

#include "config.h" // UUID 宏、设备名、MTU、广播间隔、包头长度宏等
#include "ota.h"    // 同层协议 peer：OTA 特征的指针注入与命令转发

// ---------------------------------------------------------------------------------
// BLE UUID 定义（原 app.cpp:42-59，逐行搬迁）
// ---------------------------------------------------------------------------------

// Device Information Service UUIDs
#define DEVICE_INFORMATION_SERVICE_UUID (uint16_t) 0x180A
#define MANUFACTURER_NAME_STRING_CHAR_UUID (uint16_t) 0x2A29
#define MODEL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A24
#define FIRMWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A26
#define HARDWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A27
#define SERIAL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A25

// Main Friend Service - using config.h UUIDs
static BLEUUID serviceUUID(OMI_SERVICE_UUID);                 // 主服务（音频+命令）
static BLEUUID audioDataUUID(AUDIO_DATA_UUID);                // 音频数据特征 19B10001
static BLEUUID audioCodecUUID(AUDIO_CODEC_UUID);              // 音频 codec 特征 19B10002
static BLEUUID voiceprintControlUUID(VOICEPRINT_CONTROL_UUID); // 命令写特征 19B10003

// OTA Service UUIDs
static BLEUUID otaServiceUUID(OTA_SERVICE_UUID);              // OTA 服务 19B10010
static BLEUUID otaControlUUID(OTA_CONTROL_UUID);              // OTA 控制特征 19B10011
static BLEUUID otaDataUUID(OTA_DATA_UUID);                    // OTA 进度特征 19B10012

// ---------------------------------------------------------------------------------
// 特征指针（原 app.cpp:62-67 的全局量，现收进本模块并改为文件静态）
// 只有本文件内部创建/使用；外部如需上报，一律走本文件的公开函数。
// ---------------------------------------------------------------------------------
static BLECharacteristic *s_batteryLevelCharacteristic = nullptr;    // 电池电量特征（0x2A19）
static BLECharacteristic *s_audioDataCharacteristic = nullptr;       // 音频 notify 特征
static BLECharacteristic *s_audioCodecCharacteristic = nullptr;      // codec 标识特征
static BLECharacteristic *s_otaControlCharacteristic = nullptr;      // OTA 控制特征
static BLECharacteristic *s_otaDataCharacteristic = nullptr;         // OTA 进度特征
static BLECharacteristic *s_voiceprintControlCharacteristic = nullptr; // 命令写特征

// ---------------------------------------------------------------------------------
// 连接 / 订阅状态（原 app.cpp:71 的 audioSubscribed、:75 的 connected）
// 这两个量会被 Bluedroid 任务（BLE 回调）与主循环同时访问，
// 原代码是普通全局变量，语义保持不变（audioSubscribed 仍带 volatile）。
// ---------------------------------------------------------------------------------
static volatile bool s_audioSubscribed = false; // 手机是否已订阅音频通知
static bool s_connected = false;                // 是否有客户端连接

// ---------------------------------------------------------------------------------
// 音频发送缓冲（原 app.cpp:82 的 audio_packet_buffer）
// ⚠️ 尺寸从 OPUS_OUTPUT_MAX_BYTES + AUDIO_PACKET_HEADER_SIZE(163B) 扩大到
//    REPLAY_PACKET_MAX_BYTES(169B)：因为重构后回传路径也走同一个发送函数，
//    而它的负载是 [6B BCD 时间][opus]，比实时链路长 6 字节。
//    （重构前回传路径自己另开了一个 169B 的局部数组，实时路径才用这 163B 的缓冲。）
// ---------------------------------------------------------------------------------
static uint8_t s_packetBuf[REPLAY_PACKET_MAX_BYTES];

// ---------------------------------------------------------------------------------
// 由应用层（app.cpp）注册进来的回调指针 —— 协议层向上通知的唯一出口
// ---------------------------------------------------------------------------------
static omi_ble_rx_fn s_rxFn = nullptr;   // 手机写入命令特征时调用
static omi_ble_conn_fn s_connFn = nullptr; // 连接/断开时调用
static omi_ble_conn_fn s_subFn = nullptr;  // 音频订阅状态变化时调用

// =========================================================================
// BLE 回调类（原 app.cpp:809-890 的 4 个 + :693-712 的 1 个，共 5 个）
// 这些类一律不出现业务函数名，只调用上面注册进来的函数指针。
// =========================================================================

/**
 * @brief 服务器级回调：连接建立、MTU 协商、连接断开。
 * 说明：原 app.cpp:812-834。
 */
class ServerHandler : public BLEServerCallbacks
{
    void onConnect(BLEServer *server) override
    {
        s_connected = true;         // 标记已连接
        s_audioSubscribed = false;  // 新连接默认未订阅
        if (s_connFn) {
            s_connFn(true);         // ≡ 原 `lastActivity = millis()` + 上报电量；由 app.cpp 实现
        }
        Serial.println(">>> BLE Client connected.");
        // 备注：重构前「上报电量」发生在这一行打印之后，现改由上面的回调统一处理，
        //       仅影响这一条串口日志与一次 notify 的相对先后，功能无差异。
    }
    void onMtuChanged(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
    {
        Serial.printf(">>> MTU negotiated: %d\n", param->mtu.mtu); // 打印协商后的 MTU
    }
    void onDisconnect(BLEServer *server) override
    {
        s_connected = false;        // 标记断开
        s_audioSubscribed = false;  // 断开即视为未订阅
        if (s_connFn) {
            s_connFn(false);        // 通知上层；app.cpp 的实现对 false 不做额外动作（与重构前一致）
        }
        Serial.println("<<< BLE Client disconnected. Restarting advertising.");
        BLEDevice::startAdvertising(); // 断开后重新开始广播，保证可再次被发现
    }
};

/**
 * @brief 音频数据特征的 CCCD 回调：手机开/关通知时触发。
 * 说明：原 app.cpp:837-858。重构前这里直接读写 bleAudioTxState，
 *       现改为「先更新本模块的订阅标志，再回调上层」。
 */
class AudioCCCDCallback : public BLEDescriptorCallbacks
{
    void onWrite(BLEDescriptor *pDescriptor)
    {
        uint8_t *value = pDescriptor->getValue();
        if (value && pDescriptor->getLength() >= 2) {
            // Check notification bit (bit 0)
            if (value[0] & 0x01) {
                s_audioSubscribed = true;                  // ① 先更新本模块状态
                Serial.println("Audio notifications enabled");
                if (s_subFn) {
                    s_subFn(true);                         // ② 再通知上层
                }
                // 重构前此处还有「订阅即开始」逻辑（暂停态自动切实时发送），现已移交
                // src/audio_tx.cpp 的 audio_tx_on_subscribe()，由 app.cpp 注册进来。
            } else {
                s_audioSubscribed = false;                 // 取消订阅
                Serial.println("Audio notifications disabled");
                if (s_subFn) {
                    s_subFn(false);                        // 通知上层（audio_tx 对其不做动作）
                }
            }
        }
    }
};

/**
 * @brief 音频数据特征的读写回调：仅用于记录发送结果，无实际逻辑。
 * 说明：原 app.cpp:860-873，逐行保持不变。
 */
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

/**
 * @brief OTA 控制特征回调：写入转发给 OTA 模块，读取返回当前状态。
 * 说明：原 app.cpp:875-890。ota.cpp 是本模块的**同层协议 peer**，
 *       故这里允许直接调用 ota_handle_command() / ota_get_status()（不算跨层）。
 */
class OTAControlCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pChar) override
    {
        std::string value = pChar->getValue();
        if (value.length() > 0) {
            ota_handle_command((uint8_t *) value.data(), value.length()); // 交给 OTA 模块解析
        }
    }

    void onRead(BLECharacteristic *pChar) override
    {
        uint8_t status[2] = {ota_get_status(), 0}; // [状态, 进度]
        pChar->setValue(status, 2);
    }
};

/**
 * @brief 命令特征（19B10003）回调：把手机写来的原始字节转发给上层。
 * 说明：原 app.cpp:693-712 的 VoiceprintControlCallback。
 *       重构前它直接写业务状态（voiceprintPendingPayload 等），
 *       现在只做「取长度 → 取字节 → 交给 s_rxFn」，
 *       原来的 5 字节截断与「先写负载后写命令」的次序不变式，
 *       已随接收方一起搬到 src/cmd_router.cpp。
 */
class VoiceprintControlCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        size_t len = pCharacteristic->getLength();
        if (len < 1) {
            return; // 空写入直接忽略（原 :698-700 的早退，保持不变）
        }
        std::string val = pCharacteristic->getValue();
        if (s_rxFn) {
            s_rxFn((const uint8_t *) val.data(), val.length()); // 原样转发，不做任何解析
        }
    }
};

// =========================================================================
// 对外接口实现（说明见 ble_transport.h）
// =========================================================================

/**
 * @brief 初始化 BLE：起协议栈、建服务与特征、开始广播。
 * 入参：initialBatteryPct 开机电量初值；audioCodecId 音频 codec 标识。
 * 出参：无。
 * 说明：逐行等价于重构前 app.cpp 的 configure_ble()（原 :972-1076），
 *       仅两处取值改为由参数传入（电量、codec id），避免协议层反向依赖底层/业务模块。
 */
void ble_transport_init(uint8_t initialBatteryPct, uint8_t audioCodecId)
{
    Serial.println("Initializing BLE...");
    BLEDevice::init(BLE_DEVICE_NAME);                        // 用 config.h 里的设备名初始化协议栈
    BLEDevice::setMTU(BLE_MTU_SIZE); // Set local MTU (BLE_MTU_SIZE from config.h) 设置本端 MTU
    Serial.printf("Local MTU set to %d\n", BLEDevice::getMTU());
    BLEServer *server = BLEDevice::createServer();            // 建 GATT 服务器
    server->setCallbacks(new ServerHandler());                // 挂连接/MTU/断开回调

    // Main service
    BLEService *service = server->createService(serviceUUID); // 主服务

    // Audio Data characteristic (for streaming audio to app)
    s_audioDataCharacteristic = service->createCharacteristic(
        audioDataUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *audioCcc = new BLE2902();                        // 音频通知的 CCCD
    audioCcc->setNotifications(true);                         // 允许通知
    audioCcc->setCallbacks(new AudioCCCDCallback());          // 挂订阅状态回调
    s_audioDataCharacteristic->addDescriptor(audioCcc);
    s_audioDataCharacteristic->setCallbacks(new AudioDataCallback());

    // Audio Codec characteristic (tells app which codec we're using)
    s_audioCodecCharacteristic = service->createCharacteristic(audioCodecUUID, BLECharacteristic::PROPERTY_READ);
    uint8_t codecId = audioCodecId;                           // ← 由应用层传入（原来直接调 opus_get_codec_id()）
    s_audioCodecCharacteristic->setValue(&codecId, 1);

    // Voiceprint control characteristic: 1-byte commands (enroll/abort/finish/erase/status)
    // 同时支持「有响应写」与「无响应写」：不少客户端/调试工具默认用 Write Without Response，
    // 若只声明 PROPERTY_WRITE，该写入会在 GATT 层被直接拒收，onWrite() 不会被调用，
    // 表现为「发了命令没反应、也不报错」。
    s_voiceprintControlCharacteristic = service->createCharacteristic(
        voiceprintControlUUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    s_voiceprintControlCharacteristic->setCallbacks(new VoiceprintControlCallback());

    // Battery Service
    BLEService *batteryService = server->createService(BATTERY_SERVICE_UUID);
    s_batteryLevelCharacteristic = batteryService->createCharacteristic(
        BATTERY_LEVEL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *batteryCcc = new BLE2902();                      // 电量的 CCCD
    batteryCcc->setNotifications(true);
    s_batteryLevelCharacteristic->addDescriptor(batteryCcc);

    // Set initial battery level
    // ⚠️ 保持原有次序：仍在 service->start() 之前写入初值（与重构前一致）。
    uint8_t initialBatteryLevel = initialBatteryPct;          // ← 由应用层传入（原来调 readBatteryLevel() 后取全局量）
    s_batteryLevelCharacteristic->setValue(&initialBatteryLevel, 1);

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

    manufacturerNameCharacteristic->setValue(MANUFACTURER_NAME);      // 厂商名
    modelNumberCharacteristic->setValue(BLE_DEVICE_NAME);             // 型号（=设备名）
    firmwareRevisionCharacteristic->setValue(FIRMWARE_VERSION_STRING); // 固件版本
    hardwareRevisionCharacteristic->setValue(HARDWARE_REVISION);      // 硬件版本

    // Generate serial number from ESP32 chip ID
    uint64_t chipId = ESP.getEfuseMac();                     // 取芯片唯一 ID
    char serialNumber[17];
    snprintf(serialNumber, sizeof(serialNumber), "%04X%08X", (uint16_t) (chipId >> 32), (uint32_t) chipId);
    serialNumberCharacteristic->setValue(serialNumber);       // 序列号

    // OTA Service
    BLEService *otaService = server->createService(otaServiceUUID);

    // OTA Control characteristic (for receiving commands and reading status)
    s_otaControlCharacteristic = otaService->createCharacteristic(
        otaControlUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    s_otaControlCharacteristic->setCallbacks(new OTAControlCallback());

    // OTA Data characteristic (for progress notifications)
    s_otaDataCharacteristic = otaService->createCharacteristic(
        otaDataUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *otaCcc = new BLE2902();                          // OTA 进度的 CCCD
    otaCcc->setNotifications(true);
    s_otaDataCharacteristic->addDescriptor(otaCcc);

    // Set OTA characteristics for the OTA module
    ota_set_characteristics(s_otaControlCharacteristic, s_otaDataCharacteristic); // 把特征指针交给 OTA 模块

    // Start services
    service->start();            // 启动主服务
    batteryService->start();     // 启动电池服务
    deviceInfoService->start();  // 启动设备信息服务
    otaService->start();         // 启动 OTA 服务

    // Start advertising
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(service->getUUID()); // Main service (fits in 31 bytes) 广播主服务
    advertising->setScanResponse(true);              // 允许扫描响应
    advertising->setMinPreferred(BLE_ADV_MIN_INTERVAL); // 广播最小间隔
    advertising->setMaxPreferred(BLE_ADV_MAX_INTERVAL); // 广播最大间隔
    BLEDevice::startAdvertising();                   // 开始广播

    Serial.println("BLE initialized and advertising started.");
}

/**
 * @brief 查询 BLE 是否已连接。
 * 入参/出参：无；返回 s_connected。
 */
bool ble_transport_is_connected(void)
{
    return s_connected;
}

/**
 * @brief 查询手机是否已订阅音频通知。
 * 入参/出参：无；返回 s_audioSubscribed。
 */
bool ble_transport_audio_subscribed(void)
{
    return s_audioSubscribed;
}

/**
 * @brief 更新电池特征数值并在连接时通知手机。
 * 入参：pct = 0-100 电量百分比。出参：无。
 * 说明：等价于原 app.cpp 的 updateBatteryService()（:957-967），
 *       区别只是电量由调用方传入，而不是自己去读全局量。
 */
void ble_transport_update_battery(uint8_t pct)
{
    if (s_batteryLevelCharacteristic) {                       // 特征已创建才操作
        uint8_t batteryLevel = pct;                           // 归一类型
        s_batteryLevelCharacteristic->setValue(&batteryLevel, 1); // 写数值

        if (s_connected) {                                    // 只有连接时才 notify
            s_batteryLevelCharacteristic->notify();
        }
    }
}

/**
 * @brief 通过音频特征发一帧（拼 3 字节包头后 notify）。
 * 入参：seq 包头序号；subIdx 子序号；payload 负载；len 负载长度。
 * 出参：true = 已 notify；false = 未连接/未订阅/负载超长，已丢弃。
 * 说明：等价于原 broadcastAudioPacket()（:745-761），并统一接管回传路径的发送。
 */
bool ble_transport_send_audio_frame(uint16_t seq, uint8_t subIdx,
                                    const uint8_t *payload, size_t len)
{
    if (!s_connected || !s_audioSubscribed || s_audioDataCharacteristic == nullptr) {
        return false; // 与原 broadcastAudioPacket() 的守卫一致（:747-749）
    }
    if (len > OPUS_OUTPUT_MAX_BYTES + 6) {
        return false; // 防越界：最大负载 = 6B 时间戳 + 160B opus（回传链路）
    }

    // Build packet: 2 bytes index + 1 byte sub-index + data
    s_packetBuf[0] = seq & 0xFF;             // 序号低字节
    s_packetBuf[1] = (seq >> 8) & 0xFF;      // 序号高字节
    s_packetBuf[2] = subIdx;                 // 子序号（实时=0，留给分片用）

    memcpy(s_packetBuf + AUDIO_PACKET_HEADER_SIZE, payload, len); // 拷贝负载

    s_audioDataCharacteristic->setValue(s_packetBuf, len + AUDIO_PACKET_HEADER_SIZE); // 写进特征
    s_audioDataCharacteristic->notify();                                              // 通知手机
    return true;                             // 已发出，调用方可以自增序号
}

/**
 * @brief 注册命令写入回调。
 * 入参：fn 回调指针。出参：无。
 */
void ble_transport_set_rx_callback(omi_ble_rx_fn fn)
{
    s_rxFn = fn;
}

/**
 * @brief 注册连接状态回调。
 * 入参：fn 回调指针。出参：无。
 */
void ble_transport_set_conn_callback(omi_ble_conn_fn fn)
{
    s_connFn = fn;
}

/**
 * @brief 注册音频订阅状态回调。
 * 入参：fn 回调指针。出参：无。
 */
void ble_transport_set_subscribe_callback(omi_ble_conn_fn fn)
{
    s_subFn = fn;
}
