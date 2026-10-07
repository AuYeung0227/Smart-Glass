#ifndef VOICEPRINT_H
#define VOICEPRINT_H

#include <stddef.h>
#include <stdint.h>

// Template-based speaker voiceprint.
//
// The Xi-Vector (MFCC band statistics) is a deterministic feature, so enrolling
// a speaker is pure averaging - no gradient training, no host PC, no model file.
// A template is 640 B in SPIFFS: per-dimension mean and spread.
//
// Matching scores a fresh segment by normalized Euclidean distance:
//     d = sqrt(mean_d( ((x_d - mean_d) / sigma_d)^2 ))
// so the distance is measured in units of the speaker's own utterance-to-
// utterance variation. d < VOICEPRINT_MATCH_THRESHOLD means "same speaker".

typedef enum {
    VOICEPRINT_OK = 0,
    VOICEPRINT_ERR_SPIFFS = -1,       // SPIFFS mount failed
    VOICEPRINT_ERR_NO_TEMPLATE = -2,  // nothing enrolled yet
    VOICEPRINT_ERR_SAVE = -3,         // template write failed
    VOICEPRINT_ERR_NOT_ENOUGH = -4,   // too few segments to enroll
} voiceprint_status_t;

/** @brief Mount SPIFFS and load an existing template, if any. */
voiceprint_status_t voiceprint_init();

/** @brief True once voiceprint_init() succeeded (SPIFFS mounted). */
bool voiceprint_ready();

/** @brief True if a speaker template is enrolled and loaded. */
bool voiceprint_has_template();

/** @brief Delete the stored template. */
void voiceprint_erase_template();

// --- Enrollment ---------------------------------------------------------

/** @brief Begin collecting Xi-Vectors. Returns false if already enrolling. */
bool voiceprint_enroll_start();

/** @brief True while enrollment is collecting. */
bool voiceprint_is_enrolling();

/** @brief Segments collected so far in this enrollment. */
int voiceprint_enroll_segments();

/**
 * @brief Average the collected Xi-Vectors and persist the template.
 * @return VOICEPRINT_OK, or ERR_NOT_ENOUGH / ERR_SAVE.
 */
voiceprint_status_t voiceprint_enroll_finish();

/** @brief Discard the in-progress enrollment. */
void voiceprint_enroll_abort();

// --- Matching -----------------------------------------------------------

/**
 * @brief Score one 2-second segment against the enrolled template.
 *
 * While enrolling, this instead accumulates the segment's Xi-Vector.
 *
 * @param matched   out: true if within VOICEPRINT_MATCH_THRESHOLD
 * @param distance  out: normalized distance (may be null)
 * @return 0 on success, negative if no template / not ready.
 */
int voiceprint_run(const int16_t *samples, size_t n, bool *matched,
                   float *distance);

#endif // VOICEPRINT_H
