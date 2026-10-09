# Project Update（改动记录）

> 规范要求：每次任务完成后，把本次修改内容 + 时间戳 + 遗留隐患写在本文件。

---

## 2026-10-08 20:08 — 蓝牙音频推送改为三态开关（0x30 / 0x31）

### 需求来源
`WorkFlow_Files/Request.md`：
1. 只有收到手机 **0x30** 才主动推送实时 Opus，否则不推；
4. 收到 **0x31** 暂停推送；
5. 蓝牙发送设成几种状态（已与用户确认按"暂停 / 实时 / 回放"三态实现）；
2、3. 项目进度与待解决问题写入 `Overview.md`。

### 做了什么

**1. `src/config.h`**
- 新增状态枚举 `ble_audio_tx_state_t { BLE_AUDIO_TX_PAUSED=0, BLE_AUDIO_TX_LIVE=1, BLE_AUDIO_TX_REPLAY=2 }`。
- 新增命令宏 `VP_CMD_AUDIO_SEND_START 0x30`、`VP_CMD_AUDIO_SEND_PAUSE 0x31`。

**2. `src/app.cpp`**
- 用状态变量 `bleAudioTxState`（volatile）+ `replayPrevState` **替换原 `static bool replayActive`**，把回放收编进同一状态机。
- 新增 4 个辅助函数（均带中文注释：功能/入参/出参/引用变量位置）：
  - `bleAudioTxStateName()` —— 状态 → 中文名（日志用）。
  - `bleAudioTxSetState()` —— 唯一的状态切换出口（写状态 + 打印）。
  - `bleAudioTxRequestLive()` / `bleAudioTxRequestPause()` —— 供 0x30/0x31 调用；若正在回放则先 `stopReplay()` 收尾。
- `startReplay()` / `stopReplay()` / `pumpReplay()`（自然结束处）改为读写新状态机，并记录/恢复 `replayPrevState`。
- `processAudioTx()`：改为**始终排空环形缓冲，但仅在 `BLE_AUDIO_TX_LIVE` 时才 `broadcastAudioPacket()` + `delay(1)`**。→ 暂停期帧被丢弃，恢复后从当下开始，无陈旧音频突发。
- `loop_app()` 闸门：`bleAudioTxState == REPLAY` 走 `pumpReplay()`，否则走 `processAudioTx()`。
- 命令分发 4 处：`voiceprintCmdName()` 加两个中文名；`voiceprintHandleCommand()` 加 `VP_CMD_AUDIO_SEND_START/PAUSE` 两个 case；`VP_CMD_STATUS` 打印追加当前推送状态；串口别名加 `sendstart` / `sendstop`。
- `setup_app()`：开机打印初始推送状态（`暂停`）并提示 0x30/0x31。

**3. `WorkFlow_Files/Overview.md`**（新建）：分 8 个模块写进度 + 8 条待解决问题 + 需求落实情况表，带时间戳。

### 没做 / 明确不改
- **不改动 `mic.cpp`、`opus_encoder.cpp`** 的采集与编码逻辑（调用点、参数、数据格式全未动）。
- 不改 GATT 特征 / UUID、不改录音文件格式、不改声纹参数。
- 没有新增"向手机回报推送状态"的 notify 特征（手机目前只能靠串口/日志知道状态）。

### 验证
- `pio run` 两个环境均 `SUCCESS`：
  - `seeed_xiao_esp32s3`：RAM 37.4%，Flash **96.2%**（1 765 829 / 1 835 008 B）
  - `seeed_xiao_esp32s3_slow`：RAM 37.4%，Flash **99.7%**（调试环境，加了很多日志）
- 未上机验证（需真机 + 手机 BLE 客户端）：默认不推流、0x30 开始、0x31 暂停、回放互斥。

### 遗留隐患
1. **Flash 余量偏紧（96.2%）**，`_slow` 调试环境 99.7%；后续加功能前须评估分区表 `partitions_ota.csv`。
2. 手机侧无状态回读通道，若写入命令丢失（BLE 写无 ack），手机与设备状态可能不一致。
3. 命令邮箱 `voiceprintPendingPayload[5]` 只有 1 个待处理槽（既有问题），快写会覆盖。
4. 暂停期仍持续编码并丢弃，CPU/功耗略高于"连采集一起停"的方案（已与用户确认选择"只停发送"）。
5. SD 卡仍无法读取（硬件问题，非本次改动），录音/回传无法验证 —— 见 `Overview.md` 待解决问题 1。

---

## 2026-10-08 21:08 — 【临时诊断】强制 BLE 始终推送音频

### 背景
用户反馈"蓝牙突然收不到数据"。判断为上一次改动（需求 1）的**预期行为**：
推送状态开机默认 `BLE_AUDIO_TX_PAUSED`，手机不发 `0x30` 就不推送。
为排除"重构写坏"的可能，做**临时诊断版**：强制始终推送。

### 做了什么（仅 `src/app.cpp` 两行）
- `app.cpp:321`：默认状态 `BLE_AUDIO_TX_PAUSED` → **`BLE_AUDIO_TX_LIVE`**（诊断期默认常开）。
- `app.cpp:771`：`processAudioTx()` 里的推送判断旁路 —— 原表达式注释保留，改为
  `const bool send = true;`（未注释整块 `if`，避免大括号配错）。

### 副作用 / 遗留
- 诊断期 **0x31 不会再真正停推**（判断被旁路）；0x30 也无实际作用。
- 其余逻辑（排空缓冲、拷贝、`broadcastAudioPacket`、`delay(1)`、回放分支）**未动**，
  `send=true` 时与改动前执行路径一致。
- 编译：`seeed_xiao_esp32s3` SUCCESS，Flash 96.2%。
- **待办**：用户上机测试后回报 —— 若恢复收数据则证明重构无问题；
  然后需决定是**恢复三态默认暂停**还是**永久默认常开**，并撤掉诊断注释。

---

## 2026-10-08 21:28 — 诊断结论 + 改为「订阅即开始」

### 诊断结论
- 诊断版（默认常开）恢复收数据 → 证明三态重构**逻辑无误**，"收不到"纯粹是**默认暂停**造成的。
- 排查发现：仓库内手机 App（`omiGlass/sources`）**只做拍照**（写入 `19b10006 ← 0x05`），
  从不引用音频特征 `19B10001`、命令特征 `19B10003`，更不会发 `0x30`。
  → 原需求"必须 0x30 才推"在本项目里没有任何客户端能满足。

### 做了什么（`src/app.cpp`，3 处）
1. **撤回诊断改动**：默认状态回 `BLE_AUDIO_TX_PAUSED`；`processAudioTx()` 恢复
   `const bool send = (bleAudioTxState == BLE_AUDIO_TX_LIVE);`（删掉硬编码 `true`）。
2. **新增「订阅即开始」**：`AudioCCCDCallback::onWrite()` 里，当手机开启音频通知
   （`value[0] & 0x01`）且当前为 `PAUSED` 时，自动 `bleAudioTxSetState(BLE_AUDIO_TX_LIVE)`；
   在 `REPLAY` 态则不打断（交给回放结束后的状态恢复逻辑）。
3. 开机日志更新为"手机订阅音频特征后自动开始；0x31 暂停，0x30 恢复"。

### 为什么这样最稳妥
任何"能收到音频"的客户端**必然**订阅过音频特征（notify 的前提），
所以"订阅即开始"对这些客户端等价于改动前的"无条件下发"，不会再有收不到的问题；
同时保留了 `0x31` 手动暂停 / `0x30` 恢复的能力。

### 验证
- `pio run -e seeed_xiao_esp32s3` SUCCESS（Flash 96.2%）。
- 待上机：手机订阅音频特征后应立刻收到音频；串口出现 `Audio notifications enabled`
  与 `【音频】推送状态 → 实时发送`。

### 遗留 / 待办
- 仓库内手机 App 不支持音频，本次未改 App（用户未要求）。若以后要 App 手动控制，
  需补 `19b10003 ← 0x30/0x31` 的写入。
- 控制特征 `19B10003` 只有 `PROPERTY_WRITE`（无 `WRITE_NR`）——**未改**（不在本次授权范围），
  但若手机端用"无响应写"发命令会被拒收，建议后续补 `BLECharacteristic::PROPERTY_WRITE_NR`。

---

## 2026-10-08 21:37 — 命令特征 19B10003 补上「无响应写」(WRITE_NR)

### 需求来源
用户确认上一段遗留项："好的你加一下吧"。同时提问"意思是原本手机发送，蓝牙收不到是吧"——
**不是**，已当面澄清（见下"澄清"）。

### 澄清（重要，避免误解）
- **不是**"手机在发、蓝牙收不到"。仓库内手机 App（`omiGlass/sources`）根本不碰 `19B10003`，
  其唯一 BLE 写入是拍照 `19b10006 ← 0x05`。
- 之前"收不到音频"的原因已查明并解决 = **开机默认暂停 + App 不发 0x30** → 已改「订阅即开始」，
  与本次改动**无关**。
- 本次属**预防性加固**：若将来客户端用「无响应写」发命令，固件会在 GATT 层拒收，
  `onWrite()` 不被调用，表现为"发了没反应、也不报错"。补 `WRITE_NR` 后两种写法都能收。

### 做了什么（`src/app.cpp`，1 行）
`configure_ble()` 内：
```cpp
// 原：BLECharacteristic::PROPERTY_WRITE
// 现：BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
```
并加了两行中文注释说明为什么。**回调无需改**：Bluedroid 的 `ESP_GATTS_WRITE_EVT`
无论 `need_rsp` 真假都会调 `onWrite()`，且 `getLength()`/`getValue()` 均已填好，
现有 `VoiceprintControlCallback::onWrite()`（`app.cpp:695`）读的正是这两个。

### 没做 / 明确不改
- **不改** `OTA_CONTROL_UUID`（仍 `PROPERTY_READ | PROPERTY_WRITE`）—— 用户未要求，按规范不动。
- **不改** 任何音频链路（mic / opus / TX 环形缓冲）、UUID、录音、声纹参数、`loop_app()` 闸门。
- 未新增特征、未改命令语义。

### 验证
- `pio run -e seeed_xiao_esp32s3` → **SUCCESS**；RAM 37.4%，Flash **96.2%**（1 765 881 / 1 835 008 B，与改前一致）。
- **未上机**。上机注意：改了 GATT 属性位后，手机/系统**缓存的旧 GATT 表会失效**，
  需先"忽略/忘记此设备"或换全新客户端（nRF Connect / LightBlue）重新发现服务，
  否则可能仍按旧属性工作。测试法见 `Overview.md` 待解决问题 10。

### 遗留隐患
1. **手机端需要重新配对/忘记设备**才能看到新的属性位（GATT 缓存），否则改动"看起来没生效"。
2. 属性位是**双刃剑**：开启 `WRITE_NR` 后，无响应写没有 ack，若命令丢失设备与手机状态可能不一致
   —— 与既有隐患"命令邮箱只有 1 个槽、无重传"叠加。
3. Flash 仍 96.2% 偏紧（既有隐患，本次未变）。

---

## 2026-10-08 22:50 — 新增笔记：OTA 与 Flash 分区

### 需求来源
用户要求：讲清 ①本项目的 OTA 逻辑 ②为什么 8MB Flash 要分区 ③为什么分区后有一块区是空的，
并写进 `笔记/` 目录下的新 md 文件。

### 做了什么
- **新建 `笔记/OTA与Flash分区笔记.md`**（仅文档，**未改任何代码**）。8 节内容：
  分区表逐项解读 + 内存映射图、A/B 双槽与 `otadata` 机制、两块"空区"的区分、
  OTA 完整流程/命令集/状态机、串口烧录 vs OTA 的对比、问题与排错。
- **`WorkFlow_Files/Project_Update.md`**：追加本段。

### 调查得到的两个**事实纠正**（重要）
1. **8MB Flash 只切了 4MB**。`partitions_ota.csv` 最后一个分区 `spiffs` 结束于 `0x400000`，
   而 Flash 到 `0x800000` —— **尾部 4MB 未被任何分区定义**（已用编译产物 `partitions.bin` 反解确认）。
   → 这正是 Flash 占用 96.2% 的根因：`app0` 被限制在 1.75MB，旁边 4MB 闲置。
   用户"分成两个区"的印象，可能指 `app0`/`app1` 双槽，也可能指"前4MB用/后4MB空"，笔记里两者都解释了。
2. **双槽 ≠ 自动回滚**。本项目用 `Update.end(true)` 直接切换启动分区，
   **没有** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 相关处理，源码里也搜不到
   `esp_ota_mark_app_valid_cancel_rollback()`。所以"新固件能写进去但启动崩溃"会**卡死**，
   只能串口重刷 —— 这一点旧笔记（`音频与BLE知识笔记.md` 第 10 节）写的"天然可回滚"**只对下载失败成立**，需修正。

### 没做 / 明确不改
- **未改** `partitions_ota.csv`、`src/ota.cpp`、`platformio.ini` 等任何文件。
  扩分区（7.1 节方案）与 OTA 特征补 `WRITE_NR`（7.3 节）都只是**建议**，等用户决定。
- 未改动 `笔记/音频与BLE知识笔记.md`（旧笔记第 4、10 节与本笔记有重叠，
  且第 10 节"天然可回滚"表述不准确 —— 是否回改由用户决定）。

### 隐患 / 需要用户拍板
1. **扩分区必须整片重刷**（`erase` + `upload`），且 `spiffs` 里的声纹模板会丢失，需重新录入。
   在板子还没修好、USB 都连不上的当下**不建议做**。
2. `src/ota.cpp:269` 的 `setInsecure()` 跳过 HTTPS 证书校验，是 OTA 链路最大的安全口子（旧问题，本次仅记录）。
3. OTA 控制特征 `19B10011` 同样只有 `PROPERTY_WRITE`，缺 `PROPERTY_WRITE_NR`（与 `19B10003` 同款问题）。

---

## 2026-10-09 10:31 — `app.cpp` 分层重构（方案 B）：单体 1218 行 → 5 模块 + 编排层

### 需求来源
用户在对话中提出：`src/app.cpp` 过大，希望按**协议层 / 业务层 / 底层**拆分为多个模块，
以利于后期管理、整合、解耦与维护，并要求同步更新 `Overview.md`（含**逐文件功能表**）。
经用户从 3 个备选方案中选定 **方案 B（分层拆分）**，并确认：分 3 次提交、跨文件接口加模块前缀。
（`WorkFlow_Files/Request.md` 当时为**空文件**，需求来自对话，已补记进该文件。）

### 做了什么 —— 三次提交，纯搬迁，算法一行未改

| 提交 | 内容 | Flash 变化 | app.cpp |
|---|---|---|---|
| `f362931` | 拆出 `power_mgmt`（按键/LED/电源/深睡）+ `battery`（电量 ADC） | 1 765 881 → 1 766 089 B (**+208 B**) | 1218 → 1000 行 |
| `24539cb` | 拆出 `ble_transport`（协议层）+ `audio_tx`（业务层） | → 1 766 497 B (**+408 B**) | → 439 行 |
| `cf6b640` | 拆出 `cmd_router`（命令路由）；`app.cpp` 收尾 | → 1 766 549 B (**+52 B**) | → **234 行** |

**新增文件（均含文件头作用说明 + 逐函数"功能/入参/出参/引用变量定义位置"注释 + 逐行中文备注）**：

| 新文件 | 层 | 搬入的原 `app.cpp` 行号 |
|---|---|---|
| `src/power_mgmt.{h,cpp}` | 底层 | :103-287（按键 ISR / LED / 电源状态机 / 深睡） |
| `src/battery.{h,cpp}` | 底层 | :895-955（电量 ADC）；:1108-1109（ADC 配置） |
| `src/ble_transport.{h,cpp}` | 协议层 | :42-84、:693-712、:809-890、:957-967、:972-1076（GATT / UUID / 特征 / 5 个回调类） |
| `src/audio_tx.{h,cpp}` | 业务层 | :78-81、:323-511、:714-743、:763-807（推送三态 + 0x20 回传 + 发送环形缓冲） |
| `src/cmd_router.{h,cpp}` | 路由层 | :79-255、:272-283（命令分派表 / BLE 命令邮箱 / 串口行解析） |

**解耦的核心改动**：协议层回调类里**不再出现任何业务函数名**，一律调用由 `app.cpp` 注册进来的回调槽
（`s_rxFn` / `s_connFn` / `s_subFn`）；业务层 `audio_tx` **不 include 协议层**，
发送改走应用层注册的 `s_sink` 函数指针；"BLE 是否可发""SD 是否挂载"等状态
一律由 `app.cpp` 读出后**作为参数传入**底层/业务层。

**已用 `grep` 复核的解耦证据**（命中项全部落在注释里，无一处真实调用）：
- `ble_transport.cpp` 不含 `voiceprint*` / `speaker_monitor*` / `audio_tx_*` / `recorder_*` / `battery_percentage` / `opus_get_codec_id`
- `audio_tx.cpp` 不含 `ble_transport_*` / `BLECharacteristic` / `broadcastAudioPacket`
- `power_mgmt.cpp` 不含 `recorder_sd_ok()` / `ble_transport_*`
- `cmd_router.cpp` 不含 `ble_transport_*` / `BLECharacteristic`

### 与原计划（方案 B）的**有意偏离**（3 处，均已在本文件说明理由）

1. **`audio_tx` 与 `ble_transport` 合并为同一次提交**（原计划分属步骤 2、3）。
   理由：`broadcastAudioPacket()` 从 `app.cpp` 搬到协议层、同时 `audio_tx` 改为走 sink 指针，
   两者必须**同时切换**，否则中间态编译不过。
2. **`onBleConnectionChanged()` 是新增函数**（原 `ServerHandler::onConnect` 里的两件事搬到了 `app.cpp`）。
   理由：那两件事分属底层（`power_mgmt_note_activity`）与协议层（电量上报），
   只有应用层有资格同时认识两者，正好体现"经应用层协调"。
3. **`updateBatteryService()` 删除**，其"仅连接时 notify"的判断并入 `ble_transport_update_battery(pct)`。
   理由：函数原先三合一（读全局电量 + 写特征 + 上报），拆分后电量改由调用方传入。

**另有一处必要的行为等价调整**：`ble_transport.cpp` 里的共享发送缓冲
`audio_packet_buffer` 由 **163 B**（`OPUS_OUTPUT_MAX_BYTES + 3`）扩到 **169 B**（`REPLAY_PACKET_MAX_BYTES`）。
因为回传路径统一走同一个发送函数，而它的负载多 6 字节 BCD 时间戳
（重构前回传自己另开了一个 169 B 的局部数组绕开，实时路径才用那 163 B）。已加长度上界判断防越界。

### 逐位复现、**刻意未修**的既存缺陷

`loop_app()` 顶部捕获的陈旧 `now` 被显式作为参数传进 `power_mgmt_idle_check(now, ...)`：
短按按键后 `lastActivity` 会大于 `now`，`now - lastActivity` 在 `unsigned long` 下溢成极大值
→ **立即进入省电模式**。这是重构前就存在的缺陷，本次按"零行为变更"原则逐位复现，
`src/power_mgmt.cpp` 内有醒目注释说明，**未顺手修好**。修法待用户拍板。

### 验证结果

| 关卡 | 结果 |
|---|---|
| 编译 `seeed_xiao_esp32s3` | ✅ SUCCESS，**1 766 549 / 1 835 008 B = 96.3%**（余 68 459 B） |
| 编译 `seeed_xiao_esp32s3_slow` | ✅ SUCCESS，但 **1 830 609 / 1 835 008 B = 99.79%**，⚠️ 仅余 **4 399 B** |
| RAM | 122 456 / 327 680 B = 37.4%，**与重构前完全一致** |
| 静态解耦核对 | ✅ 通过（见上"解耦证据"） |
| **真机回归** | ❌ **未做** —— 检查时 `pio device list` 只有蓝牙虚拟串口（COM4/COM9），板子未枚举到 USB |

### 没做 / 明确不改
- **未改**任何算法：`mic` / `opus_encoder` / `mfcc` / `voiceprint` / `speaker_monitor` / `recorder` / `ogg_opus` / `ota`
  一行未动 —— 满足"必须保留麦克风、蓝牙、声纹识别写入部分"与"保留 MFCC 及声纹相关全部代码"的要求。
- **未删**只写不读的既存符号（`deviceActive` / `deviceState` / `buttonPressed` / `blinkLED`），原样保留。
- **未动** `src/ota.cpp:38` 那个同名但从未实例化的 `OTAControlCallback` 类（现状无冲突）。
- **未改** `platformio.ini`、`partitions_ota.csv`。
- **未做**真机回归、未做 SD 卡相关链路验证（板子未连 + SD 硬件本就未修好）。

### 隐患 / 需要用户拍板

1. **🔴 `_slow` 调试环境只剩 4 399 B**。该环境（`CORE_DEBUG_LEVEL=5`）后续几乎不能再加任何代码。
   若要在该环境下继续开发，需先处理 Flash 分区（`partitions_ota.csv` 只切了 4 MB，8 MB 芯片尾部 4 MB 未分配）。
2. **🟡 重构版尚未真机验收**。回调接线漏接一个**不会编译报错**（因为走的是函数指针，空指针只是静默不响应）。
   烧录后请按 `Overview.md` 待解决问题 11 的清单逐项验；**验完之前不应视为已交付**。
3. **🟡 未对时/未修模板等既有问题依然存在**，本次未触碰。
4. **`WorkFlow_Files/Request.md` 原先为空**，我已把本次需求要点补记进去（带"由 Claude 补记"标注）。
   若你本来打算用它记别的需求，直接覆盖即可 —— 该文件无历史内容被删。



