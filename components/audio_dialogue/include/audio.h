
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "peripheral_config.h"  // All pin definitions live in peripheral_config.h

#ifdef __cplusplus
extern "C" {
#endif

// Audio configuration (see AUDIO_SAMPLE_RATE in board_config.h)
#define AUDIO_CHANNELS          1       // Mono
#define AUDIO_BIT_WIDTH         32      // 32-bit slot for INMP441 (24-bit data padded)
#define VAD_SILENCE_TIMEOUT_MS  500     // Stop recording after 0.5s of silence
#define VAD_SILENCE_THRESHOLD   1000    // RMS amplitude below this = silence

// =============================================================================
// Core Audio Functions
// =============================================================================

/**
 * @brief Initialize I2S full-duplex bus (INMP441 mic + MAX98357A speaker).
 *        Must be called once before any audio_record/audio_play calls.
 *        Logs pin assignments and returns via ESP_LOGI.
 */
void audio_init(void);

/**
 * @brief Check if audio was initialized successfully.
 */
bool audio_is_initialized(void);

/**
 * @brief Record audio from INMP441 microphone with VAD silence detection.
 *        Stops automatically when silence exceeds VAD_SILENCE_TIMEOUT_MS.
 * @param buffer   Output buffer for raw PCM-16 data (mono, 16 kHz)
 * @param max_len  Maximum bytes to write to buffer
 * @return Actual bytes recorded, or 0 on error
 */
int audio_record(uint8_t *buffer, int max_len);

/**
 * @brief Play raw PCM-16 audio data through MAX98357A speaker.
 * @param data  Pointer to PCM-16 mono audio data (16 kHz)
 * @param len   Length of data in bytes
 */
void audio_play(const uint8_t *data, int len);

/**
 * @brief Play raw PCM-16 audio data through MAX98357A speaker in streaming mode without appending silence.
 * @param data  Pointer to PCM-16 mono audio data (16 kHz)
 * @param len   Length of data in bytes
 */
void audio_play_chunk(const uint8_t *data, int len);

/**
 * @brief Feed raw PCM audio data from network into the playback RingBuffer.
 * @param data  Pointer to PCM-16 mono audio data
 * @param len   Length of data in bytes
 */
void audio_feed_ringbuffer(const uint8_t *data, size_t len);

// =============================================================================
// Hardware Test Functions (Phase 0 Validation)
// =============================================================================

/**
 * @brief TEST: Play a 440 Hz sine wave for ~2 seconds through the speaker.
 *        Pass: you hear a clear 440 Hz tone (musical note A4).
 *        Fail: silence → check MAX98357A wiring and GPIO 47.
 */
void audio_test_play_sine(void);

/**
 * @brief TEST: Record 3 seconds from the mic and save to SD card.
 *        File saved to: /sdcard/test_mic.raw
 *        Verify with Audacity: File → Import → Raw, 16 kHz, 16-bit, mono
 *        Pass: file ~96 KB exists, you can hear audio in Audacity.
 *        Fail: file missing/empty → check INMP441 wiring and GPIO 48.
 */
void audio_test_record_to_sd(void);

/**
 * @brief TEST: Toggle manual recording. Click once to start, again to stop and play.
 */
void audio_test_record_toggle(void);
int audio_test_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_H
