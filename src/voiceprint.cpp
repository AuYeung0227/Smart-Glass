#include "voiceprint.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "mfcc.h"

// Per-dimension spread floor. Xi-Vector components span roughly [-25, 10]; a
// dimension that barely moves during enrollment must not blow up the distance.
static const float kSigmaFloor = 0.5f;

static bool s_ready = false;          // SPIFFS mounted
static bool s_have_template = false;
static bool s_enrolling = false;

static float s_mean[MFCC_XI_DIM];
static float s_sigma[MFCC_XI_DIM];

// Enrollment accumulators.
static float s_sum[MFCC_XI_DIM];
static float s_sumsq[MFCC_XI_DIM];
static int s_count = 0;

static float s_mfcc[MFCC_MEL_BANDS * MFCC_NUM_FRAMES];
static float s_xi[MFCC_XI_DIM];

bool voiceprint_ready()
{
    return s_ready;
}

bool voiceprint_has_template()
{
    return s_have_template;
}

static bool load_template()
{
    File f = SPIFFS.open(VOICEPRINT_TEMPLATE_PATH, FILE_READ);
    if (!f) {
        return false;
    }
    const size_t want = sizeof(float) * MFCC_XI_DIM;
    if (f.size() != want * 2) {
        f.close();
        return false;
    }
    bool ok = f.read((uint8_t *)s_mean, want) == want
           && f.read((uint8_t *)s_sigma, want) == want;
    f.close();
    return ok;
}

static bool save_template()
{
    File f = SPIFFS.open(VOICEPRINT_TEMPLATE_PATH, FILE_WRITE);
    if (!f) {
        return false;
    }
    const size_t want = sizeof(float) * MFCC_XI_DIM;
    bool ok = f.write((const uint8_t *)s_mean, want) == want
           && f.write((const uint8_t *)s_sigma, want) == want;
    f.close();
    return ok;
}

void voiceprint_erase_template()
{
    if (!s_ready) {
        return;
    }
    SPIFFS.remove(VOICEPRINT_TEMPLATE_PATH);
    s_have_template = false;
    Serial.println("【声纹】模板已擦除");
}

voiceprint_status_t voiceprint_init()
{
    if (!s_ready) {
        mfcc_init();
        if (!SPIFFS.begin(true)) {
            Serial.println("【声纹】SPIFFS 挂载失败");
            return VOICEPRINT_ERR_SPIFFS;
        }
        s_ready = true;
    }

    if (!s_have_template) {
        s_have_template = load_template();
    }
    // 开机先报声纹数量，让用户明确「旧模板是否还在」（SPIFFS 烧录时不会擦除）。
    Serial.printf("【声纹】声纹数量：%s\n",
                  s_have_template ? "已录入 1 个" : "还未录入");
    return VOICEPRINT_OK;
}

// ---------------------------------------------------------------------------
// Enrollment
// ---------------------------------------------------------------------------
bool voiceprint_enroll_start()
{
    if (s_enrolling || !s_ready) {
        return false;
    }
    for (int i = 0; i < MFCC_XI_DIM; i++) {
        s_sum[i] = 0.0f;
        s_sumsq[i] = 0.0f;
    }
    s_count = 0;
    s_enrolling = true;
    Serial.println("【声纹】录入已开始，请持续说话");
    return true;
}

bool voiceprint_is_enrolling()
{
    return s_enrolling;
}

int voiceprint_enroll_segments()
{
    return s_count;
}

void voiceprint_enroll_abort()
{
    if (!s_enrolling) {
        return;
    }
    s_enrolling = false;
    s_count = 0;
    Serial.println("【声纹】录入已中止");
}

voiceprint_status_t voiceprint_enroll_finish()
{
    s_enrolling = false;

    if (s_count < VOICEPRINT_ENROLL_MIN_SEGMENTS) {
        Serial.printf("【声纹】录入失败 - 声纹录入失败 "
                      "（%d 段，至少需要 %d 段）\n",
                      s_count, VOICEPRINT_ENROLL_MIN_SEGMENTS);
        s_count = 0;
        return VOICEPRINT_ERR_NOT_ENOUGH;
    }

    for (int i = 0; i < MFCC_XI_DIM; i++) {
        float mean = s_sum[i] / (float)s_count;
        float var = s_sumsq[i] / (float)s_count - mean * mean;
        if (var < 0.0f) {
            var = 0.0f;
        }
        float sd = sqrtf(var);
        if (sd < kSigmaFloor) {
            sd = kSigmaFloor;
        }
        s_mean[i] = mean;
        s_sigma[i] = sd;
    }
    int used = s_count;
    s_count = 0;

    if (!save_template()) {
        Serial.println("【声纹】录入失败 - 声纹录入失败（模板保存出错）");
        return VOICEPRINT_ERR_SAVE;
    }

    s_have_template = true;
    Serial.printf("【声纹】录入完成 - 声纹录入已完成 "
                  "（%d 段，模板已保存）\n", used);
    return VOICEPRINT_OK;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------
int voiceprint_run(const int16_t *samples, size_t n, bool *matched,
                   float *distance)
{
    if (!s_ready || samples == nullptr) {
        return -1;
    }

    mfcc_compute(samples, n, s_mfcc);
    mfcc_xi_vector(s_mfcc, s_xi);

    // While enrolling, this call is a contribution to the average instead.
    if (s_enrolling) {
        for (int i = 0; i < MFCC_XI_DIM; i++) {
            s_sum[i] += s_xi[i];
            s_sumsq[i] += s_xi[i] * s_xi[i];
        }
        s_count++;
        if (matched != nullptr) {
            *matched = false;
        }
        if (distance != nullptr) {
            *distance = 0.0f;
        }
        return 0;
    }

    if (!s_have_template) {
        return VOICEPRINT_ERR_NO_TEMPLATE;
    }

    float acc = 0.0f;
    for (int i = 0; i < MFCC_XI_DIM; i++) {
        float z = (s_xi[i] - s_mean[i]) / s_sigma[i];
        acc += z * z;
    }
    const float d = sqrtf(acc / (float)MFCC_XI_DIM);

    if (distance != nullptr) {
        *distance = d;
    }
    if (matched != nullptr) {
        *matched = (d < VOICEPRINT_MATCH_THRESHOLD);
    }
    return 0;
}
