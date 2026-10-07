#include "mfcc.h"

#include <math.h>
#include <string.h>

#include "config.h"

// All parameters mirror utils/mfcc.py in the training repo. Any drift here
// changes the feature statistics the enrolled template was built from.

#define FFT_SIZE MFCC_WIN_SIZE         // 512
#define FFT_BINS (FFT_SIZE / 2 + 1)    // 257
#define MEL_NUM MFCC_MEL_BANDS         // 20
#define FRAME_NUM MFCC_NUM_FRAMES      // 63

static bool s_ready = false;

static float s_hamming[FFT_SIZE];
static float s_melbank[MEL_NUM][FFT_BINS];
static float s_dct[MEL_NUM][MEL_NUM];
static float s_tw_re[FFT_SIZE / 2];
static float s_tw_im[FFT_SIZE / 2];

// FFT scratch, reused every frame.
static float s_re[FFT_SIZE];
static float s_im[FFT_SIZE];

// ---------------------------------------------------------------------------
// 512-point radix-2 FFT (in place)
// ---------------------------------------------------------------------------
static void fft_512(float *re, float *im)
{
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < FFT_SIZE; i++) {
        int bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }

    // Butterfly stages.
    for (int len = 2; len <= FFT_SIZE; len <<= 1) {
        int half = len >> 1;
        int step = FFT_SIZE / len;
        for (int i = 0; i < FFT_SIZE; i += len) {
            for (int k = 0; k < half; k++) {
                int tw = k * step;
                float wr = s_tw_re[tw];
                float wi = s_tw_im[tw];
                float ur = re[i + k];
                float ui = im[i + k];
                float xr = re[i + k + half];
                float xi = im[i + k + half];
                float vr = xr * wr - xi * wi;
                float vi = xr * wi + xi * wr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + half] = ur - vr;
                im[i + k + half] = ui - vi;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static float hz_to_mel(float hz)
{
    return 2595.0f * log10f(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
}

void mfcc_init()
{
    if (s_ready) {
        return;
    }

    // Hamming window: 0.53836 - 0.46164*cos(2*pi*n/(N-1))
    for (int n = 0; n < FFT_SIZE; n++) {
        s_hamming[n] = 0.53836f
                     - 0.46164f * cosf((2.0f * (float)M_PI * n) / (FFT_SIZE - 1));
    }

    // Triangular Mel filterbank over 20 Hz .. 8 kHz.
    float mel_min = hz_to_mel((float)MFCC_FREC_MIN);
    float mel_max = hz_to_mel((float)MFCC_FREC_MAX);
    int bins[MEL_NUM + 2];
    for (int i = 0; i <= MEL_NUM + 1; i++) {
        float m = mel_min + (mel_max - mel_min) * (float)i / (float)(MEL_NUM + 1);
        float hz = mel_to_hz(m);
        int b = (int)floorf(((float)FFT_BINS) * hz / (float)MFCC_FREC_MAX);
        if (b < 0) {
            b = 0;
        }
        if (b >= FFT_BINS) {
            b = FFT_BINS - 1;
        }
        bins[i] = b;
    }
    for (int m = 0; m < MEL_NUM; m++) {
        int f1 = bins[m], f2 = bins[m + 1], f3 = bins[m + 2];
        for (int k = 0; k < FFT_BINS; k++) {
            float v = 0.0f;
            if (k >= f1 && k < f2 && f2 > f1) {
                v = (float)(k - f1) / (float)(f2 - f1);
            } else if (k >= f2 && k < f3 && f3 > f2) {
                v = (float)(f3 - k) / (float)(f3 - f2);
            }
            s_melbank[m][k] = v;
        }
    }

    // Orthonormal DCT-II matrix (scipy norm="ortho").
    for (int k = 0; k < MEL_NUM; k++) {
        float alpha = (k == 0) ? sqrtf(1.0f / (float)MEL_NUM)
                               : sqrtf(2.0f / (float)MEL_NUM);
        for (int n = 0; n < MEL_NUM; n++) {
            s_dct[k][n] = alpha
                        * cosf((float)M_PI * (float)k * (2.0f * n + 1.0f)
                               / (2.0f * (float)MEL_NUM));
        }
    }

    // FFT twiddles: exp(-2*pi*i*k/N)
    for (int k = 0; k < FFT_SIZE / 2; k++) {
        float ang = -2.0f * (float)M_PI * (float)k / (float)FFT_SIZE;
        s_tw_re[k] = cosf(ang);
        s_tw_im[k] = sinf(ang);
    }

    s_ready = true;
}

// ---------------------------------------------------------------------------
// MFCC
// ---------------------------------------------------------------------------
void mfcc_compute(const int16_t *samples, size_t n, float *out)
{
    if (!s_ready) {
        mfcc_init();
    }

    memset(out, 0, sizeof(float) * MEL_NUM * FRAME_NUM);
    if (n < (size_t)FFT_SIZE) {
        return;
    }

    // Peak-normalize to 0.99, matching normalize_peak() in the reference.
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t a = (samples[i] < 0) ? -(int32_t)samples[i] : (int32_t)samples[i];
        if (a > peak) {
            peak = a;
        }
    }
    float max_abs = (float)peak / 32768.0f + 1e-9f;
    float scale = (max_abs > 0.99f) ? (0.99f / max_abs) : 1.0f;

    int n_frames = (int)((n - FFT_SIZE) / MFCC_HOP_SIZE) + 1;
    if (n_frames < 1) {
        return;
    }

    // Centre-crop or pad to FRAME_NUM, same as the reference.
    int start = 0;
    if (n_frames >= FRAME_NUM) {
        start = (n_frames - FRAME_NUM) / 2;
    }

    for (int f = 0; f < n_frames; f++) {
        int s = f * MFCC_HOP_SIZE;
        for (int k = 0; k < FFT_SIZE; k++) {
            size_t i = (size_t)(s + k);
            // Normalized, clipped sample.
            float cur = ((float)samples[i] / 32768.0f) * scale;
            if (cur > 1.0f) cur = 1.0f;
            if (cur < -1.0f) cur = -1.0f;

            // Pre-emphasis y[i] = x[i] - 0.97*x[i-1], causally equivalent to
            // applying it to the whole signal before framing.
            float prev = (i > 0) ? (((float)samples[i - 1] / 32768.0f) * scale) : 0.0f;
            if (prev > 1.0f) prev = 1.0f;
            if (prev < -1.0f) prev = -1.0f;

            s_re[k] = (cur - MFCC_PREEMPH * prev) * s_hamming[k];
            s_im[k] = 0.0f;
        }

        fft_512(s_re, s_im);

        // Power spectrum -> Mel -> log -> DCT-II.
        float mel[MEL_NUM];
        for (int m = 0; m < MEL_NUM; m++) {
            float acc = 0.0f;
            const float *fb = s_melbank[m];
            for (int b = 0; b < FFT_BINS; b++) {
                if (fb[b] != 0.0f) {
                    acc += (s_re[b] * s_re[b] + s_im[b] * s_im[b]) * fb[b];
                }
            }
            mel[m] = logf(acc + MFCC_EPSILON);
        }

        // Which output frame this lands in (skip frames outside the crop).
        int dst = f - start;
        if (dst < 0 || dst >= FRAME_NUM) {
            continue;
        }
        for (int k = 0; k < MEL_NUM; k++) {
            float acc = 0.0f;
            for (int m = 0; m < MEL_NUM; m++) {
                acc += s_dct[k][m] * mel[m];
            }
            out[k * FRAME_NUM + dst] = acc;
        }
    }
}

// ---------------------------------------------------------------------------
// Xi-Vector pooling: [mean, std, max, min] per band -> 80-d
// ---------------------------------------------------------------------------
void mfcc_xi_vector(const float *mfcc, float *xi)
{
    for (int band = 0; band < MEL_NUM; band++) {
        const float *row = &mfcc[band * FRAME_NUM];

        float sum = 0.0f, mx = row[0], mn = row[0];
        for (int t = 0; t < FRAME_NUM; t++) {
            sum += row[t];
            if (row[t] > mx) mx = row[t];
            if (row[t] < mn) mn = row[t];
        }
        float mean = sum / (float)FRAME_NUM;

        float var = 0.0f;
        for (int t = 0; t < FRAME_NUM; t++) {
            float d = row[t] - mean;
            var += d * d;
        }
        float sd = sqrtf(var / (float)FRAME_NUM);  // population std (ddof=0)

        xi[band * 4 + 0] = mean;
        xi[band * 4 + 1] = sd;
        xi[band * 4 + 2] = mx;
        xi[band * 4 + 3] = mn;
    }
}
