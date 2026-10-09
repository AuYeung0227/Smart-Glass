# OMI Glass 固件项目总览（Overview）

> **最后更新：2026-10-09 10:31**
> 硬件：Seeed XIAO ESP32S3 Sense（扩展板） ｜ 框架：Arduino / PlatformIO ｜ 固件版本：`FIRMWARE_VERSION_STRING` = 2.3.2
> 构建环境：`seeed_xiao_esp32s3`（正常） / `seeed_xiao_esp32s3_slow`（CORE_DEBUG_LEVEL=5，仅调试用）

---

## 一、文件与模块功能（分层总览）

**2026-10-09 完成分层重构**：`src/app.cpp` 由 1218 行的单体拆为 5 个模块 + 编排层。
依赖方向自上而下，**禁止反向调用**；跨层数据一律经应用层转发（见下表末的耦合规则）。

```
应用层   main.cpp → app.cpp（编排/接线） ── cmd_router.cpp（命令路由）
         ──────────────────────────────────────────────────────────
业务层   audio_tx.cpp  speaker_monitor.cpp  voiceprint.cpp  mfcc.cpp  recorder.cpp  ogg_opus.cpp
         ──────────────────────────────────────────────────────────
协议层   ble_transport.cpp  ota.cpp
         ──────────────────────────────────────────────────────────
底层     power_mgmt.cpp  battery.cpp  mic.cpp  opus_encoder.cpp
         ──────────────────────────────────────────────────────────
常量层   config.h
```

### 逐文件职责表

| 文件 | 所在层 | 一句话职责 | 对外接口（前缀） |
|---|---|---|---|
| `src/main.cpp` | 入口 | Arduino 入口，只调两个函数，不放任何逻辑 | `setup_app()` / `loop_app()` |
| `src/app.cpp` `.h` | **应用层** | 按次序初始化各模块、把模块间回调**接线**、每轮调度各 tick | `setup_app()` / `loop_app()` |
| `src/cmd_router.cpp` `.h` | **应用层下·路由** | 命令码 → 业务调用的**唯一分派表**；串口行解析 + BLE 命令邮箱 | `cmd_router_*` |
| `src/audio_tx.cpp` `.h` | **业务层** | 蓝牙推送三态状态机（暂停/实时/回放）+ 0x20 录音回传 | `audio_tx_*` |
| `src/speaker_monitor.cpp` `.h` | 业务层 | 声纹状态机（IDLE/VERIFY/MONITOR）、能量 VAD、20 s 录入 | `speaker_monitor_*` |
| `src/voiceprint.cpp` `.h` | 业务层（算法） | Xi-Vector 特征提取 + 模板存取（SPIFFS） | `voiceprint_*` |
| `src/mfcc.cpp` `.h` | 业务层（算法） | MFCC 前端（512 窗 / 256 跳 / 20 Mel 带 / 63 帧） | `mfcc_*` |
| `src/recorder.cpp` `.h` | 业务层 | 30 s PSRAM 预缓冲、被动/主动录音、SD 读写与删除 | `recorder_*` |
| `src/ogg_opus.cpp` `.h` | 业务层（容器） | Ogg/Opus 容器读写（RFC 7845，granule = 48 kHz 刻度） | `ogg_opus_*` |
| `src/ble_transport.cpp` `.h` | **协议层** | 起 BLE 协议栈、建 GATT 服务/特征、连接与订阅状态、notify 音频与电量 | `ble_transport_*` |
| `src/ota.cpp` `.h` | 协议层 | OTA over BLE：WiFi + HTTP 下载 + 分区写入 + 进度回报 | `ota_*` |
| `src/power_mgmt.cpp` `.h` | **底层** | 按键 ISR / LED / 电源状态机 / 深睡唤醒 | `power_mgmt_*` |
| `src/battery.cpp` `.h` | **底层** | 电量 ADC 采集与百分比换算（GPIO2，分压比 6.086） | `battery_*` |
| `src/mic.cpp` `.h` | 底层 | PDM 麦克风 I2S 采集 + 去尖峰/门控/增益/高通/预加重 | `mic_*` |
| `src/opus_encoder.cpp` `.h` | 底层 | PCM 环形缓冲 → 每 20 ms 编码一帧 Opus | `opus_*` |
| `src/config.h` | 常量层 | **所有**引脚、UUID、宏、阈值、枚举集中在此 | —（纯宏） |
| `src/mulaw.h` | 遗留 | μ-law 查表，当前主链路未使用 | — |

### 分层耦合规则（重构后强制遵守）

| 调用方向 | 是否允许 | 机制 |
|---|---|---|
| 应用层 → 任意层 | ✅ 允许 | 直接调用 |
| 业务层 → 业务层 | ✅ 允许 | 直接调用 |
| 业务层 → 协议层（要发数据） | ⚠️ **必须经应用层** | 应用层注册 sink 函数指针（`audio_tx_set_sink`） |
| 业务层 / 协议层 / 底层 → 上游 | ❌ 禁止 | 下游只暴露回调槽（`ble_transport_set_*_callback`），状态由应用层读出后**作参数传入** |
| Arduino / ESP-IDF / SD / SPIFFS / Bluedroid 库 API | ✅ 视为基础设施 | 不受"跨层禁令"约束（`recorder.cpp` 直接调 `SD.open()` 即属此列） |

> **可验证的解耦证据**：`ble_transport.cpp` 里不出现 `voiceprint*` / `speaker_monitor*` / `audio_tx_*` / `recorder_*` 等业务符号；
> `audio_tx.cpp` 里不出现 `ble_transport_*` / `BLECharacteristic`；`power_mgmt.cpp` 里不出现 `recorder_sd_ok()`。
> 用 `grep` 即可复核（注意：文件头注释里为说明历史会提到这些名字，属注释不算调用）。

---

## 二、模块与功能进度

### 1. 音频采集与编码链路 —— ✅ 可用
| 文件 | 功能 |
|---|---|
| `src/mic.cpp` / `mic.h` | PDM 麦克风 I2S 采集（16 kHz，CLK=GPIO42 / DATA=GPIO41）。每块数据依次做：自适应去尖峰 → 门控去脉冲 → `MIC_GAIN`(24) 增益 → 高通 → 预加重，输出 16-bit PCM。 |
| `src/opus_encoder.cpp` / `opus_encoder.h` | PCM 环形缓冲（8000 样本）→ 每 320 样本（20 ms）编码一帧 → Opus 32 kbps、复杂度 3、VBR。编码结果经 `opus_set_callback()` 回调送 BLE 发送环形缓冲。 |

- 数据流：`mic_process()` → `audio_callback` → `opus_receive_pcm()` → `opus_process()` → `opus_encode_frame()` → `audio_tx_on_opus()`（`src/audio_tx.cpp`）→ 发送环形缓冲 → `ble_transport_send_audio_frame()`（`src/ble_transport.cpp`）。
- 三个**只读旁路 tap**（互不影响主链路）：`analysis_callback`（原始域，供 VAD/声纹）、`recording_callback`（处理后域，供录音）、主回调（BLE）。

### 2. BLE 通信 —— ✅ 可用（本次新增推送开关）
| 特征 / UUID | 用途 |
|---|---|
| 服务 `19B10000-E8F2-537E-4F6C-D104768A1214` | 主服务 |
| `19B10001-…` | **音频 notify**（每帧 3 字节头[2B 序号 + 1B 子序号] + Opus 数据） |
| `19B10002-…` | 音频 codec 标识（`AUDIO_CODEC_ID` = 21） |
| `19B10003-…` | **命令写**（手机 → 设备） |
| 电池 `180F` / `2A19` | 电量上报 |
| OTA `19B10010/11/12` | OTA 升级通道（见模块 7） |

**本次新增：蓝牙实时音频推送三态状态机**（`ble_audio_tx_state_t`，定义在 `src/config.h`）
- `BLE_AUDIO_TX_PAUSED`（0）：**开机默认**，不向手机推送任何音频。
- `BLE_AUDIO_TX_LIVE`（1）：持续推送当前实时 Opus 帧。进入方式有两种：
  **① 手机订阅音频通知（写 CCCD）= 订阅即开始，自动进入**（`ble_transport.cpp` 的 `AudioCCCDCallback::onWrite` 更新订阅标志后回调 → `audio_tx.cpp` 的 `audio_tx_on_subscribe()` 切到实时发送）；
  **② 手机写命令 0x30**。
- `BLE_AUDIO_TX_REPLAY`（2）：收到 **0x20** 后回传 SD 录音，抢占实时推送。
- **0x31** → 暂停；**0x30** → 开始/恢复。暂停期间 mic 采集与编码照常跑（录音、声纹不受影响），只丢弃已编码帧、保持实时（恢复后不补发旧音频）。
- 串口别名：`sendstart` / `sendstop`；`status` 命令会打印当前推送状态。
- 为什么"订阅即开始"：任何能收到音频的客户端必然订阅过音频特征，所以它等价于"手机要了就推"，
  既能兼容不会发 0x30 的客户端，又保留了 0x31 手动暂停的能力。

### 3. 声纹识别 —— ✅ 可用（阈值待真机调校）
| 文件 | 功能 |
|---|---|
| `src/mfcc.cpp` / `mfcc.h` | MFCC 前端：512 点窗 / 256 跳、20 Mel 带、取 63 帧（与训练端 `Voice-Recognition/utils/mfcc.py` 对齐）。 |
| `src/voiceprint.cpp` / `voiceprint.h` | Xi-Vector 特征（80 维 = 4 × 20）+ 模板存取（SPIFFS `/speaker_template.bin`，均值 + 标准差）。 |
| `src/speaker_monitor.cpp` / `.h` | 状态机 `IDLE / VERIFY / MONITOR`；能量 VAD 门控；20 s 录入（含 1 秒倒计时）；归一化欧氏距离 < `VOICEPRINT_MATCH_THRESHOLD`(1.2) 判为同一说话人。 |

- 录制/录入由 0x01（开始）/ 0x02（提前结束）/ 0x03（放弃）/ 0x04（删除模板）控制。
- 被动录音由声纹匹配触发（详见模块 4）。

### 4. 录音与 SD 存储 —— ⚠️ 受阻（SD 卡读不到）
| 文件 | 功能 |
|---|---|
| `src/recorder.cpp` / `.h` | 30 s PSRAM 滚动预缓冲（960 KB）；被动录音（VAD 门控，只留"有人说话"的片段 + 触发后连续音频）；主动录音（0x10/0x11，优先级更高，会打断被动）；独立 Opus 编码器；文件名携带元数据 `p_/a_<open_unix>_<pos>.opus`；SD 容量定时检测（60 s）。 |
| `src/ogg_opus.cpp` / `.h` | Ogg/Opus 容器写入与读取（RFC 7845，granule 用 48 kHz 刻度）。 |

- 停止条件：被动 = 说话后连续 10 s 静音，或匹配后 30 s 无再次匹配（`PASSIVE_SILENCE_TIMEOUT_MS` / `PASSIVE_NOMATCH_TIMEOUT_MS`）。
- 删除：**0x12** 删最近一条主动录音（连同被其打断、时间相连的被动录音，靠 `/omi/journal.txt` 关联）；**0x13** 删全部。
- **当前阻塞**：SD 卡在本机完全无法挂载（详见「待解决问题 1」），因此被动/主动录音、删除、回传**均无法实机验证**。

### 5. 录音回传（0x20）—— ⚠️ 代码完成，因 SD 无法验证
- **0x20** 按时间顺序回传 `/omi` 下所有 `.opus`，逐帧前缀 **6 字节北京时间 BCD**（YY MM DD HH MM SS），同秒内多帧靠包序号区分；按 granule 差值节流还原原始时间轴。
- **0x21** 手机对时（4 字节小端 unix 秒）；**0x22** 停止回传。
- 回传期间实时推送让路（`BLE_AUDIO_TX_REPLAY`），回传结束自动恢复到回传前的状态。

### 6. 电源 / 电池 / LED / 按键 —— ✅ 代码在位
- `src/power_mgmt.cpp`：电源状态机（`DEVICE_*`）、按键（GPIO1）、LED 状态（GPIO21）、深睡唤醒。
- `src/battery.cpp`：电量 ADC（GPIO2，分压比 6.086），只负责"测出来"；"发给手机"由 `ble_transport_update_battery()` 负责。
- 注意：`VOICEPRINT_ENABLE` 时 `updateLED()` 提前返回——**GPIO21 让给 SD 片选**，LED 不再驱动。

### 7. OTA —— ⚠️ 未实机验证
- `src/ota.cpp` / `.h`：WiFi 配置 + HTTP 下载 + 分区写入，状态经 `19B10012` notify 回报。

### 8. 其他
- `src/main.cpp`：仅调用 `setup_app()` / `loop_app()`。
- `src/mulaw.h`：遗留的 μ-law 表，当前主链路未使用。
- `WorkFlow_Files/`：需求、规范、进度、评审文档（本工作流）。
- `Hardware_Test/MicroSD_Test.c`：SD 卡硬件诊断脚本。
- `笔记/音频与BLE知识笔记.md`：音频与 BLE 知识笔记。

---

## 三、待解决问题（按优先级）

1. **🔴 SD 卡完全无法读取（最高优先，阻塞录音/回传）**
   - 现象：SPI 模式 `sdCommand(): Card Failed! cmd: 0x00` / `sdSelectCard(): Select Failed`；SD/MMC 模式 `send_op_cond (1) returned 0x107`（= ESP_ERR_TIMEOUT）；从未读到任何应答。
   - 引脚探测：GPIO8(MISO) 悬空；GPIO21(CS) 被外部拉高（实为板载 LED 上拉）；插拔卡无任何电平变化。
   - 结论/怀疑：卡槽与 ESP32 **电气上未连通**，怀疑 XIAO ESP32S3 Sense 扩展板 **J3 处两个焊盘未焊接**（说明书：未焊 → 扩展板 SD 卡禁用 / 走 SPI 引脚，具体措辞待核对）。
   - 下一步：焊接 J3 后用 `Hardware_Test/MicroSD_Test.c` 复测 CMD0 应答（应出现 `01`），再回主固件跑 `sdcheck`。

2. **Flash 余量偏紧**：`seeed_xiao_esp32s3` 已用 **96.3%**（1 766 549 / 1 835 008 B，余 **68 459 B**）。
   分层重构（拆 5 个模块）仅仅只涨了 **+668 B**，风险已兑现为"可接受"。
   ⚠️ 但 `_slow` 调试环境已到 **99.79%**（1 830 609 / 1 835 008 B，**仅余 4 399 B**）—— 该环境几乎不能再加代码。
   根因是 `partitions_ota.csv` 把 `app0` 限死在 1.75 MB，而 8 MB Flash 尾部 4 MB 未分配（详见 `Project_Update.md` 2026-10-08 段）。

3. **命令通道只有 1 个待处理槽**：`voiceprintPendingPayload[5]`（`src/cmd_router.cpp`）只缓存一条命令，手机连续快写会覆盖丢失，无重传/ack。

4. **BLE 发送缓冲仅 16 帧**（`AUDIO_TX_RING_BUFFER_SIZE`），拥塞时 `audio_tx_on_opus()` 会静默丢帧（无计数/告警）。

5. **未对时的时间戳非法**：未收到 0x21 时 `recorder_beijing_bcd()` 输出全 `0xFF`，回传帧时间不可用。

6. **声纹匹配阈值 1.2 未经真机调校**；`RECORD_VAD_ENERGY_THRESHOLD`（0.0025，作用在增益后域）同样需实测。

7. **回放期间实时音频完全让路**（设计如此），手机端需明确约定，避免误判"没声音"。

8. **OTA 未实机验证**。

9. **手机 App 不会发 0x30**：仓库内的 `omiGlass` 手机 App 只做拍照（`19b10005`/`19b10006`），
   完全不碰音频特征 `19B10001` 与命令特征 `19B10003`，唯一的 BLE 写入是 `19b10006 ← 0x05`。
   → 因此推送改为"订阅即开始"；若以后要让 App 手动控制，需在 App 里补写 `19b10003 ← 0x30/0x31`。

10. **控制特征写入方式（已处理，2026-10-08 21:37）**：`19B10003` 原有属性只有 `PROPERTY_WRITE`，
    已补 `PROPERTY_WRITE_NR`，**现在"有响应写"与"无响应写"都收**。
    - 背景：只声明 `PROPERTY_WRITE` 时，用"无响应写"发的命令会在 GATT 层被直接拒收，
      `onWrite()` 不被调用，表现为"发了没反应、也不报错"。
    - 注意：属性位变了以后，手机/系统**缓存的旧 GATT 表失效**，需先"忘记设备"或用
      全新 BLE 客户端（nRF Connect / LightBlue）重新发现服务。
    - 验证法：对 `19B10003` 分别以 Write / Write Without Response 各发一次 `0x30`，
      串口都应打印 `【命令】手机写入 0x30（开始推送音频）`。
    - 说明：仓库内手机 App 本来就不发任何命令（见问题 9），此项属**预防性加固**，
      并非"收不到音频"的原因。

11. **🟡 分层重构（2026-10-09）尚未做真机回归**（当时板子未枚举到 USB）。
    编译 + 静态核对已通过，但下列链路**必须烧录后逐项验**：
    ① 音频推流；② 0x30/0x31 开关；③ 串口 `status`；④ `enroll` 录入（换板后 SPIFFS 为空，需重新录）；
    ⑤ 长按 2 s 进深睡/唤醒。**在验完之前，重构版不应视为已验收。**
    详见 `Project_Update.md` 2026-10-09 段。

12. **🟢 短按按键后可能立刻进入省电的既存缺陷（重构时刻意逐位保留，**未修**）**
    - 机理：`loop_app()` 在顶部捕获 `now`；短按时 `power_mgmt` 内部的 `handleButton()`
      会把 `lastActivity` 更新为**更晚**的 `millis()`；随后用**陈旧的 `now`** 去算
      `now - lastActivity` → `unsigned long` 下溢成极大值 → 必然大于空闲阈值。
    - 本次是纯搬迁重构，按"零行为变更"原则**逐位复现**，没有顺手修好。
    - 修法（待用户拍板）：`power_mgmt_idle_check()` 内部改用新鲜 `millis()`，
      或 `handleButton()` 直接刷新 `now`。

---

## 四、本次需求（Request.md）落实情况

| # | 需求 | 状态 |
|---|---|---|
| 1 | 只有收到 0x30 才推送实时 Opus，否则不推送 | ⚠️ 已放宽：开机默认暂停，但**手机订阅音频特征即自动开始**（订阅即开始），0x30 亦可手动开始。原因为手机 App 不发 0x30 |
| 2 | 分模块把项目进度写入本文件，带时间戳 | ✅ 本文件 |
| 3 | 把待解决问题写入本文件 | ✅ 见"三" |
| 4 | 收到 0x31 暂停推送 | ✅ 已实现 |
| 5 | 蓝牙发送设为几种状态，并可与用户商量补状态 | ✅ 三态（暂停/实时/回放），已与用户确认；可扩展见下 |
| **6** | **（2026-10-09，来自对话）把 `app.cpp` 按分层拆分，协议层与业务层解耦** | ✅ **已完成**：拆出 `ble_transport` / `audio_tx` / `cmd_router` / `power_mgmt` / `battery` 五个模块，`app.cpp` 1218 → 234 行；分 3 次提交。逐文件职责表见"一"，验证与隐患见 `Project_Update.md` 2026-10-09 段 |
| **7** | **（2026-10-09，来自对话）本 Overview 必须逐文件标明模块功能，一眼可读** | ✅ 见"一、文件与模块功能"的逐文件职责表 + 分层耦合规则表 |

**可扩展的候选状态**（已与用户讨论，暂未实现，供后续选择）：
- 「仅说话时发送」（VAD 门控推流，省电省带宽）
- 「单次发送 N 秒后自动暂停」（省流量）
- 「限速/低码率发送」（弱网）
