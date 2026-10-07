# omiGlass 音频链路 · 知识点分类整理

> 按**知识类别**组织（区别于按数据流排序的 `omiGlass-Audio-Pipeline-Notes.md`）
> 每个知识点都附着 `omi-main/omiGlass/firmware/` 里的**实际代码**
> 共 7 大类 · 43 个问题

## 目录

| 类别 | 主题 | 知识点数 |
|---|---|---|
| [一](#一信号与采样基础) | 信号与采样基础 | 6 |
| [二](#二i2s-与-dma-硬件层) | I2S 与 DMA 硬件层 | 9 |
| [三](#三音频处理与增益) | 音频处理与增益 | 1 |
| [四](#四环形缓冲) | 环形缓冲 | 6 |
| [五](#五opus-编码与码率) | Opus 编码与码率 | 8 |
| [六](#六软件架构回调与并发) | 软件架构：回调与并发 | 4 |
| [七](#七ble-发送与协议) | BLE 发送与协议 | 1 |

---

## 数据流全景（对照用）

```
麦克风 ─PDM─▶ I2S外设 ─PCM─▶ DMA环 ─▶ i2s_read ─▶ 增益 ─▶ onMicData
   │                                                          │
   │                                                          ▼
   │                                                    PCM环形缓冲
   │                                                          │
   │                                                    opus_process
   │                                                          │
   │                                                          ▼
   └──────────────────────────  手机 ◀─ BLE ◀─ 发送环 ◀─ onOpusEncoded ◀─ Opus编码
```

---

# 一、信号与采样基础

## 1.1 PDM 是什么？

**知识点**：Pulse Density Modulation（脉冲密度调制）——用「1 的疏密」表示幅度，是数字 MEMS 麦克风的默认输出格式。幅度不在单比特的值里，而在一段比特的密度里。

**代码**——PDM 模式就体现在 `mode` 这一个标志位上：

`mic.cpp:45-57`

```c
i2s_config_t i2s_config = {
    .mode = (i2s_mode_t) (I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),  // ← PDM 模式
    .sample_rate = MIC_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
};
```

引脚也印证了 PDM 的双线特征——`ws_io_num` 被复用成 PDM 时钟：

`mic.cpp:60-65`

```c
i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_PIN_NO_CHANGE,
    .ws_io_num = MIC_CLK_PIN,   // PDM CLK
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_DATA_PIN, // PDM DATA
};
```

| 波形位置 | PDM 位流（16 bit 窗口） | 1 的比例 | 代表幅度 |
|---|---|---|---|
| 波峰 | `1111111111111111` | 100% | 正向满量程 |
| 上升 | `1111111111110000` | 75% | +50% |
| 零点 | `1111111100000000` | 50% | 0（静音） |
| 波谷 | `0000000000000000` | 0% | 负向满量程 |

**要点**：PDM 是「快而粗」（1 bit 高速），PCM 是「慢而精」（16 bit 低速）。转换在硬件里完成。

---

## 1.2 PDM 的 1 bit 怎么变成 16 bit？

**知识点**：在时间窗口内**数密度**。把 1 记 +1、0 记 −1，对窗口内求和。硬件里是**抽取滤波器**（低通 + 降采样），常用 CIC（级联积分梳状）实现。

**代码**——`bits_per_sample` 就是「抽取后的输出位宽」：

`mic.cpp:48`

```c
.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,   // ← 1-bit 进，16-bit 出
```

抽出来的结果直接就是 `int16_t`，代码后面全按 16 位处理：

`mic.cpp:121-125`

```c
i2s_read(I2S_PORT, i2s_read_buffer, MIC_BUFFER_SAMPLES * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(20));

if (err == ESP_OK && bytes_read > 0) {
    size_t samples_read = bytes_read / sizeof(int16_t);   // ← 已经是可以直接用的 PCM 了
```

| 64 个 bit 的情况 | 1 的个数 | 求和 | 还原幅度 |
|---|---|---|---|
| 全是 1 | 64 | +64 | +满量程 |
| 一半 1 一半 0 | 32 | 0 | 0 |
| 全是 0 | 0 | −64 | −满量程 |

**要点**：`i2s_read` 拿到的已经是 16-bit PCM，**PDM 从不出现在软件层**。

---

## 1.3 采样率：输入 vs 输出不一样

**知识点**：PDM 输入的速率单位是 **bit/s**（~MHz），抽取输出的单位是 **样本/s**（16 kHz）。两者由**过采样率 OSR** 关联。

**代码**——只有「输出侧」的 16 kHz 出现在配置里：

`config.h:129-130`

```c
#define MIC_SAMPLE_RATE 16000          // 16kHz sample rate
#define MIC_BUFFER_SAMPLES 1600        // 100ms buffer (16000 * 0.1)
```

**注意 1600 这个数就是 `16000 × 0.1` 算出来的**——采样率决定「100ms 等于多少样本」。

**要点**：`MIC_SAMPLE_RATE` 是**抽取之后**的 PCM 采样率；PDM 时钟由外设按 OSR 反推，代码里不用你算。

---

## 1.4 为什么 PDM 流要乘以 64？

**知识点**：16 kHz 是**你要求的规格**，×64 是**硬件满足它付出的代价**。1 bit 表示不了幅度（只有 ~6 dB 信噪比），所以用「过采样 + 噪声整形」换精度。

**为什么必须两个一起用**：

| 做法（64× 过采样） | 有效精度 |
|---|---|
| 只有过采样 | ~4 bit ✗（要凑 16 bit 需 2¹⁶ 倍过采样，不现实） |
| 一阶噪声整形 | ~9 bit |
| **二阶噪声整形**（主流 MEMS） | **~14 bit** ✓ |

**要点**：交易本质是「**比特率下降（1.024 Mbit/s → 256 kbit/s），但每样本位数上升（1 bit → 16 bit）**」——用快而粗换慢而精。

> ⚠️ **纠错**：ESP32-S3 的硬件下采样器有自己档位（ESP-IDF `i2s_pdm_dsr_t` 的 `DSR_8S`/`DSR_16S`），PDM 时钟是采样率的**十几到几十倍**，**不是固定 64×**。64× 是通用 PDM 麦克风的典型值。

---

## 1.5 16 kHz 是 16000 个样本，不是 16000 个包

**知识点**：三个层次要分清——**样本**（sample）→ **帧**（frame）→ **包**（packet）。

**代码**——三个常量正好对应这三层：

`config.h:129` / `config.h:138` / `config.h:146`

```c
#define MIC_SAMPLE_RATE 16000          // 采样率：每秒 16000 个【样本】
#define OPUS_FRAME_SAMPLES 320         // 帧长：每 320 个样本凑一【帧】(20ms)
#define AUDIO_TX_RING_BUFFER_SIZE 16   // 发送环：缓冲 16 个【包】
```

**换算链**：

```
16000 样本/秒 ÷ 320 样本/帧 = 50 帧/秒
50 帧/秒 → Opus 压缩 → 50 个 BLE 包/秒   ← 不是 16000 个包！
```

**要点**：采样率 ≠ 发送率。中间隔着「320 分组」和「Opus 压缩」两道工序。

---

## 1.6 `bits_per_sample` 是什么？

**知识点**：它说的是**抽取之后的输出位宽**（16 位），不是 PDM 的 1 位。它把整条链路的类型钉死成 `int16_t`。

**代码**——三处必须一致：

`mic.cpp:48`（I2S 输出位宽）

```c
.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
```

`opus_encoder.cpp:76`（告诉 Opus 输入位深）

```c
opus_encoder_ctl(encoder, OPUS_SET_LSB_DEPTH(16));
```

`mic.cpp:132-135`（限幅边界就是 int16 的范围）

```c
if (sample > 32767)  sample = 32767;
if (sample < -32768) sample = -32768;
```

**要点**：改成 32 位的话，这三处（以及缓冲分配 `sizeof(int16_t)`）**必须一起改**，否则音频变噪声。

---

# 二、I2S 与 DMA 硬件层

## 2.1 为什么 16 kHz 要配 100 ms 缓冲？

**知识点**：先定「时长」，再乘以采样率得「样本数」。100 ms 是配合主循环抖动、DMA 深度、Opus 帧长三方折中的结果。

**代码**——`1600` 就是 `16000 × 0.1`：

`config.h:129-132`

```c
#define MIC_SAMPLE_RATE 16000          // 16kHz sample rate
#define MIC_BUFFER_SAMPLES 1600        // 100ms buffer (16000 * 0.1)
#define MIC_GAIN 2                     // Microphone gain multiplier
#define AUDIO_RING_BUFFER_SAMPLES 8000 // 500ms of audio data
```

**三个约束**：

| 约束 | 数值 | 关系 |
|---|---|---|
| DMA 硬件深度 | 8 × 256 = 2048 样本 = **128 ms** | 100 ms 要 ≤ 它 |
| Opus 帧长 | 320 样本 = 20 ms | 100 ms 要能被 20 整除 |
| 主循环周期 | ~25~30 ms | 100 ms 要远大于它 |

**要点**：**100 ms 是选定的延迟目标，1600 是它的算术结果**。

---

## 2.2 I2S 三个配置项：`communication_format` / `dma_buf_count` / `dma_buf_len`

**知识点**：前一个是线路协议格式（PDM 下基本不起作用），后两个共同定义**硬件环形缓冲的容量**。

**代码**：

`mic.cpp:50-53`

```c
.communication_format = I2S_COMM_FORMAT_STAND_I2S,   // ① 线路时序格式
.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
.dma_buf_count = 8,                                  // ② 环分几块
.dma_buf_len = 256,                                  // ③ 每块多少样本
```

**容量 = ② × ③**：

$$8 \times 256 = 2048 \text{ 样本} = \frac{2048}{16000} \approx 128 \text{ ms}$$

**要点**：①在 PDM 模式下是**摆设**——PDM 不用 I2S 那套「WS 分左右声道 + BCK 逐位对齐」的框架，只有一条数据线。

---

## 2.3 DMA 的作用

**知识点**：硬件搬运工，**不占用 CPU** 地把外设数据搬进内存。让 CPU 和采样时钟**解耦**。

**代码**——`i2s_driver_install` 就是分配 DMA 的地方：

`mic.cpp:68`

```c
esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
//                                                          ↑ ↑
//                                          queue_size=0 ─┘ └─ 无事件队列 → 纯轮询
```

清空：

`mic.cpp:82`

```c
i2s_zero_dma_buffer(I2S_PORT);
```

**128 ms 余量的意义**：

> CPU 因照片 BLE 上传等卡住时，DMA 照填；CPU 回来一次性排空。**卡顿 < 128 ms 就一帧不丢**，超过就绕回覆盖最旧数据 → 丢样。

**要点**：这 128ms 是**给 CPU 抖动买的保险**，衡量单位是「能扛住多长时间的阻塞」。

---

## 2.4 DMA 是「双缓冲队列」吗？

**知识点**：**不是。**`dma_buf_count = 8` 是 **8 缓冲环形队列**。双缓冲（N=2 乒乓）是它的特例。

**代码**：

`mic.cpp:52-53`

```c
.dma_buf_count = 8,     // ← 8 个，不是 2
.dma_buf_len = 256,
```

**同一个「生产者/消费者解耦」模式在项目里重复了三次**：

| 层 | 位置 | 容量 | 解耦谁和谁 |
|---|---|---|---|
| ① DMA 环 | `mic.cpp:52` | 8 × 256 = 2048 样本 / 128 ms | 麦克风硬件 ↔ CPU |
| ② PCM 环 | `opus_encoder.cpp:13,31` | 8000 样本 / 500 ms | 麦克风回调 ↔ Opus 编码器 |
| ③ 发送环 | `app.cpp:83` | 16 × 162 字节 | Opus 编码器 ↔ BLE 发送 |

**要点**：**越往下游容量越大**，因为卡顿来源越不可控。

---

## 2.5 DMA 相关代码在哪？

**知识点**：DMA 环**本身不在项目里**——是 ESP-IDF 驱动 `i2s_driver_install()` 时 malloc 的。项目只做四件事。

**代码**：

| 步骤 | 位置 | 代码 |
|---|---|---|
| 配置 | `mic.cpp:52-53` | `.dma_buf_count = 8, .dma_buf_len = 256` |
| 安装（真正分配） | `mic.cpp:68` | `i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL)` |
| 清空 | `mic.cpp:82` | `i2s_zero_dma_buffer(I2S_PORT)` |
| **读出** | **`mic.cpp:122`** | `i2s_read(...)` |

**读出的完整代码**：

`mic.cpp:114-144`

```c
void mic_process()
{
    if (!mic_running || i2s_read_buffer == nullptr) {
        return;
    }

    size_t bytes_read = 0;
    esp_err_t err =
        i2s_read(I2S_PORT, i2s_read_buffer, MIC_BUFFER_SAMPLES * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(20));

    if (err == ESP_OK && bytes_read > 0) {
        size_t samples_read = bytes_read / sizeof(int16_t);

        if (MIC_GAIN != 1) {
            for (size_t i = 0; i < samples_read; i++) {
                int32_t sample = (int32_t) i2s_read_buffer[i] * MIC_GAIN;
                if (sample > 32767)  sample = 32767;
                if (sample < -32768) sample = -32768;
                i2s_read_buffer[i] = (int16_t) sample;
            }
        }

        if (audio_callback != nullptr) {
            audio_callback(i2s_read_buffer, samples_read);
        }
    }
}
```

**要点**：**`i2s_read` 那一行就是把数据从 DMA 环取出来的唯一地方。**

---

## 2.6 DMA 搬的是 PDM 还是 PCM？

**知识点**：**搬的是已经处理好的 16-bit PCM。**转换在 DMA **之前**，由 I2S 外设的硬件 sinc 滤波器完成。

**证据 1**——ESP32-S3 TRM 有寄存器 `I2S_RX_PDM_SINC_DSR_16_EN`，文档原文「配置 **PDM RX 滤波器组** 模块的**下采样率**」。

**证据 2**——`dma_buf_len` 的单位是**样本**，代码里也是这么用的：

`mic.cpp:122`

```c
i2s_read(I2S_PORT, i2s_read_buffer, MIC_BUFFER_SAMPLES * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(20));
//                                 ↑ MIC_BUFFER_SAMPLES = 1600【样本】= 3200 字节，不是 1600 个 bit
```

**`* sizeof(int16_t)` 这个乘法本身就说明问题**——代码认为 DMA 里装的是 16 位样本，不是 1 位流。

**为什么 DMA 不可能搬原始 PDM**：

1. DMA 的搬运单位是**字**不是**位**
2. PDM 只存在于引脚和移位寄存器里，**不在内存总线上**
3. 让 CPU 做抽取滤波是**巨大浪费**

**要点**：**PCM 是软件世界的通用货币，PDM 不是。**转换放在硬件边界的意义是让 DMA 之后的一切只说一种语言。

---

## 2.7 `i2s_read` 是一次把 8×256 全读出来吗？

**知识点**：**不是。**一次最多读 **1600 个样本**（请求上限），实际通常只有几百个。

**代码**——注意三个数字的角色不同：

`mic.cpp:121-122`

```c
i2s_read(I2S_PORT,
         i2s_read_buffer,
         MIC_BUFFER_SAMPLES * sizeof(int16_t),   // ← 请求上限：1600 样本（不是 2048！）
         &bytes_read,                            // ← 输出：实际读了多少
         pdMS_TO_TICKS(20));                     // ← 最多等 20ms
```

**三个数字的关系**：

```
DMA 总容量   2048 样本 (128ms)   ← 【库存】，不在读取参数里
请求上限     1600 样本 (100ms)   ← 一次最多读多少
20ms 实际产出 320 样本            ← 通常实际读到多少
```

**代码证据**——用的是 `bytes_read`（实际值）而不是硬编码 1600：

`mic.cpp:125`

```c
size_t samples_read = bytes_read / sizeof(int16_t);   // ← 值是可变的
```

**要点**：**DMA 是「持续旋转的环」，不是「填满再倒空」**。`i2s_read` 只推进读指针，写指针完全不受影响。

---

## 2.8 `i2s_read` 的超时是什么情况？

**知识点**：它是**带截止时间的阻塞调用**。超时**不是出错**，而是「有多少给多少」。

**代码**：

`mic.cpp:122`

```c
pdMS_TO_TICKS(20)     // Arduino-ESP32 的 CONFIG_FREERTOS_HZ=1000 → 精确 20ms
```

**三种结果**：

| 情况 | 耗时 | bytes_read |
|---|---|---|
| 数据早就在 DMA 里等着 | ~0 | 3200（满额） |
| 等到攒满 | < 20ms | 3200（满额） |
| **20ms 到了还没攒满** | **= 20ms** | **部分** ← **常态** |

**为什么超时是常态**：

$$20 \text{ ms} \times 16 \text{ 样本/ms} = 320 \text{ 个样本} \ll 1600$$

**攒满 1600 要 100ms，超时只有 20ms** —— 超时永远先到。

**代码怎么处理**——`err == ESP_OK` 挡住「一个字节都没来」的情况：

`mic.cpp:124`

```c
if (err == ESP_OK && bytes_read > 0) {
```

**要点**：等待期间任务是**挂起（sleep）**的，CPU 让给 NimBLE 协议栈等任务——所以这 20ms 不浪费。

---

## 2.9 DMA 会不会「放满」？

**知识点**：「放满」有两个意思——① DMA 里有没有数据（**永远有**）；② 会不会溢出覆盖未读数据（**正常不会**）。

**代码**——关键在于实际水位远低于容量：

`mic.cpp:122` 的 20ms 超时决定了每次只取走几百个，**软件在持续小批量排空**。

**真实水位图**：

```
水位
 480 ┤      ╱╲          ╱╲          ╱╲
     │     ╱  ╲        ╱  ╲        ╱  ╲     ← 每轮排空一次
   0 ┤────╱────╲──────╱────╲──────╱────╲──
2048 ┼─────────────────────────────────────  ← 容量线，差得远
```

**要点**：不会溢出的原因**不是「1600 < 2048」这个静态算术**，而是**软件持续排空**。2048 那份余量的真正作用，是容忍最长 **128 ms** 的 CPU 卡顿。

---

## 2.10 DMA 取数据的速率是多少？

**知识点**：**三个不同的速率**别混。DMA 不是「定时取」，而是 **FIFO 水位触发的突发搬运**。

**三个速率**：

| 阶段 | 速率 | 含义 |
|---|---|---|
| ① PDM 流 → I2S 外设 | PDM 时钟（~MHz） | 每个时钟沿进 1 bit |
| ② 抽取输出 → DMA 环 | **16 kHz** | **每 62.5 µs 产生 1 个样本** |
| ③ DMA 环 → `i2s_read_buffer` | **~25~30 ms 一次** | 每次几百个样本 |

**代码**——③ 的速率由主循环决定：

`app.cpp:1003-1009`

```c
// Adaptive delays for power saving (gentle optimization)
if (photoDataUploading || audioSubscribed) {
    delay(5); // Fast during upload or audio streaming
} else if (powerSaveMode) {
    delay(50);
} else {
    delay(50);
}
```

`app.cpp:883-886`

```c
if (audioEnabled && mic_is_running()) {
    mic_process();      // ← 每轮主循环调用一次（≈25~30ms）
    opus_process();
}
```

**要点**：
- **DMA 不写 `i2s_read_buffer`** —— 它写硬件环；`i2s_read_buffer` 是 `i2s_read()` **拷**过来的目的地
- **不是 100ms 一次** —— 100ms 只是「1600 样本」对应的时间上限，实际永远达不到

---

## 2.11 DMA 参数可以改成 `count=4, len=512` 吗？

**知识点**：**可以。**总量不变、抗抖动能力不变，只是**粒度变粗**。

**代码**：

`mic.cpp:52-53`

```c
.dma_buf_count = 8,     // 改成 4
.dma_buf_len = 256,     // 改成 512 —— 总量仍是 2048 样本
```

**能过驱动检查**——ESP-IDF 的约束是 `dma_buf_len ∈ [8, 1024]`：

```c
I2S_CHECK((i2s_config->dma_buf_len >= 8 && i2s_config->dma_buf_len <= 1024),
          "I2S buffer length at most 1024 and more than 8", ...);
```

| 参数 | 8 × 256（当前） | 4 × 512 |
|---|---|---|
| 总容量 | 2048 样本 | **2048 样本** ✅ |
| 缓冲时间 | 128 ms | **128 ms** ✅ |
| 描述符数 | 8 | 4 |
| 粒度 | 细 | **粗** |

**要点**：硬件上也没问题（ESP32 DMA 描述符单块最大 4095 字节，1024 字节远在其内）。但 `8 × 256` 是常见安全默认值，**没有实测需求不必改**。

---

# 三、音频处理与增益

## 3.1 `MIC_GAIN` 增益放大的过程

**知识点**：软件数字增益，把每个样本乘一个常数。**必须用 `int32_t` 中间变量**，否则溢出会把正峰值**反转成负峰值**。

**代码**——完整过程：

`config.h:131`

```c
#define MIC_GAIN 2                     // Microphone gain multiplier
```

`mic.cpp:127-138`

```c
if (MIC_GAIN != 1) {
    for (size_t i = 0; i < samples_read; i++) {
        int32_t sample = (int32_t) i2s_read_buffer[i] * MIC_GAIN;   // ① 转 int32 防溢出
        if (sample > 32767)  sample = 32767;                        // ② 正向限幅
        if (sample < -32768) sample = -32768;                       // ③ 负向限幅
        i2s_read_buffer[i] = (int16_t) sample;                      // ④ 写回原缓冲（就地修改）
    }
}
```

### ⭐ 为什么必须转 `int32_t`

```
sample = 20000（一个很响的声音）

❌ 直接 int16_t 相乘：
   20000 × 2 = 40000，但 int16_t 最大 32767
   40000 按 16 位截断 = 40000 − 65536 = −25536
   ↑ 【正的大声音变成负的大声音】—— 听感是剧烈爆音"啪"

✅ 转 int32 再限幅：
   (int32_t)20000 × 2 = 40000     ← 32 位里完全正常
   40000 > 32767 → 限幅到 32767    ← 削波，但【方向是对的】
```

| | 溢出反转 | 限幅削波 |
|---|---|---|
| 波形 | 峰值**翻到反方向** | 峰值**被削平** |
| 听感 | 剧烈爆音 ✗ | 轻微失真 ✓ |
| 原因 | 没转宽类型 | int32 + clamp |

### 其他要点

- **2 倍 = +6 dB**（`20·log₁₀(2) = 6.02`）
- **信号和噪声一起放大** → 信噪比不变，**不能改善音质**（这和「麦克风硬件增益」不同）
- **就地修改** `i2s_read_buffer` → 回调收到的是**已放大**的数据

**为什么是 2**：XIAO ESP32S3 Sense 内置 PDM 麦克风输出电平偏低，抬高一档提升编码效率。属于**实测经验值**。

---

# 四、环形缓冲

## 4.1 写入环形缓冲的过程

**知识点**：四步——**先算下一格 → 判断会不会撞上读指针 → 写当前格 → 指针前移（`% N` 绕回）**。

**代码**：

`opus_encoder.cpp:98-113`

```c
int opus_receive_pcm(int16_t *data, size_t samples)
{
    if (pcm_ring_buffer == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < samples; i++) {
        size_t next_write = (ring_write_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;   // ① 先算下一格
        if (next_write == ring_read_pos) {                                      // ② 会撞上读指针？
            // Buffer full, drop oldest sample
            ring_read_pos = (ring_read_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;    // ③ 丢最旧的
        }
        pcm_ring_buffer[ring_write_pos] = data[i];                              // ④ 写当前格
        ring_write_pos = next_write;                                            // ⑤ 指针前移
    }
    return 0;
}
```

**指针定义**：

`opus_encoder.cpp:13-15`

```c
static int16_t *pcm_ring_buffer = nullptr;
static volatile size_t ring_write_pos = 0;
static volatile size_t ring_read_pos  = 0;
```

**写入一个样本的 5 步（N=8 示意）**：

```
初始:   read=2, write=5          ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
                                 │     │     │  a  │  b  │  c  │     │     │     │
                                 └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
                                               ↑                       ↑
① next_write = (5+1) % 8 = 6              read=2                  write=5
② 6 == read(2) ?  否
④ buf[5] = x
⑤ write = 6
```

**⭐ 顺序很关键**：先算指针 +1 判断满没满（①②），**再往当前位置写**（④）——不是先写再判断。

**满了会怎样**：「丢最旧、保最新」——对实时音频是对的，宁可丢一段旧声音，也不能让新声音卡住。

**三个设计要点**：

1. **先算后写** → 保证写端**永远不覆盖未读数据**（除非满，那时主动丢弃）
2. **牺牲一格** → `N−1 = 7999` 可用。因为「空」定义为 `write == read`，不留一格的话「满」也是 `write == read`，两种状态分不清
3. **`volatile`** → 但本项目读写都在同一个 `loopTask` 里，其实无并发，更像防御性写法

---

## 4.2 每次写入是 2 字节吗？

**知识点**：**是的**，但**下标单位是「样本」不是「字节」**，`×2` 由编译器自动完成。

**代码**——两边都是 `int16_t`：

`opus_encoder.cpp:13` 和 `opus_encoder.cpp:98`

```c
static int16_t *pcm_ring_buffer = nullptr;              // ← int16_t 数组

int opus_receive_pcm(int16_t *data, size_t samples)     // ← data 也是 int16_t*
...
pcm_ring_buffer[ring_write_pos] = data[i];
//    int16_t（2字节）              int16_t（2字节）
```

**内存布局**：

```
pcm_ring_buffer (int16_t *)
下标:       0        1        2        3       ...      7999
         ┌────────┬────────┬────────┬────────┬─────┬────────┐
         │ 2 字节 │ 2 字节 │ 2 字节 │ 2 字节 │ ... │ 2 字节 │
         └────────┴────────┴────────┴────────┴─────┴────────┘
字节地址:  0        2        4        6              15998
```

编译器实际生成 `*(pcm_ring_buffer + ring_write_pos * 2)`。

**要点**：
- 单次循环 = **2 字节**
- 一次调用总量 = `samples` × 2 字节
- **`pcm_ring_buffer[3]` 落在字节地址 6**，不是 3

> ⚡ **对比记忆**：`int16_t *` 的 `p[3]` = 字节 6；`uint8_t *` 的 `p[3]` = 字节 3。**指针算术单位永远是元素大小。**

---

## 4.3 「解环」的代码

**知识点**：把环形里可能被**切成两段**的一帧，拼成一段**连续内存**。Opus 只认连续数组。

**代码**——就是带 `% N` 的 4 行循环：

`opus_encoder.cpp:153-158`

```c
while (ring_buffer_available() >= OPUS_FRAME_SAMPLES) {
    // Read samples from ring buffer
    for (size_t i = 0; i < OPUS_FRAME_SAMPLES; i++) {          // 320 次
        opus_input_buffer[i] = pcm_ring_buffer[ring_read_pos];  // 拷到【连续】的第 i 格
        ring_read_pos = (ring_read_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;  // ← 取模让指针绕回
    }
```

**跨边界的情况**：

```
pcm_ring_buffer   ring_read_pos = 7900
  ... [7900] ... [7999] │ [0] [1] ... [219]
       └── 100 个 ──┘    └── 220 个 ──┘
       ↑ 物理上不连续！但逻辑上连续

       拷贝到 ↓

opus_input_buffer
  [0] ... [99] [100] ... [319]
  └ 来自 7900~7999 ┘└ 来自 0~219 ┘   ← 被「拉直」成连续 320 个
```

**跑到 `ring_read_pos = 7999` 时，`(7999 + 1) % 8000 = 0`** —— 指针无缝回到开头。

**要点**：**每次 2 字节**（`opus_input_buffer[i] = pcm_ring_buffer[...]`，两边都是 `int16_t`），320 次共 **640 字节**。

---

## 4.4 `pcm_ring_buffer` 和 `pcm_data` 的关系

**知识点**：**不是同一块内存。**`pcm_data` 是 `opus_input_buffer` 的**形参别名**；`opus_input_buffer` 的内容是从 `pcm_ring_buffer` **拷**来的。

**代码**——三个名字三条线：

`opus_encoder.cpp:13`（仓库，8000 样本）

```c
static int16_t *pcm_ring_buffer = nullptr;
```

`opus_encoder.cpp:19`（出货箱，320 样本）

```c
static int16_t *opus_input_buffer = nullptr;
```

`opus_encoder.cpp:124`（形参名）

```c
int opus_encode_frame(int16_t *pcm_data, size_t samples)   // ← 形参叫 pcm_data
```

`opus_encoder.cpp:161`（实参是 opus_input_buffer）

```c
int encoded_bytes = opus_encode_frame(opus_input_buffer, OPUS_FRAME_SAMPLES);
```

| 名字 | 是什么 | 大小 | 角色 |
|---|---|---|---|
| `pcm_ring_buffer` | 全局静态指针 | **8000 样本** | **仓库** |
| `opus_input_buffer` | 全局静态指针 | **320 样本** | **出货箱** |
| `pcm_data` | **函数形参** | 指向 320 样本 | **只是形参名** |

**要点**：在 `opus_encode_frame()` 内部，`pcm_data` 和 `opus_input_buffer` **指向同一块 640 字节内存**。中间那次拷贝**不能省**——因为环形的一帧可能不连续。

---

## 4.5 Opus 的环形缓冲区多大？

**知识点**：**8000 个样本 = 16 KB = 500 ms**（可用 7999，因为牺牲一格）。

**代码**：

`config.h:132`

```c
#define AUDIO_RING_BUFFER_SAMPLES 8000 // 500ms of audio data
```

`opus_encoder.cpp:31`（分配在 PSRAM）

```c
pcm_ring_buffer = (int16_t *)heap_caps_malloc(AUDIO_RING_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
```

| 单位 | 值 | 算法 |
|---|---|---|
| 样本数 | **8000** | 定义值 |
| 字节 | **16000 B ≈ 15.6 KiB** | 8000 × 2 |
| 时长 | **500 ms** | 8000 ÷ 16000 |
| 可装帧数 | **25 帧** | 8000 ÷ 320 |

**⚠️ 别混淆**——同文件里还有两个「暂存缓冲」，不是环：

`opus_encoder.cpp:18-19`

```c
static uint8_t *opus_output_buffer = nullptr;      // 160 字节输出暂存（不是环）
static int16_t *opus_input_buffer = nullptr;       // 320 样本输入暂存（不是环）
```

**要点**：这 500ms 是**最大蓄水池**，不是常态水位——正常时读写速度相当，缓冲接近空，**不增加延迟**。

---

## 4.6 发送环形缓冲为什么是 16 × 162？

**知识点**：`16` 是**帧数容量**，`162 = 160 + 2` 是**单帧最大占地**（数据 + 长度前缀）。**16 是「最坏情况保证」，实际能装约 31 帧。**

**代码**：

`app.cpp:81-85`

```c
// Audio ring buffer for encoded packets
#define AUDIO_TX_BUFFER_SIZE (AUDIO_TX_RING_BUFFER_SIZE * (OPUS_OUTPUT_MAX_BYTES + 2))
static uint8_t audio_tx_buffer[AUDIO_TX_BUFFER_SIZE];
static volatile size_t audio_tx_write_pos = 0;
static volatile size_t audio_tx_read_pos  = 0;
```

`config.h:146` / `config.h:139`

```c
#define AUDIO_TX_RING_BUFFER_SIZE 16   // Number of encoded frames to buffer
#define OPUS_OUTPUT_MAX_BYTES 160      // Max encoded frame size
```

| 部分 | 值 | 含义 |
|---|---|---|
| `AUDIO_TX_RING_BUFFER_SIZE` | **16** | **帧数** |
| `OPUS_OUTPUT_MAX_BYTES + 2` | 160 + 2 = **162** | 每帧占地 |
| 乘积 | **2592** | 总字节数 |

**为什么 `+2`**——因为写入时加了 2 字节长度前缀（见 6.4）：

```
┌────────┬────────┬──────────────────────────┐
│ 长度低 │ 长度高 │   Opus 数据（最多 160）   │
└────────┴────────┴──────────────────────────┘
 └── 2 字节 ──┘└────── 最多 160 字节 ──────┘
```

**⭐ 为什么实际能装约 31 帧**：

$$16 \times 162 = 2592 \text{ 字节（按每帧最大算）}, \qquad \frac{2592}{80 + 2} \approx \mathbf{31 \text{ 帧}}$$

**要点**：它是**字节环**（指针按字节取模 `% 2592`），不是帧环。「16 帧」只是设计意图。对应时间约 **320 ms**（按最大值算）。

---

# 五、Opus 编码与码率

## 5.1 码率是什么？瞬时的还是平均的？

**知识点**：码率 = **每秒用多少比特**表示音频。`OPUS_BITRATE` 是**目标平均值**，不是瞬时值。

**代码**：

`config.h:140`

```c
#define OPUS_BITRATE 32000             // 32kbps
```

`opus_encoder.cpp:71`

```c
opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
```

**从码率推出每帧大小**：

$$\frac{32000\ \text{bit/s}}{8} = 4000\ \text{字节/s}, \qquad \frac{4000}{50\ \text{帧/s}} = \mathbf{80\ \text{字节/帧}}$$

**瞬时 vs 平均**：

| 帧内容 | 帧大小 | 瞬时码率 |
|---|---|---|
| 静音 | 20 B | 8 kbps |
| 平稳元音 | 60 B | 24 kbps |
| 正常说话 | 80 B | **32 kbps** ← 正好等于目标 |
| 语速快 + 噪音 | 120 B | 48 kbps |
| 音乐（顶到上限） | 160 B | **64 kbps** |

```
瞬时码率
(kbps)
 64 ┤                    ▄            ← 上限：160 字节/帧
    │      ▄        ▄    █   ▄
 32 ┼━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ ← 目标平均线（OPUS_BITRATE）
    │  ▄ █   ▀  ▄ ▀        ▀    ▀
  8 ┤ █
    └────────────────────────────────▶ 时间
```

**要点**：类比「平均时速 100 km/h」——瞬时可能在 0~120 之间摆，**平均才是那个计划**。

---

## 5.2 `opus_process()` 一次读多少？

**知识点**：**固定 320 个样本**（= 20 ms），并用 `while` 循环**把积压全部读完**。

**代码**：

`opus_encoder.cpp:146-167`

```c
void opus_process()
{
    if (encoder == nullptr || pcm_ring_buffer == nullptr || opus_input_buffer == nullptr) {
        return;
    }

    // Check if we have enough samples for a frame
    while (ring_buffer_available() >= OPUS_FRAME_SAMPLES) {        // ← 320
        for (size_t i = 0; i < OPUS_FRAME_SAMPLES; i++) {
            opus_input_buffer[i] = pcm_ring_buffer[ring_read_pos];
            ring_read_pos = (ring_read_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;
        }

        int encoded_bytes = opus_encode_frame(opus_input_buffer, OPUS_FRAME_SAMPLES);

        if (encoded_bytes > 0 && encoded_callback != nullptr) {
            encoded_callback(opus_output_buffer, encoded_bytes);
        }
    }
}
```

`config.h:138`

```c
#define OPUS_FRAME_SAMPLES 320         // 20ms frame @ 16kHz
```

| 这轮积压 | 编码帧数 | 剩余 |
|---|---|---|
| 8000（满） | 25 帧 | 0 |
| 500 | 1 帧 | 180 |
| 319 | **0 帧** | 319 |

**⚠️ 它读的是 PCM 环**（`pcm_ring_buffer`，麦克风灌进来的原始 PCM），不是「opus 输出环」。整个函数的**方向是「入」不是「出」**。

**要点**：用 `>=` 不是 `==`——**零头留在缓冲里**，和下次新数据拼成完整帧。

---

## 5.3 没攒够 320 会一直卡着吗？

**知识点**：**不会卡，立刻返回。**`while` 条件不成立 ≠ 阻塞等待。

**代码**：

`opus_encoder.cpp:153`

```c
while (ring_buffer_available() >= OPUS_FRAME_SAMPLES) {
    ...
}
// ← 条件不成立时，直接跳到这里，函数结束（一毫秒都不等）
```

**对比 `i2s_read` 的阻塞**：

| | `i2s_read` | `opus_process` 的 `while` |
|---|---|---|
| 数据不够时 | **阻塞**最多 20ms | **立即返回** |
| 代码 | `pdMS_TO_TICKS(20)` | `while (available >= 320)` |
| 为什么 | 等待有价值（让出 CPU 给 BLE 任务） | **等也没用**——数据自己会来，阻塞只会拖慢主循环 |

**要点**：这是「**有就干，没有就走**」的轮询（poll）模式。主循环 ~25ms 后就会再调它一次。

---

## 5.4 `opus_process()` 是 20 ms 读一次吗？

**知识点**：**不是。**「20ms」是**一帧音频的时长**，不是**函数调用间隔**。

**代码**——调用点在主循环里，**没有定时器**：

`app.cpp:883-886`

```c
if (audioEnabled && mic_is_running()) {
    mic_process();      // ← 里面 i2s_read 阻塞最多 20ms
    opus_process();     // ← 紧接着被调用（不是定时器触发）
}
```

| | 值 | 由谁决定 |
|---|---|---|
| **一帧消耗的音频时长** | 320 ÷ 16000 = **20 ms** | Opus 帧长（固定） |
| **函数被调用的间隔** | ≈ **25~30 ms** | 主循环节奏（浮动） |

**长期平均被钉死在 50 帧/秒**：

$$\frac{16000}{320} = 50\ \text{帧/秒}, \qquad \frac{50}{35} \approx \mathbf{1.4\ \text{帧/次}}$$

**要点**：它是「**数据驱动**」不是「时钟驱动」——主循环慢了就多读几帧自动追平，快了就读 0 帧。

---

## 5.5 `opus_encode` 各参数

**知识点**：五个参数 = 「把 320 个样本交给编码器，压缩成最多 160 字节」。

**代码**：

`opus_encoder.cpp:135-136`

```c
opus_int32 encoded_bytes =
    opus_encode(encoder, pcm_data, OPUS_FRAME_SAMPLES,
                opus_output_buffer, OPUS_OUTPUT_MAX_BYTES);
```

**官方原型对照**：

```c
opus_int32 opus_encode(OpusEncoder *st,            // ① 编码器实例
                       const opus_int16 *pcm,      // ② 输入 PCM
                       int frame_size,             // ③ 本帧样本数
                       unsigned char *data,        // ④ 输出缓冲
                       opus_int32 max_data_bytes); // ⑤ 输出缓冲上限
```

| # | 实参 | 值 | 作用 |
|---|---|---|---|
| ① | `encoder` | — | 编码器**实例**（**有状态！**记住上一帧） |
| ② | `pcm_data` | 320 个样本 | 输入 PCM |
| ③ | `OPUS_FRAME_SAMPLES` | **320** | 本帧**每声道**样本数 |
| ④ | `opus_output_buffer` | 160 字节缓冲 | 压缩结果写这里 |
| ⑤ | `OPUS_OUTPUT_MAX_BYTES` | **160** | ④ 的**容量上限**（安全阀） |

**① 有状态**——创建时设定：

`opus_encoder.cpp:57`

```c
encoder = opus_encoder_create(MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
//                           16000 Hz     单声道   VoIP 场景（优先走 SILK 引擎）
```

**后果**：不能打乱顺序（第 N 帧依赖第 N−1 帧）、不能并行。

**③ 是「每声道」样本数**——立体声时 320 意味着数组里有 640 个 `int16_t`。

**⑤ 不够时 Opus 降质量而不越界。**

---

## 5.6 输出到 `encoded_bytes` 还是 `opus_output_buffer`？

**知识点**：**两个都去**——数据进 `opus_output_buffer`，`encoded_bytes` 只拿到**长度**。

**代码**：

`opus_encoder.cpp:135` 和 `opus_encoder.cpp:163`

```c
opus_int32 encoded_bytes =
    opus_encode(encoder, pcm_data, OPUS_FRAME_SAMPLES,
                opus_output_buffer, OPUS_OUTPUT_MAX_BYTES);     // ← 数据写到这
...
encoded_callback(opus_output_buffer, encoded_bytes);
//                ↑ 数据在哪          ↑ 数据多长（两个都传！）
```

**决定性论据**：`opus_int32` = **4 字节**，物理上装不下 80 字节的数据。

**160 和实际值的区别**：

```
opus_output_buffer（容量 160 字节）
┌──────────────────────────────────────┬───────────────────────────┐
│      有效数据：78 字节                │  未使用（上次的残留）      │
│  ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ │  ░░░░░░░░░░░░░░░░░░░░░░░ │
└──────────────────────────────────────┴───────────────────────────┘
 ↑                                     ↑
 数据本身                              encoded_bytes = 78（有效到这儿）
```

**要点**：这是 C 的经典模式——「**缓冲区 + 长度**」。只给指针不知道到哪结束，只给长度不知道在哪。

---

## 5.7 Opus 编码的具体过程代码在哪？

**知识点**：**不在项目代码里**——在依赖的 libopus 库中，**完整 C 源码就在本地**。

**项目代码只有「调用 + 配置」**：

`opus_encoder.cpp:71-79`（配置）

```c
opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
opus_encoder_ctl(encoder, OPUS_SET_VBR(OPUS_VBR));
opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0));
opus_encoder_ctl(encoder, OPUS_SET_LSB_DEPTH(16));
opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(0));
```

**源码位置**（来自 `platformio.ini:33` 的依赖）：

```
.pio/libdeps/seeed_xiao_esp32s3/libopus/src/opus-1.3.1/
├── src/opus_encoder.c     ← opus_encode() 的实现（约 26,211 行 C）
├── celt/   (18 个 .c)     ← 引擎一：CELT
└── silk/   (大量 .c)      ← 引擎二：SILK
```

**调用链**：

```
opus_encode()                        src/opus_encoder.c:2226   ← 薄封装，做帧长检查
   ↓
opus_encode_native()                 src/opus_encoder.c:1066   ← ★ 真正干活
   ↓
   ├── silk_Encode()            silk/*.c   ← 语音（LPC 预测）
   ├── celt_encode_with_ec()    celt/*.c   ← 音乐（MDCT）
   └── 混合（HYBRID）
```

| 引擎 | 擅长 | 原理 | 目录 |
|---|---|---|---|
| **SILK** | 语音 | LPC 线性预测 | `silk/` |
| **CELT** | 音乐、低延迟 | MDCT + 感知量化 | `celt/` |

**要点**：本项目 `OPUS_APPLICATION_VOIP` 就是告诉 Opus「我要传人声」，于是**优先走 SILK**。

---

## 5.8 「320 样本 → 160 字节」这个理解哪里错了？

**知识点**：**320 和 160 之间不存在换算关系**——一个是输入样本数，一个是输出容量上限；实际输出由**码率**决定（~80 字节）。

**代码**——三个常量各管一件事：

`config.h:138-140`

```c
#define OPUS_FRAME_SAMPLES 320         // 【输入】：一次喂多少样本
#define OPUS_OUTPUT_MAX_BYTES 160      // 【容量上限】：箱子多大
#define OPUS_BITRATE 32000             // 【决定输出】：实际产出多少 ← 关键
```

**实际数据量变化**：

```
     输入                     编码                    输出
┌───────────────┐        ┌──────────┐        ┌──────────────┐
│  320 个样本    │        │  Opus    │        │  ~80 字节     │
│  = 640 字节 PCM│ ─────▶ │  压缩 8:1 │ ─────▶ │ (浮动 60~140) │
└───────────────┘        └──────────┘        └──────────────┘
```

**「必须 320 吗」——不是必须，是选择**：

| 帧长 | 样本 | 每秒帧数 | 延迟 | BLE 开销 |
|---|---|---|---|---|
| 10 ms | 160 | 100 | 低 | 高 |
| **20 ms** | **320** | **50** | **中** | **中** |
| 40 ms | 640 | 25 | 高 | 低 |

**要点**：**`160` 是「箱子有多大」，不是「装了多满」**。它是平均值的 2 倍，给 VBR 峰值留余量。

---

## 5.9 每帧输出多少由声音复杂度决定吗？

**知识点**：**是，但只是一半。**是「声音复杂度」和「目标码率」两股力量拉扯的结果。

**代码**——VBR 开关就是「允许浮动」：

`opus_encoder.cpp:74-75`

```c
opus_encoder_ctl(encoder, OPUS_SET_VBR(OPUS_VBR));              // OPUS_VBR = 1
opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0));          // 0 = 不限制浮动幅度
```

**如果改成 CBR**（`OPUS_SET_VBR(0)`），每帧就固定约 80 字节——**瞬时 = 平均**。

| 这 20ms 是什么声音 | 大概多少字节 |
|---|---|
| 静音 / 只有底噪 | 10 ~ 40 |
| 平稳元音（"啊——"） | 50 ~ 70 |
| 正常说话 | 70 ~ 90 |
| 语速快 + 噪音 + 爆破音 | 100 ~ 140 |
| 音乐 / 混杂噪声 | 140 ~ 160（逼近上限） |

**规律**：周期性、平稳的声音 → 预测得准 → **少字节**；突变的 → 难预测 → **多字节**。这正是 SILK 的 **LPC 预测**在做的事。

**要点**：**帧长和复杂度是两个独立维度**——同样 20ms，静音可能 15 字节，摇滚乐可能 155 字节，**差 10 倍**。

---

# 六、软件架构：回调与并发

## 6.1 轮询 vs 中断

**知识点**：轮询 = CPU **主动问**；中断 = 硬件**主动报**。**本项目两种都用。**

### 麦克风 = 轮询

`mic.cpp:121-122`

```c
size_t bytes_read = 0;
esp_err_t err =
    i2s_read(I2S_PORT, i2s_read_buffer, MIC_BUFFER_SAMPLES * sizeof(int16_t), &bytes_read, pdMS_TO_TICKS(20));
    //                                                                                    ↑ 主动去问，最多等 20ms
```

装驱动时 `queue_size = 0`（`mic.cpp:68`）——**没有 DMA 中断**。

> ⚠️ 这是**「阻塞式轮询」**，不是纯忙等——20ms 等待期间任务挂起，CPU 让给 BLE 任务。纯轮询是 `pdMS_TO_TICKS(0)`。

### 按键 = 中断 + 轮询混合

`app.cpp:120-123`（ISR 里只做一件事）

```c
void IRAM_ATTR buttonISR()
{
    buttonPressed = true;      // ← ISR 只设一个 flag
}
```

`app.cpp:814`（注册中断）

```c
attachInterrupt(digitalPinToInterrupt(POWER_BUTTON_PIN), buttonISR, CHANGE);
```

`app.cpp:192-214`（主循环里轮询 flag，做去抖和长按）

```c
void handleButton()
{
    unsigned long now = millis();
    static unsigned long lastDebounceTime = 0;
    static bool buttonDown = false;
    static bool longPressTriggered = false;

    bool currentButtonState = !digitalRead(POWER_BUTTON_PIN); // Active low

    if (currentButtonState && !buttonDown) {
        if (now - lastDebounceTime < 50) {      // ← 去抖
            return;
        }
        buttonPressTime = now;
        buttonDown = true;
        longPressTriggered = false;
        lastDebounceTime = now;

    } else if (currentButtonState && buttonDown && !longPressTriggered) {
        unsigned long pressDuration = now - buttonPressTime;
        if (pressDuration >= 2000) {            // ← 长按 2 秒
```

**两条路径对比**：

```
【中断】按键
   硬件电平变化 → buttonISR() 立刻执行（只设 flag，几微秒返回）
                → 主循环下一轮 handleButton() 轮询 flag，做去抖/长按

【轮询】麦克风
   主循环每轮 → mic_process() → i2s_read(超时 20ms) 主动问"有数据吗"
```

**为什么按键用混合**：ISR **必须极短**（不能 `malloc`、不能 `Serial.print`、不能 `delay`），所以只设 flag；而去抖/长按需要「等一段时间看状态」，这种逻辑放主循环最合适。

**同一个 I2S 的两种用法**——区别只在驱动安装的第三个参数：

```c
// 方案 A：轮询（本项目）
i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);   // ← 0 = 不要事件队列

// 方案 B：事件驱动
static QueueHandle_t i2s_evt_queue;
i2s_driver_install(I2S_PORT, &i2s_config, 4, &i2s_evt_queue);   // ← 要事件队列
// 然后开一个独立任务 xQueueReceive 等事件
```

---

## 6.2 `audio_callback` 的函数指针逻辑

**知识点**：函数指针 = 存着**函数地址**的变量。四步链条：定义类型 → 声明变量 → 注册 → 调用。

**① 定义类型** —— `mic.h:8`

```c
typedef void (*mic_data_handler)(int16_t *data, size_t samples);
```

从里往外读：`mic_data_handler` 是「指向一个接收 `(int16_t*, size_t)`、返回 `void` 的函数的指针」类型。

**② 声明变量** —— `mic.cpp:12`

```c
static mic_data_handler audio_callback = nullptr;   // 初始"没人登记"
```

**③ 注册** —— `mic.cpp:109-112` + `app.cpp:856`

```c
void mic_set_callback(mic_data_handler callback)
{
    audio_callback = callback;      // 只是把地址存起来
}
// ...
mic_set_callback(onMicData);        // ← 传函数【名】，不是 onMicData()
```

> ⚠️ **`onMicData` 和 `onMicData()` 差一个括号，意思完全不同**
> - `onMicData` = 函数的**地址**（一个值）
> - `onMicData()` = **调用**这个函数（执行它）

**④ 调用** —— `mic.cpp:140-142`

```c
if (audio_callback != nullptr) {                    // 守卫：没人注册就跳过
    audio_callback(i2s_read_buffer, samples_read);  // 通过指针【跳到那个地址执行】
}
```

**内存视图**：

```
audio_callback 变量（8 字节指针，在 RAM）
┌──────────────────┐
│  0x400D1A2C      │ ──────────▶  onMicData 的机器码（在 Flash）
└──────────────────┘
      ↑ 注册时把地址写进来
```

**要点**：`nullptr` 守卫**必须**——否则时序出错时会跳到地址 0，**立即崩溃**。

---

## 6.3 `onMicData` 的实际作用

**知识点**：**一行——把麦克风刚采到的 PCM 转交给 Opus 编码器。**

**代码**：

`app.cpp:324-328`

```c
void onMicData(int16_t *data, size_t samples)
{
    // Feed PCM data to Opus encoder
    opus_receive_pcm(data, samples);
}
```

**在链路里的位置**：

```
i2s_read() 填满 i2s_read_buffer（PCM）
   ↓
MIC_GAIN 增益 ×2
   ↓
audio_callback(i2s_read_buffer, samples_read)   ← mic.cpp:141 调用
   ↓
★ onMicData(data, samples) ★                    ← app.cpp:324
   ↓
opus_receive_pcm(data, samples)                 ← opus_encoder.cpp:98
   ↓
pcm_ring_buffer[ring_write_pos] = data[i]       ← 逐样本存入环形缓冲
```

**三个实际要点**：

1. **收到的是 PCM，不是 PDM** —— PDM→PCM 早在硬件层完成
2. **必须当场消费** —— `data` 指向的 `i2s_read_buffer` 是全局静态缓冲，下一轮会被覆盖。`opus_receive_pcm()` 内部逐样本拷贝是对的
3. **不是中断** —— 跑在 Arduino `loopTask` 上下文，可以放心做循环拷贝，但耗时算进主循环

**如果没有它会怎样**（紧耦合版本）：

```c
// mic.cpp —— ❌ 麦克风模块被迫知道编码器存在
#include "opus_encoder.h"
void mic_process() {
    ...
    opus_receive_pcm(i2s_read_buffer, samples_read);   // 写死
}
```

**要点**：它是 mic 和 opus 两个模块之间**唯一的桥**，存在理由是**解耦**——换消费者只改一行注册。

---

## 6.4 `onOpusEncoded` 回调的作用

**知识点**：**「编码器」和「BLE 发送」之间的中转站**——把编码好的帧**暂存进发送队列**（加 2 字节长度前缀）。**它不发数据。**

**代码**：

`app.cpp:330-359`

```c
void onOpusEncoded(uint8_t *data, size_t len)
{
    // Store encoded data in TX ring buffer
    if (len > OPUS_OUTPUT_MAX_BYTES) {
        return;                                    // ① 校验长度
    }

    size_t packet_size = len + 2;
    size_t next_write = (audio_tx_write_pos + packet_size) % AUDIO_TX_BUFFER_SIZE;

    // ③ 溢出保护
    if ((audio_tx_write_pos < audio_tx_read_pos && next_write >= audio_tx_read_pos) ||
        (audio_tx_write_pos >= audio_tx_read_pos && next_write < audio_tx_write_pos &&
         next_write >= audio_tx_read_pos)) {
        return;                                    // 环满，丢这一帧
    }

    // ② 加 2 字节长度前缀
    audio_tx_buffer[audio_tx_write_pos] = len & 0xFF;
    audio_tx_buffer[(audio_tx_write_pos + 1) % AUDIO_TX_BUFFER_SIZE] = (len >> 8) & 0xFF;

    for (size_t i = 0; i < len; i++) {
        audio_tx_buffer[(audio_tx_write_pos + 2 + i) % AUDIO_TX_BUFFER_SIZE] = data[i];
    }

    audio_tx_write_pos = next_write;
}
```

**注册**：`app.cpp:853`

```c
opus_set_callback(onOpusEncoded);
```

**⚠️ 它不发数据**——真正的发送在 `processAudioTx()`（`app.cpp:380-417`），主循环里另一处：

`app.cpp:883-891`

```c
if (audioEnabled && mic_is_running()) {
    mic_process();
    opus_process();          // ← 编码，触发 onOpusEncoded（只是暂存）
}
if (connected && audioSubscribed) {
    processAudioTx();        // ← 这里才真正发 BLE
}
```

**为什么拆两步**：编码由 Opus 帧长定时定量、不能停（麦克风一直在进数据）；发送要看 BLE 连接状态和拥塞。**发送环就是缓冲池。**

**为什么要 2 字节长度前缀**：Opus 是 VBR，每帧大小不一样，而发送环是**字节流**，不知道帧的边界：

```
不加前缀：[帧1 80字节][帧2 95字节][帧3 62字节]  ← 读端不知第一帧到哪结束
加前缀：  [80][帧1][95][帧2][62][帧3]           ← 一看 95 就知道接下来 95 字节是一帧
```

**和 `onMicData` 对比**：

| | `onMicData` | `onOpusEncoded` |
|---|---|---|
| 代码量 | **1 行** | **29 行** |
| 做什么 | 原样转发 | 加前缀 + 溢出检查 |
| 为什么 | 下游是**定长样本流**，天然对齐 | 下游是**变长帧字节流**，必须自己划边界 |

---

# 七、BLE 发送与协议

## 7.1 包组装：`memcpy` + `setValue`

**知识点**：把 Opus 帧装进 BLE 包——**3 字节包头 + 负载**。

**代码**：

`app.cpp:361-378`

```c
void broadcastAudioPacket(uint8_t *data, size_t len)
{
    if (!connected || !audioSubscribed || audioDataCharacteristic == nullptr) {
        return;
    }

    // Build packet: 2 bytes index + 1 byte sub-index + data
    audio_packet_buffer[0] = audioPacketIndex & 0xFF;            // 序号 低字节
    audio_packet_buffer[1] = (audioPacketIndex >> 8) & 0xFF;     // 序号 高字节
    audio_packet_buffer[2] = 0;                                  // 子序号

    memcpy(audio_packet_buffer + AUDIO_PACKET_HEADER_SIZE, data, len);        // ① 拷负载
    audioDataCharacteristic->setValue(audio_packet_buffer, len + AUDIO_PACKET_HEADER_SIZE);  // ② 交给 BLE
    audioDataCharacteristic->notify();                            // ③ 真正发出

    audioPacketIndex++;
}
```

`config.h:145`

```c
#define AUDIO_PACKET_HEADER_SIZE 3     // 2 bytes index + 1 byte sub-index
```

**① `memcpy`** —— 把 Opus 数据搬进缓冲，**故意从偏移 3 开始**（前 3 字节是包头）。

> ⚡ **注意 `+3` 和 `pcm_ring_buffer[3]` 完全不同**：
> - `int16_t *pcm_ring_buffer` 的 `p[3]` → **字节 6**（×2）
> - `uint8_t *audio_packet_buffer` 的 `+3` → **字节 3**（×1）

**② `setValue`** —— 为什么是 `len + 3` 不是 `len`？因为要送**整个包**。只写 `len` 会把包头切掉，接收端无法判断丢包。

**包布局**：

```
audio_packet_buffer （容量 163 字节 = 160 + 3）
┌──────────┬──────────┬──────────┬────────────────────────────────┬─────────┐
│  [0]     │  [1]     │  [2]     │  [3] ... [3+len-1]             │  未使用  │
│ 序号低   │ 序号高   │ 子序号   │  Opus 帧数据 (len 字节)         │         │
└──────────┴──────────┴──────────┴────────────────────────────────┴─────────┘
 └──── 3 字节包头 ────┘└──────────── 负载 ────────────┘
 └──────────────── len + 3 字节 ─────────────────────┘
```

**包头的作用**：

| 字节 | 内容 | 作用 |
|---|---|---|
| `[0]` `[1]` | `audioPacketIndex`（小端序） | **包序号**——接收端靠它检测丢包/乱序 |
| `[2]` | `0` | 子序号，为分片预留 |

**为什么需要序号**：BLE 通知可能丢包、乱序。手机收到 `100, 101, 103` 就知道 102 丢了。Opus 支持丢包隐藏（PLC），但前提是**知道丢了包**。

**边界安全**：靠上游校验（`app.cpp:396`）保证 `len ≤ 160`，所以 `len + 3 ≤ 163` = 缓冲容量。

**⚠️ 配套条件：BLE MTU** —— 包总长最大 163 字节，**超过 BLE 默认 ATT MTU**（23 字节，负载 20）。前提是手机连接时协商了足够大的 MTU（≥ 166）。如果 MTU 太小，`notify()` 会截断或失败——**这是调试「音频发不出去」时该查的一个点**。

---

# 附录：跨类别的易错点

## A. 三个「20ms」不要混

| 说法 | 数值 | 性质 |
|---|---|---|
| Opus 帧的音频时长 | 320 样本 = 20 ms | 固定，Opus 规定 |
| `i2s_read` 的超时 | 20 ms | 固定，代码设定 |
| `opus_process()` 调用间隔 | ≈ 25~30 ms | 浮动，主循环决定 |

## B. 指针算术速查（最反复的坑）

| 类型 | 表达式 | 实际字节偏移 |
|---|---|---|
| `int16_t *p` | `p[3]` / `p + 3` | **+6 字节** |
| `uint8_t *p` | `p[3]` / `p + 3` | **+3 字节** |

代码里的实例：

```c
pcm_ring_buffer[3]                      // int16_t → 字节 6
audio_packet_buffer + AUDIO_PACKET_HEADER_SIZE   // uint8_t → 字节 3
```

## C. 阻塞 vs 不阻塞

| 函数 | 数据不够时 | 代码 | 原因 |
|---|---|---|---|
| `i2s_read` | **阻塞**最多 20ms | `pdMS_TO_TICKS(20)` | 等待有价值（让出 CPU） |
| `opus_process` 的 `while` | **立即返回** | `while (available >= 320)` | 等待没意义，只会拖慢主循环 |

## D. 两个环的溢出策略不同

| 环 | 满时 | 代码位置 | 理由 |
|---|---|---|---|
| PCM 环 | 丢**最旧**（推 read_pos） | `opus_encoder.cpp:107` | 保实时性，不能有累积延迟 |
| 发送环 | 丢**最新**（`return`） | `app.cpp:345-347` | 保持队列顺序，实现简单 |

## E. 一次典型流程的数据量变化

```
麦克风 PDM          1 bit @ ~1 MHz          →（硬件抽取）→
i2s_read            几百个样本（≤1600）      →（增益 ×2）→
pcm_ring_buffer     攒够 320 样本            →（拷贝解环）→
opus_input_buffer   320 样本 = 640 字节      →（Opus 编码 8:1）→
opus_output_buffer  ~80 字节                 →（+2 长度前缀）→
audio_tx_buffer     ~82 字节                 →（+3 包头）→
BLE notify          ~83 字节                 →  手机
```

## F. 七个关键常量一览

| 常量 | 值 | 位置 | 类别 |
|---|---|---|---|
| `MIC_SAMPLE_RATE` | 16000 | `config.h:129` | 采样 |
| `MIC_BUFFER_SAMPLES` | 1600 | `config.h:130` | DMA 读取 |
| `MIC_GAIN` | 2 | `config.h:131` | 增益 |
| `AUDIO_RING_BUFFER_SAMPLES` | 8000 | `config.h:132` | 环形缓冲 |
| `OPUS_FRAME_SAMPLES` | 320 | `config.h:138` | Opus |
| `OPUS_OUTPUT_MAX_BYTES` | 160 | `config.h:139` | Opus |
| `OPUS_BITRATE` | 32000 | `config.h:140` | 码率 |
| `AUDIO_TX_RING_BUFFER_SIZE` | 16 | `config.h:146` | 发送环 |
| `dma_buf_count × dma_buf_len` | 8 × 256 | `mic.cpp:52-53` | DMA |
