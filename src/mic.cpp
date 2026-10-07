#include "mic.h"

#include <driver/i2s.h>

#include "config.h"

// I2S configuration for PDM microphone
#define I2S_PORT I2S_NUM_0

// Static variables
static volatile bool mic_running = false;
static mic_data_handler audio_callback = nullptr;
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

        // Apply gain (clamp to 16-bit to avoid overflow), then despike glitches
        for (size_t i = 0; i < samples_read; i++) {
            int32_t sample = (int32_t) i2s_read_buffer[i] * MIC_GAIN;
            if (sample > 32767)
                sample = 32767;
            if (sample < -32768)
                sample = -32768;
            i2s_read_buffer[i] = (int16_t) sample;
        }
        despike(i2s_read_buffer, samples_read);
        highpass(i2s_read_buffer, samples_read);

        if (audio_callback != nullptr) {
            audio_callback(i2s_read_buffer, samples_read);
        }
    }
}
