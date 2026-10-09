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


