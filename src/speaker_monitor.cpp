#include "speaker_monitor.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "recorder.h"
#include "voiceprint.h"

#define SEG_SAMPLES ((size_t)MFCC_SEGMENT_SAMPLES)
#define SEG_HOP ((size_t)MFCC_SEGMENT_HOP_SAMPLES)

// 送给 MFCC 前端的连续 2 秒窗口。每次推理后把窗口左移一个 hop，等下一段新音频
// 填满尾部——这自然决定了推理节奏（当前为 0.5s 一次）。
static int16_t *s_win = nullptr;
static volatile size_t s_win_len = 0;
static volatile bool s_win_ready = false;

static volatile speaker_state_t s_state = SPEAKER_STATE_IDLE;
static volatile uint32_t s_state_since = 0;
static volatile uint32_t s_last_target = 0;   // 上次匹配到用户本人的时刻
static volatile uint32_t s_last_speech = 0;   // 上次检测到有人说话的时刻（全态维护）
static volatile int s_vad_run = 0;

// VAD 帧在这里跨 feed() 调用累积（每次收到的块小于一帧）。
static int16_t s_vad_buf[VAD_FRAME_SAMPLES];
static size_t s_vad_fill = 0;

static TaskHandle_t s_task = nullptr;
static bool s_enabled = false;

// 录入与运行时状态机不能同时驱动同一个窗口。
static volatile bool s_enrolling = false;
static volatile uint32_t s_enroll_since = 0;
static volatile uint32_t s_enroll_last_sec = 0;   // 倒计时已打印过的秒数（去重）
static uint32_t s_no_tpl_hint = 0;                // 「未录入声纹」提示的节流时刻

static const char *STATE_NAMES[] = {"待机", "待验证", "被动录音"};

const char *speaker_monitor_state_name()
{
    if (s_enrolling) {
        return "录入";
    }
    speaker_state_t st = s_state;
    return STATE_NAMES[(st <= SPEAKER_STATE_MONITOR) ? (int)st : 0];
}

speaker_state_t speaker_monitor_state()
{
    return s_state;
}

static void reset_window()
{
    s_win_len = 0;
    s_win_ready = false;
    s_vad_fill = 0;
}

static void enter_idle()
{
    if (recorder_is_writing()) {
        recorder_stop();
    }
    s_state = SPEAKER_STATE_IDLE;
    s_state_since = millis();
    s_vad_run = 0;
    reset_window();
    Serial.println("【声纹】状态 → 待机（仅VAD）");
}

static void enter_verify()
{
    s_state = SPEAKER_STATE_VERIFY;
    s_state_since = millis();
    s_vad_run = 0;
    reset_window();
    Serial.println("【声纹】状态 → 待验证（检测到语音，全速比对）");
}

static void enter_monitor()
{
    bool recording = recorder_start();  // 先冲刷滚动预缓冲，之后持续写直播
    s_state = SPEAKER_STATE_MONITOR;
    s_state_since = millis();
    uint32_t now = millis();
    s_last_speech = now;
    s_last_target = now;
    reset_window();
    Serial.printf("【声纹】状态 → 被动录音（%s）\n",
                  recording ? "正在写卡" : "无SD卡，未录音");
}

// ---------------------------------------------------------------------------
// 耗时工作：只在监控任务里跑。
// ---------------------------------------------------------------------------
static void segment_ready()
{
    speaker_state_t st = s_state;

    if (s_enrolling) {
        // 录入期间 voiceprint_run() 负责累加到模板平均里。
        voiceprint_run(s_win, SEG_SAMPLES, nullptr, nullptr);
    } else if (st == SPEAKER_STATE_VERIFY || st == SPEAKER_STATE_MONITOR) {
        bool matched = false;
        float dist = 0.0f;
        if (voiceprint_run(s_win, SEG_SAMPLES, &matched, &dist) == 0) {
            // 被动录音期间全速比对；只有距离打印做限流，避免刷屏。
            static uint32_t last_d_print = 0;
            const uint32_t now_ms = millis();
            if (st == SPEAKER_STATE_VERIFY || now_ms - last_d_print >= 1000) {
                last_d_print = now_ms;
                Serial.printf("【声纹】距离 d=%.2f%s\n", dist, matched ? "  匹配" : "");
            }
            if (matched) {
                if (st == SPEAKER_STATE_VERIFY) {
                    // 主动录音优先级更高：主动在录时不启动被动录音。
                    if (recorder_active_writing()) {
                        Serial.println("【声纹】主动录音进行中，忽略本次匹配");
                    } else {
                        Serial.println("【声纹】匹配到用户，开始被动录音");
                        enter_monitor();
                        return;  // enter_monitor() 已经重置窗口
                    }
                } else {
                    s_last_target = millis();
                }
            }
        }
    }

    // 窗口左移一个 hop；feed() 会补齐尾部。
    memmove(s_win, s_win + SEG_HOP, (SEG_SAMPLES - SEG_HOP) * sizeof(int16_t));
    s_win_len = SEG_SAMPLES - SEG_HOP;
    s_win_ready = false;
}

static void check_timeouts()
{
    uint32_t now = millis();

    if (s_enrolling) {
        uint32_t elapsed = now - s_enroll_since;
        const uint32_t total_ms = (uint32_t)VOICEPRINT_ENROLL_SECONDS * 1000;

        // 每 1 秒倒数一次，直到 20s 录入结束。
        uint32_t sec = elapsed / 1000;
        if (sec != s_enroll_last_sec) {
            s_enroll_last_sec = sec;
            uint32_t remain = (elapsed >= total_ms) ? 0 : (total_ms - elapsed) / 1000;
            Serial.printf("【声纹】录入倒计时：剩余 %lu 秒（已捕获 %d 段）\n",
                          (unsigned long)remain, voiceprint_enroll_segments());
        }

        if (elapsed >= total_ms) {
            // voiceprint_enroll_finish() 自己会报告结果。
            voiceprint_enroll_finish();
            s_enrolling = false;
            reset_window();
        }
        return;
    }

    if (s_state == SPEAKER_STATE_VERIFY) {
        if (now - s_state_since >= VERIFY_TIMEOUT_MS) {
            Serial.println("【声纹】待验证超时");
            enter_idle();
        }
    } else if (s_state == SPEAKER_STATE_MONITOR) {
        // 被动录音停止条件（任一满足即停）：
        //  1) 有人说完后连续 10s 安静；
        //  2) 上次匹配到用户后 30s 内无再次匹配。
        if (now - s_last_speech >= PASSIVE_SILENCE_TIMEOUT_MS) {
            Serial.println("【声纹】连续静音超时，停止被动录音");
            enter_idle();
        } else if (now - s_last_target >= PASSIVE_NOMATCH_TIMEOUT_MS) {
            Serial.println("【声纹】长时间未匹配到用户，停止被动录音");
            enter_idle();
        }
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        check_timeouts();

        if (s_win_ready) {
            segment_ready();
            // 让出 CPU，推理突发时也不会饿死音频循环。
            vTaskDelay(1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

// ---------------------------------------------------------------------------
// 廉价工作：跑在音频路径上。
// ---------------------------------------------------------------------------
void speaker_monitor_feed(const int16_t *samples, size_t n)
{
    if (!s_enabled || s_win == nullptr || samples == nullptr || n == 0) {
        return;
    }

    // VAD 全态运行：逐帧能量统计，刷新「上次有人说话」的时刻。
    // mic_process() 每次只给几毫秒的小块，所以帧在多次调用间凑齐。
    size_t off = 0;
    while (off < n) {
        size_t space = VAD_FRAME_SAMPLES - s_vad_fill;
        size_t take = ((n - off) < space) ? (n - off) : space;
        memcpy(&s_vad_buf[s_vad_fill], &samples[off], take * sizeof(int16_t));
        s_vad_fill += take;
        off += take;

        if (s_vad_fill < VAD_FRAME_SAMPLES) {
            break;
        }
        s_vad_fill = 0;

        int64_t acc = 0;
        for (size_t i = 0; i < VAD_FRAME_SAMPLES; i++) {
            int32_t v = s_vad_buf[i];
            acc += (v < 0) ? -v : v;
        }
        float energy = ((float)acc / (float)VAD_FRAME_SAMPLES) / 32768.0f;
        if (energy >= VAD_ENERGY_THRESHOLD) {
            s_last_speech = millis();
            if (s_state == SPEAKER_STATE_IDLE && !s_enrolling) {
                if (++s_vad_run >= VAD_TRIGGER_FRAMES) {
                    // 没有模板就没有可比对的依据：不进待验证，也不打 d。
                    if (!voiceprint_has_template()) {
                        uint32_t now = millis();
                        if (now - s_no_tpl_hint >= 10000) {   // 10s 节流，避免刷屏
                            s_no_tpl_hint = now;
                            Serial.println("【声纹】未录入声纹，无法比对；手机发送 0x01 可录入");
                        }
                        s_vad_run = 0;
                    } else {
                        enter_verify();
                        break;
                    }
                }
            }
        } else if (s_state == SPEAKER_STATE_IDLE) {
            s_vad_run = 0;
        }
    }

    if (s_state == SPEAKER_STATE_IDLE && !s_enrolling) {
        return;  // 待机态不填推理窗口
    }

    // 窗口未满时累积样本。
    if (!s_win_ready && s_win_len < SEG_SAMPLES) {
        size_t space = SEG_SAMPLES - s_win_len;
        size_t take = (n < space) ? n : space;
        memcpy(&s_win[s_win_len], samples, take * sizeof(int16_t));
        s_win_len += take;
        if (s_win_len >= SEG_SAMPLES) {
            s_win_ready = true;
        }
    }
}

// ---------------------------------------------------------------------------
// 状态控制（由串口/BLE 命令处理器调用）
// ---------------------------------------------------------------------------
void speaker_monitor_force_idle()
{
    if (s_state == SPEAKER_STATE_MONITOR) {
        Serial.println("【声纹】主动录音抢占，停止被动录音");
        enter_idle();
    }
}

// ---------------------------------------------------------------------------
// 录入控制（模板采集）
// ---------------------------------------------------------------------------
bool speaker_monitor_enroll_start()
{
    if (!s_enabled || s_enrolling) {
        return false;
    }
    if (!voiceprint_enroll_start()) {
        return false;
    }

    // 录入与运行时状态机不能同时驱动窗口。
    if (s_state != SPEAKER_STATE_IDLE) {
        enter_idle();
    }

    s_enrolling = true;
    s_enroll_since = millis();
    s_enroll_last_sec = 0xFFFFFFFF;   // 让第 0 秒立刻打印倒计时
    reset_window();
    Serial.printf("【声纹】开始录入 - %d 秒，请持续说话\n",
                  VOICEPRINT_ENROLL_SECONDS);
    return true;
}

bool speaker_monitor_enroll_active()
{
    return s_enrolling;
}

int speaker_monitor_enroll_segments()
{
    return voiceprint_enroll_segments();
}

void speaker_monitor_enroll_abort()
{
    if (!s_enrolling) {
        return;
    }
    voiceprint_enroll_abort();
    s_enrolling = false;
    reset_window();
}

bool speaker_monitor_enroll_finish_now()
{
    if (!s_enrolling) {
        return false;
    }
    voiceprint_status_t r = voiceprint_enroll_finish();
    s_enrolling = false;
    reset_window();
    return r == VOICEPRINT_OK;
}

bool speaker_monitor_has_template()
{
    return voiceprint_has_template();
}

void speaker_monitor_erase_template()
{
    if (s_enrolling) {
        speaker_monitor_enroll_abort();
    }
    voiceprint_erase_template();
}

bool speaker_monitor_init()
{
    s_win = (int16_t *)heap_caps_malloc(SEG_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_win == nullptr) {
        s_win = (int16_t *)malloc(SEG_SAMPLES * sizeof(int16_t));
    }
    if (s_win == nullptr) {
        Serial.println("【声纹】窗口分配失败");
        return false;
    }
    memset(s_win, 0, SEG_SAMPLES * sizeof(int16_t));

    if (voiceprint_init() != VOICEPRINT_OK) {
        Serial.println("【声纹】声纹模块不可用，监控已禁用");
        return false;
    }

    // 录音模块由 app 初始化（录音 tap 需要独立于声纹监控）。

    s_state = SPEAKER_STATE_IDLE;
    s_state_since = millis();
    s_last_speech = millis();

    if (s_task == nullptr) {
        xTaskCreate(monitor_task, "speaker", VOICEPRINT_TASK_STACK_SIZE, nullptr,
                    VOICEPRINT_TASK_PRIORITY, &s_task);
    }

    s_enabled = true;
    Serial.println("【声纹】手机发送 0x01 即可开始声纹录入（20 秒）");
    Serial.println("【声纹】监控已启动");
    return true;
}
