#ifndef SPEAKER_MONITOR_H
#define SPEAKER_MONITOR_H

#include <stddef.h>
#include <stdint.h>

// 三态声纹监控：
//
//   待机 IDLE    - 仅能量 VAD，不做 MFCC、不做推理（最省电）
//   待验证 VERIFY - 在 30s 滚动缓存上全速推理
//   被动录音 MONITOR - 写 SD 卡，全速推理；满足停止条件（10s 静音 / 30s 无匹配）即停
//
// 声纹推理（约 65ms）绝不跑在音频路径上：feed() 只做 memcpy，
// 推理与所有 SD 操作都由专用任务负责。三种状态下 Opus/BLE 链路都不受影响。

typedef enum {
    SPEAKER_STATE_IDLE = 0,
    SPEAKER_STATE_VERIFY = 1,
    SPEAKER_STATE_MONITOR = 2,
} speaker_state_t;

/**
 * @brief Load the model and start the worker task.
 * @return false if the voiceprint model could not be loaded; the monitor then
 *         stays inert and the audio path is unaffected.
 */
bool speaker_monitor_init();

/** @brief Feed raw pre-gain PCM. Non-blocking; call from the mic analysis tap. */
void speaker_monitor_feed(const int16_t *samples, size_t n);

/** @brief 当前状态，用于日志或 LED 指示。 */
speaker_state_t speaker_monitor_state();

/** @brief 当前状态的中文名称（"待机"/"待验证"/"被动录音"/"录入"）。 */
const char *speaker_monitor_state_name();

/**
 * @brief 强制回到待机态（若正在被动录音则停录落盘）。
 *
 * 供主动录音（手机 0x10）抢占使用：主动录音优先级更高，开始时先停掉被动录音。
 */
void speaker_monitor_force_idle();

// --- Enrollment (template capture, no training) -------------------------

/**
 * @brief Start capturing the speaker template.
 *
 * Records VOICEPRINT_ENROLL_SECONDS of audio, averaging the Xi-Vector of every
 * 2 s segment, then persists the template automatically. Requires
 * VOICEPRINT_ENROLL_MIN_SEGMENTS segments or it fails.
 *
 * @return false if the monitor is disabled or already enrolling.
 */
bool speaker_monitor_enroll_start();

/** @brief True while the enrollment window is open. */
bool speaker_monitor_enroll_active();

/** @brief Segments captured so far in this enrollment. */
int speaker_monitor_enroll_segments();

/** @brief Cancel enrollment, discarding what was captured. */
void speaker_monitor_enroll_abort();

/** @brief Finish enrollment early instead of waiting out the window. */
bool speaker_monitor_enroll_finish_now();

/** @brief True if a template is enrolled and loaded. */
bool speaker_monitor_has_template();

/** @brief Delete the stored template. */
void speaker_monitor_erase_template();

#endif // SPEAKER_MONITOR_H
