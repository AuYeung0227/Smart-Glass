#include "mic.h"

#include <driver/i2s.h>

#include "config.h"

// I2S configuration for PDM microphone
#define I2S_PORT I2S_NUM_0

// Static variables
static volatile bool mic_running = false;
static mic_data_handler audio_callback = nullptr;
static mic_data_handler analysis_callback = nullptr;
static mic_data_handler recording_callback = nullptr;
static int16_t *i2s_read_buffer = nullptr;

bool mic_start()
{
    if (mic_running) {
        Serial.println("Microphone already running");
        return true;
    }

    Serial.println("Initializing I2S PDM microphone...");
    Serial.printf("  CLK Pin: GPIO%d\n", MIC_CLK_PIN);
    Serial.printf("  DATA Pin: GPIO%d\n", MIC_DATA_PIN);
    Serial.printf("  Sample Rate: %d Hz\n", MIC_SAMPLE_RATE);

    // Allocate buffer in PSRAM for better performance
    if (i2s_read_buffer == nullptr) {
        i2s_read_buffer = (int16_t *) ps_malloc(MIC_BUFFER_SAMPLES * sizeof(int16_t));
        if (i2s_read_buffer == nullptr) {
            Serial.println("Failed to allocate mic buffer in PSRAM!");
            // Try regular malloc as fallback
            i2s_read_buffer = (int16_t *) malloc(MIC_BUFFER_SAMPLES * sizeof(int16_t));
            if (i2s_read_buffer == nullptr) {
                Serial.println("Failed to allocate mic buffer!");
                return false;
            }
            Serial.println("Using regular RAM for mic buffer");
        } else {
            Serial.println("Using PSRAM for mic buffer");
        }
    }

    // I2S configuration for PDM microphone
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t) (I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
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

    // I2S pin configuration for XIAO ESP32S3 Sense PDM microphone
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = MIC_CLK_PIN,   // PDM CLK
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_PIN, // PDM DATA
    };

    // Install and configure I2S driver
    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("Failed to install I2S driver: %s\n", esp_err_to_name(err));
        return false;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("Failed to set I2S pins: %s\n", esp_err_to_name(err));
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    // Clear DMA buffers
    i2s_zero_dma_buffer(I2S_PORT);

    mic_running = true;
    Serial.println("Microphone started successfully");
    return true;
}

void mic_stop()
{
    if (!mic_running) {
        return;
    }

    Serial.println("Stopping microphone...");

    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);

    mic_running = false;
    Serial.println("Microphone stopped");
}

bool mic_is_running()
{
    return mic_running;
}

void mic_set_callback(mic_data_handler callback)
{
    audio_callback = callback;
}

void mic_set_analysis_callback(mic_data_handler callback)
{
    analysis_callback = callback;
}

void mic_set_recording_callback(mic_data_handler callback)
{
    recording_callback = callback;
}

// 自适应去尖峰：滑动基线(EMA)决定阈值，超阈值的连续尖刺段用前后正常样本线性插值填补
// 三个改进：差分跳变检测(DIFF)、最大宽度限制(MAX_SPIKE_SAMPLES)、更快 EMA(EMA_SHIFT)
static void despike(int16_t *buf, size_t n)
{
    static int32_t baseline = DESPIKE_BASELINE_FLOOR;  // EMA 短时平均幅值，跨帧保持

    size_t i = 0;
    while (i < n) {
        int32_t x = buf[i];
        int32_t ax = (x < 0) ? -x : x;

        // 自适应阈值 = baseline * DESPIKE_RATIO，带保底
        int32_t threshold = baseline * DESPIKE_RATIO;
        int32_t min_thres = DESPIKE_BASELINE_FLOOR * DESPIKE_RATIO;
        if (threshold < min_thres)
            threshold = min_thres;

        // 差分跳变检测：尖刺是瞬间跳变，语音是渐变
        int32_t diff = (i > 0) ? ((x > buf[i - 1]) ? (x - buf[i - 1]) : (buf[i - 1] - x)) : 0;
        int32_t diff_thres = baseline * DIFF_THRESHOLD_RATIO;

        // 同时满足：幅值超阈值 + 跳变超阈值，才判定为尖刺起点
        if (ax > threshold && diff > diff_thres) {
            // 找到尖刺段 [i, j]
            size_t j = i;
            while (j + 1 < n) {
                int32_t xj = buf[j + 1];
                int32_t axj = (xj < 0) ? -xj : xj;
                if (axj > threshold)
                    j++;
                else
                    break;
            }
            size_t spike_len = j - i + 1;

            // 宽度超过上限 → 判定为正常语音段，正常更新基线并跳过插值
            if (spike_len > MAX_SPIKE_SAMPLES) {
                for (size_t k = i; k <= j; k++) {
                    int32_t axk = (buf[k] < 0) ? -buf[k] : buf[k];
                    baseline += (axk - baseline) >> EMA_SHIFT;
                }
                i = j + 1;
                continue;
            }

            // 段前后正常样本（边界缺失用 0）线性插值
            int32_t left = (i > 0) ? buf[i - 1] : 0;
            int32_t right = (j + 1 < n) ? buf[j + 1] : 0;
            for (size_t k = 0; k < spike_len; k++) {
                int32_t v = left + (right - left) * (int32_t)(k + 1) / (int32_t)(spike_len + 1);
                buf[i + k] = (int16_t) v;
            }
            // 尖刺段内基线冻结（不更新），防止尖刺抬高基线导致漏检
            i = j + 1;
        } else {
            // 正常样本：更新基线（EMA）
            baseline += (ax - baseline) >> EMA_SHIFT;
            i++;
        }
    }
}

// 二阶 Butterworth 高通滤波器（biquad），截止 100Hz @ 16kHz，Q=0.7071
// 保留语音基频：50Hz -12dB、115Hz -2dB、200Hz 以上基本不衰减
// 差分方程 y = b0*x + b1*x1 + b2*x2 + a1*y1 + a2*y2（Q14 系数，int64 累加防溢出）
static void highpass(int16_t *buf, size_t n)
{
    const int32_t b0 = 15935, b1 = -31871, b2 = 15935, a1 = 31858, a2 = -15499;
    static int32_t x1 = 0, x2 = 0;  // x[n-1], x[n-2]
    static int32_t y1 = 0, y2 = 0;  // y[n-1], y[n-2]

    for (size_t i = 0; i < n; i++) {
        int32_t x = buf[i];
        int64_t acc = (int64_t)b0 * x + (int64_t)b1 * x1 + (int64_t)b2 * x2
                    + (int64_t)a1 * y1 + (int64_t)a2 * y2;
        int32_t y = (int32_t)(acc >> 14);
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        buf[i] = (int16_t) y;
    }
}

// 三点中值滤波：用窗口内三个样本的中值替换当前样本。
// 对「孤立的单样本脉冲」（PDM/电源尖刺）能直接剔除——尖点夹在两个正常样本之间，
// 取中值后消失；对「连续语音」则安全——连续波形取中值仍是语音，不会像 despike
// 的线性插值那样把辅音整段抹平。这是脉冲（椒盐）噪声的标准处理方法。
static int16_t median3_v(int16_t a, int16_t b, int16_t c)
{
    int16_t mn = a, mx = a;
    if (b < mn) mn = b;
    if (b > mx) mx = b;
    if (c < mn) mn = c;
    if (c > mx) mx = c;
    // 中值 = 三数之和 - 最大 - 最小（用 int32 求和，避免三个 int16 相加溢出）
    return (int16_t)((int32_t)a + (int32_t)b + (int32_t)c - (int32_t)mn - (int32_t)mx);
}

static void median_filter3(int16_t *buf, size_t n)
{
    if (n < 3) return;

    int16_t orig_prev = buf[0];
    for (size_t i = 0; i < n; i++) {
        int16_t cur = buf[i];
        int16_t left  = (i > 0) ? orig_prev : cur;       // 左侧用原始值
        int16_t right = (i + 1 < n) ? buf[i + 1] : cur;  // 右侧尚未处理，也是原始值
        int16_t m = median3_v(left, cur, right);
        orig_prev = cur;  // 保存原始当前值作为下一点的 left（原地写回不污染计算）
        buf[i] = m;
    }
}

// 软限幅增益：线性区保持 MIC_GAIN 不变，仅对接近满量程的瞬态峰值平滑压缩，
// 消除硬钳位的削波失真。
static int16_t apply_gain_soft(int16_t x)
{
    float g = (float)x * (float)MIC_GAIN / 32768.0f;  // 增益后归一化幅度
    float a = (g < 0.0f) ? -g : g;

    const float T0 = 0.80f;  // 线性区上限：以下严格保持增益
    const float T1 = 1.20f;  // 软封顶起点
    const float S  = 0.99f;  // 饱和顶
    float y;
    if (a <= T0) {
        y = g;                                       // 纯线性，音量不变
    } else {
        float t = (a - T0) / (T1 - T0);
        if (t > 1.0f) t = 1.0f;
        float w = t * t * (3.0f - 2.0f * t);         // smoothstep 平滑权重
        float v = (1.0f - w) * a + w * S;            // 线性值与饱和顶平滑混合
        y = (g < 0.0f) ? -v : v;
    }

    int32_t out = (int32_t)(y * 32768.0f);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

// 三点取中值（中值=三数和-最大-最小；int32 求和防 int16 溢出）
static int16_t declick_median3(int16_t a, int16_t b, int16_t c)
{
    int32_t mn = a, mx = a;
    if (b < mn) mn = b;
    if (b > mx) mx = b;
    if (c < mn) mn = c;
    if (c > mx) mx = c;
    return (int16_t)((int32_t)a + (int32_t)b + (int32_t)c - mn - mx);
}

// 门控去脉冲：用非对称 EMA 维护一个对脉冲免疫的背景电平（上升慢、下降快），
// 仅在背景电平低（停顿/静音）时用三点中值剔除孤立咔哒声；持续语音时背景升高、
// 门控关闭、完全旁路，绝不损伤高频辅音。
static void gated_declick(int16_t *buf, size_t n)
{
    static int32_t s_floor = 0;  // 背景电平 mean-abs，跨帧保持

    int16_t orig_prev = buf[0];
    for (size_t i = 0; i < n; i++) {
        int16_t cur = buf[i];
        int32_t a = (cur < 0) ? -(int32_t)cur : (int32_t)cur;
        int16_t left  = (i > 0) ? orig_prev : cur;
        int16_t right = (i + 1 < n) ? buf[i + 1] : cur;

        // 非对称背景跟踪：上升慢(>>9，1~2样本的孤立脉冲抬不动，只有持续语音能抬高)，
        // 下降快(>>7，语音一停很快回到噪声底，及时开门)
        if (s_floor == 0) s_floor = a;
        if (a > s_floor) {
            s_floor += (a - s_floor) >> 9;
        } else {
            s_floor -= (s_floor - a) >> 7;
            if (s_floor < 0) s_floor = 0;
        }

        int16_t out = cur;
        if (s_floor < DECLICK_GATE_LEVEL) {
            int16_t m = declick_median3(left, cur, right);
            int32_t jump = (cur > m) ? ((int32_t)cur - m) : ((int32_t)m - cur);
            if (jump >= DECLICK_MIN_JUMP) {
                out = m;  // 静音中的孤立脉冲：用中值替换
            }
        }

        orig_prev = cur;
        buf[i] = out;
    }
}

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

        // Read-only tap on the raw pre-gain samples for the voiceprint module.
        // Must run before gain is applied and must not modify the buffer.
        if (analysis_callback != nullptr) {
            analysis_callback(i2s_read_buffer, samples_read);
        }

        // 只做：高通去低频 → 增益（软限幅兜底）。
        // 不做去尖刺滤波——波形图上 98% 的“尖刺”是高频辅音的正常形态，
        // despike/中值会把辅音高频一并砍掉，导致快速说话发糊。
        highpass(i2s_read_buffer, samples_read);
        for (size_t i = 0; i < samples_read; i++) {
            i2s_read_buffer[i] = apply_gain_soft(i2s_read_buffer[i]);
        }

        // Read-only tap on the processed samples for the recording module.
        // Same PCM the Opus encoder sees, so .opus files match BLE audio.
        if (recording_callback != nullptr) {
            recording_callback(i2s_read_buffer, samples_read);
        }

        if (audio_callback != nullptr) {
            audio_callback(i2s_read_buffer, samples_read);
        }
    }
}
