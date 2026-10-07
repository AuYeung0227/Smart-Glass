#ifndef MIC_H
#define MIC_H

#include <Arduino.h>
#include <stdint.h>

// Callback type for audio data
typedef void (*mic_data_handler)(int16_t *data, size_t samples);

/**
 * @brief Initialize and start the microphone
 * @return true if successful, false otherwise
 */
bool mic_start();

/**
 * @brief Stop the microphone
 */
void mic_stop();

/**
 * @brief Check if mic is running
 * @return true if running
 */
bool mic_is_running();

/**
 * @brief Set callback for mic data
 * @param callback Function to call when audio data is ready
 */
void mic_set_callback(mic_data_handler callback);

/**
 * @brief Set a second, read-only tap on the RAW pre-gain I2S samples.
 *
 * Called once per mic_process() with the unprocessed 16-bit PDM samples,
 * before gain/despike/highpass/preemphasis. The callback must NOT modify the
 * buffer (the main path applies gain to it immediately afterwards) and must
 * return quickly or it will stall the audio path.
 */
void mic_set_analysis_callback(mic_data_handler callback);

/**
 * @brief Set a second, read-only tap on the PROCESSED samples (post gain/highpass).
 *
 * Called once per mic_process() with the same PCM that goes into the Opus
 * encoder, i.e. the audio that is actually transmitted over BLE. Used by the
 * recording module so its .opus files match the BLE audio exactly.
 *
 * The callback must NOT modify the buffer (the main path hands it to Opus
 * immediately afterwards) and must return quickly or it will stall audio.
 */
void mic_set_recording_callback(mic_data_handler callback);

/**
 * @brief Process mic data (call from main loop or task)
 */
void mic_process();

#endif // MIC_H
