# OMI Glass 固件 —— 音频链路与 BLE 知识笔记

> 整理日期：2026-09-29
> 代码行号基于整理时的 `src/app.cpp`、`src/config.h`、`src/ota.cpp`（摄像头功能已移除后的版本）。

## 目录

1. [音频链路全流程](#1-音频链路全流程)
2. [缓冲区清单](#2-缓冲区清单)
3. [传输速率账](#3-传输速率账)
4. [Flash 占用分析](#4-flash-占用分析)
5. [BLE 三种通信形式](#5-ble-三种通信形式)
6. [UUID 与 UUID 表](#6-uuid-与-uuid-表)
7. [handle：数据怎么对应到 UUID](#7-handle数据怎么对应到-uuid)
8. [MTU 与协商](#8-mtu-与协商)
9. [照片功能（已删除，历史参考）](#9-照片功能已删除历史参考)
10. [OTA 升级逻辑](#10-ota-升级逻辑)
11. [WiFi 用在哪](#11-wifi-用在哪)
12. [C++ 语法点](#12-c-语法点)
13. [调试工具与常见坑](#13-调试工具与常见坑)

---

## 1. 音频链路全流程

从麦克风收音到蓝牙发出，一共 7 个阶段。`loop_app()`（[app.cpp:662](src/app.cpp#L662)）每轮循环依次跑这三步：

```
loop_app()
 ├─ mic_process()      ← 采集
 ├─ opus_process()     ← 编码
 └─ processAudioTx()   ← 发送（仅 connected && audioSubscribed 时）
```

### ① 采集 —— [mic.cpp](src/mic.cpp) `mic_process()`

1. `i2s_read()` 从 I2S DMA 取数据到 `i2s_read_buffer`，最多阻塞 20ms；
2. 乘 `MIC_GAIN`（=2）并削波到 ±32767；
3. 回调 `onMicData()`（[app.cpp:264](src/app.cpp#L264)）把 PCM 交给 Opus 模块。

I2S 配置为 `I2S_MODE_MASTER | RX | PDM`，16kHz / 16bit / 单声道。

> 注：1600 样本要 100ms 才攒满，但超时只有 20ms，所以每次实际读回约 320 个样本（=20ms=1 帧）。

### ② 入队 —— [opus_encoder.cpp](src/opus_encoder.cpp) `opus_receive_pcm()`

样本逐个写进 `pcm_ring_buffer`。环形缓冲满时**丢弃最老的样本**，保证实时性。

### ③ 编码 —— [opus_encoder.cpp](src/opus_encoder.cpp) `opus_process()`

`while (可用样本 >= 320)` 循环：切 320 样本 → `opus_encode()` → 得到压缩包。
配置：VOIP 模式、32kbps、复杂度 3、**VBR 开**、DTX 关。

### ④ 打包入队 —— [app.cpp:270](src/app.cpp#L270) `onOpusEncoded()`

在 `audio_tx_buffer` 写一条 `[2字节小端长度][数据]`。该缓冲在**内部 RAM**（不是 PSRAM），便于 BLE 快速访问。

### ⑤ 发送 —— [app.cpp:319](src/app.cpp#L319) `processAudioTx()`

**门口有订阅检查**：`if (!connected || !audioSubscribed) return;`
`audioSubscribed` 只能由 CCCD 回调 [AudioCCCDCallback::onWrite](src/app.cpp#L382) 置位 —— **手机必须主动订阅音频特征**，设备才会发数据。

### ⑥ 组包 —— [app.cpp:301](src/app.cpp#L301) `broadcastAudioPacket()`

```
audio_packet_buffer[0..1] = audioPacketIndex   (小端序号，每包 +1)
audio_packet_buffer[2]    = 0                  (子索引，恒为 0)
audio_packet_buffer[3..]  = Opus 数据
```

### ⑦ 通知

`audioDataCharacteristic->setValue(...)` + `->notify()`，然后 `audioPacketIndex++`。

---

## 2. 缓冲区清单

| 缓冲区 | 位置 | 大小 | 容纳时长 | 定义处 |
|---|---|---|---|---|
| I2S DMA | 内部 RAM | 8 × 256 帧 × 2B = **4,096 B** | 128 ms | mic.cpp |
| `i2s_read_buffer` | PSRAM | 1600 × 2 = **3,200 B** | 100 ms | mic.cpp |
| `pcm_ring_buffer` | PSRAM | 8000 × 2 = **16,000 B** | 500 ms | opus_encoder.cpp |
| `opus_input_buffer` | PSRAM | 320 × 2 = **640 B** | 20 ms（1 帧） | opus_encoder.cpp |
| `opus_output_buffer` | PSRAM | **160 B** | 1 压缩帧上限 | opus_encoder.cpp |
| `audio_tx_buffer` | **内部 RAM** | 16 × 162 = **2,592 B** | 16 帧 = 320 ms | [app.cpp:71-72](src/app.cpp#L71) |
| `audio_packet_buffer` | 内部 RAM | 160 + 3 = **163 B** | 1 个 BLE 包 | [app.cpp:75](src/app.cpp#L75) |

PCM 侧合计约 23 KB（全在 PSRAM）；发送队列约 2.8 KB（内部 RAM）。

**关于 163 字节**：这是**缓冲区最大容量**（`OPUS_OUTPUT_MAX_BYTES 160` + 包头 3），不是实际发送大小。实际每包是 `len + 3`，VBR 下典型约 83 字节，163 只是"Opus 出满 160 字节"的最坏情况上限。

---

## 3. 传输速率账

```
采样率 16000 Hz  ÷  每帧 320 样本  =  50 帧/秒   （每帧 20ms）
码率   32000 bps ÷  8             =  4000 B/秒 → 平均 80 B/帧
每个 BLE 包 = 3 B 包头 + ~80 B     ≈  83 B
→ 需要约 50 次 notify/秒，约 4.2 KB/s
```

`audio_tx_buffer` 能兜 320ms，覆盖 BLE 连接间隔抖动；若 BLE 端跟不上，缓冲区填满后 `onOpusEncoded()` 会静默丢包（设计上的保护）。

---

## 4. Flash 占用分析

**注意：这里的"百分比"是 app 分区（1.75 MB）的占比，不是 8 MB 芯片的占比。**

分区表 `partitions_ota.csv`：

```
芯片总量           8,388,608 B  (8 MB)
├─ app0  (OTA槽A)  1,835,008 B  (1.75 MB)  ← 当前固件装这里
├─ app1  (OTA槽B)  1,835,008 B  (1.75 MB)  ← OTA 回滚预留
├─ spiffs            458,752 B  (0.44 MB)
└─ nvs / otadata / 引导 ...
```

固件体积参考（不同阶段）：

- 双蓝牙栈 + 相机：约 1,686,432 B（91.9%）
- 单栈（Bluedroid）+ 无相机：约 1,622,629 B（88.4%）

### 谁是 flash 大户

按链接产物（map 文件）统计，flash 段占用大致为：

| 组件 | 占用 | 占比 | 说明 |
|---|---|---|---|
| `libbt.a` + `libbtdm_app.a` + `libBLE.a` | **~520 KB** | ~50% | Bluedroid 蓝牙栈（双模，含经典蓝牙） |
| `libarduino-libopus.a` | ~143 KB | ~14% | Opus 编解码器（SILK + CELT + 解码器全在） |
| WiFi / TLS / HTTPClient / Update | 可观 | — | 仅供 OTA 使用 |
| ESP-IDF 驱动 / 系统 / FreeRTOS | ~140 KB | ~13% | driver、phy、hal、nvs、spi_flash、heap… |
| newlib / libc / libstdc++ | ~120 KB | ~11% | C/C++ 标准库 |
| **你自己的业务代码** | **~4.5 KB** | **<0.5%** | app / mic / opus_encoder / main |

**结论：业务代码只占极小一部分，剩下 99% 都是框架和协议栈。**

> 注意：蓝牙代码用的是 **Bluedroid**（`ESP32 BLE Arduino`），不是 NimBLE。源码里全是 `BLE2902.h`、`BLEDevice.h` 等 Bluedroid 头文件，**不要往 `lib_deps` 里加 `h2zero/NimBLE-Arduino`** —— 会被链接成第二套 BLE 主机栈，既浪费 flash 又可能崩溃。

---

## 5. BLE 三种通信形式

| 形式 | 时机 | 内容 | 频率 |
|---|---|---|---|
| **广播 Advertising** | 未连接时 | 设备名 + 主服务 UUID `19B10000` | 周期性，本项目 200~400ms |
| **服务发现 Discovery** | 刚连接时 | 完整 UUID 表（服务→特征→描述符） | **一次性** |
| **通知 Notification** | 连接后 | 实际数据（音频、电池…） | 各自不同 |

### 代码体现

**① 广播** —— [configure_ble()](src/app.cpp#L512) 末尾：

```cpp
BLEAdvertising *advertising = BLEDevice::getAdvertising();
advertising->addServiceUUID(service->getUUID());      // 只广播主服务
advertising->setScanResponse(true);
advertising->setMinPreferred(BLE_ADV_MIN_INTERVAL);  // 200ms
advertising->setMaxPreferred(BLE_ADV_MAX_INTERVAL);  // 400ms
BLEDevice::startAdvertising();
```

断开时在 [ServerHandler::onDisconnect](src/app.cpp#L372) 里重新调用 `BLEDevice::startAdvertising()`（BLE 设备一次只连一个中心设备，断开后必须重新广播才能被再次搜到）。

**② 服务发现** —— 没有显式"发送"代码，是 BLE 库在连接时自动应答手机的查询。你代码里做的是**搭表**，即 `configure_ble()` 里的 `createService()` / `createCharacteristic()` / `addDescriptor()`。

**③ 通知** —— 三处 `setValue(...) + notify()`：

| 位置 | 频率 |
|---|---|
| [broadcastAudioPacket()](src/app.cpp#L301) 音频 | ~50 次/秒 |
| [updateBatteryService()](src/app.cpp#L497) 电池 | ~1 次/20 秒 + 连接时一次 |
| [ota_notify_status()](src/ota.cpp) OTA | 按需（升级时每 5%） |

> "能不能通知"的前提是**手机先订阅**：订阅动作写 CCCD 描述符（`BLE2902`），触发 `AudioCCCDCallback::onWrite()` 把 `audioSubscribed` 置 true，之后 `processAudioTx()` 才真正开始发。

---

## 6. UUID 与 UUID 表

### UUID 表的作用

UUID 表（GATT 表）= **设备对外暴露的"接口清单"**。手机连上后先读这张表，才知道"这设备有哪些数据、叫什么、能怎么操作"。

层级是三层嵌套：

```
服务 Service (UUID)
  └─ 特征 Characteristic (UUID)   ← 实际数据，带 READ/WRITE/NOTIFY 属性
       └─ 描述符 Descriptor (UUID) ← 如 CCCD(0x2902) 是"订阅开关"
```

### 本项目的 UUID 完整表

| UUID | 类型 | 作用 |
|---|---|---|
| `19B10000` | 主服务 | Omi/Friend 主服务 |
| `19B10001` | 音频数据特征（NOTIFY） | 设备推 Opus 音频流 |
| `19B10002` | 音频编码特征（READ） | 手机查"用哪种编码" |
| `19B10010/11/12` | OTA 服务 / 控制 / 数据 | 远程升级 |
| `0x180F` / `0x2A19` | 电池服务 / 电量 | 蓝牙 SIG 标准 |
| `0x180A` | 设备信息服务 | 厂商 / 型号 / 固件版本 / 序列号 |
| ~~`19B10005` / `19B10006`~~ | ~~照片数据 / 控制~~ | **已随摄像头功能删除** |

### 固定的还是可自定义？

| 类型 | 例子 | 能否改 |
|---|---|---|
| **16 位标准 UUID** | `0x180F`、`0x2A19`、`0x180A` | **不能改**（蓝牙 SIG 规定，系统靠它识别） |
| **128 位自定义 UUID** | `19B10000/01/02/10/11/12-...` | **可自定**，但设备固件与手机 App 两端必须一致 |

自定义 UUID 定义在 [config.h](src/config.h)，例：

```c
#define AUDIO_DATA_UUID  "19B10001-E8F2-537E-4F6C-D104768A1214"
#define AUDIO_CODEC_UUID "19B10002-E8F2-537E-4F6C-D104768A1214"
```

---

## 7. handle：数据怎么对应到 UUID

**关键：音频数据包里没有任何 UUID 标记**，绑定发生在 BLE 协议层的 **attribute handle（属性句柄）**。

### 数据 → UUID 的绑定路径

代码里发送时写的是**特征对象**，不是 UUID 字符串：

```cpp
audioDataCharacteristic->notify();   // 只认对象，不认号
```

UUID 是在 `configure_ble()` 里"绑到特征对象上"的：

```cpp
audioDataCharacteristic = service->createCharacteristic(
    audioDataUUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
```

### 空中包的真实结构

BLE 栈实际发出的是一帧 **ATT "Handle Value Notification"**：

```
┌──────────┬──────────────┬────────────────────────────┐
│ Opcode   │ Handle       │ Value（你的音频数据）        │
│ 1 字节   │ 2 字节       │ N 字节                     │
│ 0x1B     │ 0x28 0x00    │ [序号][序号][子索引][Opus]   │
└──────────┴──────────────┴────────────────────────────┘
```

**每个包都带 handle**，由 BLE 栈自动加上，你的代码里看不到这个数字。

### 手机怎么"知道谁是谁"

```
手机收到通知 → 读 Opcode 0x1B → 读 Handle 0x0028
   → 查"服务发现"时缓存的 handle→UUID 映射表
   → 0x0028 = 19B10001 = 音频 → 交给音频解码
```

**UUID 只出现在服务发现那一刻的映射表里；之后每次通知只用 handle，不再带 UUID。**

### 类比

把每个特征想成**带编号的信箱**：

- 服务发现 = 设备告诉手机"42 号信箱是音频、50 号是电池"；
- `notify(音频特征)` = 往 42 号信箱塞信；
- 手机看到 42 号信箱有信，就知道是音频。

### 分层开销

```
你的应用数据（纯音频字节）          83 B
   ↓ ATT 层：加 [opcode][handle]    +3   → 86
   ↓ L2CAP 层：加 [长度][信道号]     +4   → 90
   ↓ 链路层：加 [访问地址][LL头][CRC] +~10 → ~100 B
```

**空中包比代码里的 83 字节大（多约 17 字节开销）**，但这些开销全被 BLE 栈吃掉，**手机 App 拿到的数据仍是 83 字节**。

---

## 8. MTU 与协商

### 为什么重要

MTU 决定**一帧通知里"value"最大能装多少**。

- 默认 MTU = 23 → 减 3 字节 ATT 包头 → **每次最多 20 字节**。
- 你的音频一帧 83 字节 → **装不下**！

这就是"连上但收不到数据"的经典坑。

### 协商机制

1. 一方发 **Exchange MTU Request**（带上期望值，如 517）；
2. 另一方回 **Exchange MTU Response**（带上自己上限）；
3. 生效 MTU = **min(双方)**。

### 各平台行为

| 端 | 是否自动 | 说明 |
|---|---|---|
| **Android** | ❌ 不自动 | App 必须显式调 `gatt.requestMtu(517)`，不调就一直是 23 |
| **iOS** | ✅ 自动 | CoreBluetooth 连接时系统自动协商 |
| **本 ESP32 固件** | ❌ 不主动 | 代码从没调 `setMTU()`，纯当被动应答方 |

> `config.h` 里定义了 `BLE_MTU_SIZE 517`，但**全工程没有调用过 `BLEDevice::setMTU()`** —— 这个宏是没被使用的。实际能传 83 字节音频，靠的是 **Omi App 主动协商了大 MTU**。

### 为什么不一上来就设大

1. **默认 23 是兼容底线**：BLE 4.0/4.1 规范固定 23，BLE 4.2 引入 Data Length Extension 后才允许到 517，但默认值保留 23。
2. **不能单方面设大**：必须协商取 min。
3. **不是越大越好**：单包大 → 空中占用时间长，可能增加功耗/时延。

### 可选改进

若想让设备侧更稳，可主动请求（Bluedroid 需在连接后调底层 API；NimBLE 则是 `BLEDevice::setMTU(BLE_MTU_SIZE)`）。但主流 App 已处理，通常不必。

---

## 9. 照片功能（已删除，历史参考）

> 摄像头/拍照功能已于 2026-09-29 移除。以下为历史记录，便于理解原来的双通道设计。

### 传输方式：BLE GATT Notification

- **数据特征** `19B10005`（READ | NOTIFY）—— 推 JPEG 分片
- **控制特征** `19B10006`（WRITE）—— 手机写拍照命令

### 流程

1. `take_photo()` 抓一帧 **JPEG**（VGA 640×480），存进全局 `camera_fb_t *fb`；
2. 把整帧切成 **~200 字节/片**，每片调一次 `notify()`；
3. 包头（小端 2 字节帧序号）：
   - 第一片：`[序号低][序号高][方向字节]` + 199 字节数据
   - 后续片：`[序号低][序号高]` + 200 字节数据
4. 传完后发一包 `0xFF 0xFF` 表示"这张照片结束"。

### 控制命令（写 `19B10006`）

| 值 | 含义 |
|---|---|
| `-1` | 拍一张单张 |
| `0` | 停止 |
| `5~300` | 定时连拍（实际固定用 30 秒间隔） |

### 与音频的关系

**在同一个 `loop()` 里交替发送**，但音频优先级更高：

- 照片每轮最多发 **2 片**（限流）；
- 发送前先检查"音频缓冲有数据吗"，有则让位给音频。

两条通道**完全独立**（不同特征、不同封包格式），所以删除照片**不影响音频**。

---

## 10. OTA 升级逻辑

**三段式：BLE 当遥控器 → WiFi 当数据通道 → `Update` 库当刷写器。**

### 三个 BLE 特征（服务 `19B10010`）

| 特征 | UUID | 属性 | 作用 |
|---|---|---|---|
| OTA Control | `19B10011` | READ \| WRITE | 手机写命令；读状态 |
| OTA Data | `19B10012` | READ \| NOTIFY | 设备回报进度 |

### 命令集（手机写 `19B10011`）

| 命令 | 值 | 数据格式 |
|---|---|---|
| `SET_WIFI` | `0x01` | `[cmd, ssid_len, ssid..., pass_len, pass...]` |
| `SET_URL` | `0x05` | `[cmd, url_len_hi, url_len_lo, url...]`（2 字节大端） |
| `START_OTA` | `0x02` | 无（需先 SET_WIFI + SET_URL） |
| `CANCEL_OTA` | `0x03` | 无 |
| `GET_STATUS` | `0x04` | 无 |

### 完整流程

```
① 手机 --BLE--> SET_WIFI：WiFi 账号密码
② 手机 --BLE--> SET_URL ：固件下载 URL
③ 手机 --BLE--> START_OTA
④ 设备连上那个 WiFi          (connect_wifi)
⑤ 设备 HTTP GET 那个 URL 下载 (download_and_install_firmware)
⑥ Update.write() 边下边写入 OTA 分区
⑦ Update.end(true) 校验 → ESP.restart() → 切到新固件
```

**关键点**：手机只需提供"网络钥匙（SSID/密码）+ 下载地址"，**设备自己去连 WiFi、自己下载、自己刷写**。

### 状态机（经 `19B10012` notify，2 字节 `[status, progress]`）

| 状态值 | 含义 |
|---|---|
| `0x00` | IDLE 空闲 |
| `0x10/0x11/0x12` | WiFi 连接中 / 已连 / 失败 |
| `0x20/0x21/0x22` | 下载中 / 完成 / 失败 |
| `0x30/0x31/0x32` | 安装中 / 完成 / 失败 |
| `0x40` | 正在重启 |
| `0xFF` | 出错 |

### 实现细节

- `START_OTA` 会创建**独立 FreeRTOS 任务** `ota_task`（栈 8192、优先级 5），所以 `ota_loop()` 是空的，OTA 不阻塞主循环；
- **双分区切换**：新固件写到非当前运行的分区，`Update.end(true)` 标记有效后重启，bootloader 自动切换；失败则留在旧固件，天然可回滚；
- **⚠️ 安全隐患**：HTTPS 下载用了 `secureClient->setInsecure()`，**跳过证书校验**（代码注释也标了 TODO），中间人可伪造固件。发布产品前应改为校验证书。

---

## 11. WiFi 用在哪

**只有 OTA 用，其它功能（音频、蓝牙、电池、按键）与 WiFi 无关。**

- 代码集中在 [ota.cpp](src/ota.cpp)，引用 `WiFi.h` / `WiFiClientSecure.h` / `HTTPClient.h` / `Update.h`；
- 配置在 [config.h](src/config.h)：`WIFI_CONNECT_TIMEOUT_MS 15000`、`WIFI_MAX_SSID_LEN/PASS_LEN`、`OTA_MAX_URL_LEN`。

**WiFi 平时是关着的**：只有收到 `START_OTA` 后在 `ota_task` 里才 `WiFi.begin()`，升级完重启前主动关闭：

```cpp
WiFi.disconnect(true);
WiFi.mode(WIFI_OFF);
```

所以正常使用时 WiFi 射频完全关闭，不耗电、不干扰蓝牙。

> 虽然 WiFi 运行时是关的，但 WiFi / TLS(mbedTLS) / HTTPClient / Update 的**代码库已链接进固件**，是 flash 占用的重要来源之一。若确定不需要 OTA，删掉 [ota.cpp](src/ota.cpp) 和这些库可再省一截 flash。

---

## 12. C++ 语法点

### `override` 关键字

```cpp
class ServerHandler : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override { ... }
    void onDisconnect(BLEServer *server) override { ... }
};
```

- 含义："**我在重写基类里的虚函数**"。
- 作用是让**编译器帮你检查**：函数名/参数写错时直接报错，而不是静默变成"定义了一个新函数"。
- 不用也能跑，但它是安全网，是好习惯。

### 回调（callback）机制

`configure_ble()` 里：

```cpp
server->setCallbacks(new ServerHandler());
```

把对象注册给 BLE 服务器后，**蓝牙栈在"手机连上/断开"事件发生时自动调用**你重写的 `onConnect` / `onDisconnect`。你不用（也不能）手动调用。

`onConnect` 做的事：置 `connected = true`、重置 `audioSubscribed = false`、记录 `lastActivity`、推一次电池电量。

`onDisconnect` 做的事：置 `connected = false`、重置 `audioSubscribed`、**重新开始广播**（否则手机再也搜不到设备）。

---

## 13. 调试工具与常见坑

### ① 改代码 ≠ 设备生效

源码改动必须经过 **重新编译 + 重新烧录** 两步才落到设备上。只编译不烧录，设备跑的还是旧固件。

```bash
pio run -e seeed_xiao_esp32s3 --target upload
pio device monitor --baud 115200
```

### ② nRF Connect 会缓存服务表

nRF Connect 按**设备 MAC 地址缓存**发现过的服务列表。重刷固件后 MAC 没变，所以 App 可能仍显示旧 UUID。

**清理方法**（由轻到重）：

1. 连接后点 `⋮` → **Refresh services**；
2. 断开 → `⋮` → **Delete bond information** → 重连；
3. 从扫描列表**移除/忘记设备** → 重新扫描连接。

### ③ `python` 命令是 Windows 商店的假 stub

本机 `python` 解析到 `AppData\Local\Microsoft\WindowsApps\python`（商店别名），**非交互模式下直接退出（错误码 49）且无任何输出**。

**解决**：用真实路径

```bash
"/c/Users/86133/AppData/Local/Programs/Python/Python314/python.exe" decode.py <文件>
```

或到"设置 → 应用 → 高级应用设置 → 应用执行别名"里关掉 `python.exe` / `python3.exe` 别名。

### ④ 用 decode.py 解码 nRF 抓到的音频

**nRF Connect 录制的宏文件（.xml）** 里，音频数据藏在每个 `<assert-value value="HEX..."/>` 属性中。提取步骤：

```bash
# 1. 抠出所有 value="..." 的十六进制
grep -oE 'value="[0-9A-Fa-f]+"' audio3.xml \
  | sed -E 's/value="//; s/"$//' > audio3_packets.txt

# 2. 解码成 wav（注意用真实 python 路径）
"<python.exe路径>" decode.py audio3_packets.txt
```

**注意**：这种粗暴 grep 会**误抓非音频的断言值**（比如 `<assert-value value="00"/>`），需要人工剔除 —— 它们在解码时会被跳过，但会污染统计。

**关于包长度不一致**：Opus 是 **VBR（可变码率）**，每帧压缩后大小随音频内容变化，所以包长度**本来就会不一样**（典型 20 字节，偶尔 17/18/19 字节），属正常现象。若发现**绝大多数包恰好等于 `MTU-3`**，则要怀疑 MTU 没协商上去导致**截断**。

### ⑤ VS Code 没有"转到定义"

那是 C/C++ 扩展（cpptools）的 IntelliSense 没工作，与代码无关。

```
Ctrl+Shift+P → PlatformIO: Rebuild IntelliSense Index
```

仍不行则：`C/C++: Reset IntelliSense Database` → `Developer: Reload Window`。
状态栏右侧应有 "C/C++" 图标（转圈=索引中，⚠️=配置有问题）。

---

## 附：一句话速查

| 问题 | 答案 |
|---|---|
| 一帧音频多大？ | 长 20ms，约 83 字节（3 头 + ~80 Opus） |
| 音频多久发一次？ | ~50 次/秒 |
| 数据怎么知道发到哪个 UUID？ | 靠 BLE 协议层的 handle，代码里只认特征对象 |
| UUID 能改吗？ | 16 位标准的不能；128 位自定义的能，但两端要一致 |
| UUID 表什么时候发？ | 只在连接后的服务发现时一次性交换，之后靠 handle |
| 手机 MTU 自动协商吗？ | Android 需 App 手动；iOS 自动；本固件不主动 |
| 照片和音频一起发吗？ | 同一 loop 里交替，音频优先（照片已删除） |
| OTA 怎么工作的？ | BLE 传 WiFi 密码 + URL → 设备自己连 WiFi 下载 → 写 OTA 分区 → 重启 |
| WiFi 平时开着吗？ | 不开，只有 OTA 时才开 |
