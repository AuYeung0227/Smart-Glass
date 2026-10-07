#ifndef MFCC_H
#define MFCC_H

#include <stddef.h>
#include <stdint.h>

// MFCC front-end for the speaker voiceprint, matching utils/mfcc.py in the
// training repo. Parameters live in config.h (MFCC_* macros).
//
// Pipeline: int16 -> normalize -> peak-normalize -> pre-emphasis (0.97)
//           -> 512/256 framing + Hamming -> power spectrum
//           -> 20 Mel bands -> log -> DCT-II -> centre-crop/pad to 63 frames.

/**
 * @brief Precompute the Mel filterbank and DCT matrix. Call once at startup.
 */
void mfcc_init();

/**
 * @brief Compute the MFCC matrix for one analysis segment.
 * @param samples  int16 PCM, expected MFCC_SEGMENT_SAMPLES long
 * @param n        number of samples available
 * @param out      destination, MFCC_MEL_BANDS * MFCC_NUM_FRAMES floats,
 *                 band-major (out[band * MFCC_NUM_FRAMES + frame])
 */
void mfcc_compute(const int16_t *samples, size_t n, float *out);

/**
 * @brief Four-statistic Xi-Vector pooling over the MFCC matrix.
 * @param mfcc  matrix from mfcc_compute()
 * @param xi    destination, MFCC_XI_DIM floats
 *              [mean, std, max, min] per band, band-major
 */
void mfcc_xi_vector(const float *mfcc, float *xi);

#endif // MFCC_H
