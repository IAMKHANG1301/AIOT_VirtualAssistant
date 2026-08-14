
// =============================================================================
// audio.cpp — I2S Full-Duplex Audio Driver
// Hardware: INMP441 Microphone (RX) + MAX98357A Speaker Amp (TX)
// Protocol: Standard Philips I2S, 16kHz Mono
// ESP-IDF: v5.x new I2S driver (driver/i2s_std.h)
// =============================================================================
#include "audio.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network.h"
#include "esp_websocket_client.h"
#include "freertos/ringbuf.h"
#include <math.h>
#include <string.h>
#include "peripheral_config.h"
#include <stdio.h>

static const char *TAG = "AUDIO";

// I2S channel handles (full-duplex, both linked to I2S_NUM_0)
static i2s_chan_handle_t s_tx_chan = NULL;   // Speaker (MAX98357A)
static i2s_chan_handle_t s_rx_chan = NULL;   // Microphone (INMP441)
static bool              s_initialized = false;

volatile uint32_t g_last_play_time_ms = 0;
volatile bool g_is_audio_playing = false;
// Cờ toàn cục: ESP32 đang chờ AI trả lời (không được fire stop_audio nữa)
volatile bool g_is_waiting_for_ai = false;
static RingbufHandle_t s_audio_ringbuf = NULL;

// =============================================================================
// audio_init — Initialize I2S Full-Duplex Bus
// =============================================================================
void audio_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Audio already initialized, skipping.");
        return;
    }

    // Enable ESP-IDF internal I2S driver debug logs
    esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
    esp_log_level_set("i2s_std", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Initializing I2S full-duplex audio...");
    ESP_LOGI(TAG, "  BCLK  = GPIO %d (shared MIC+SPK)", I2S_SPK_BCLK_PIN);
    ESP_LOGI(TAG, "  MIC   = GPIO %d (RX ← INMP441 SD)",  I2S_MIC_SD_PIN);
    ESP_LOGI(TAG, "  SPK   = GPIO %d (TX → MAX98357A DIN)", I2S_SPK_DIN_PIN);
    ESP_LOGI(TAG, "  MIC   = GPIO %d (RX ← INMP441 SD)",  I2S_MIC_SD_PIN);

    // -------------------------------------------------------------------------
    // Step 1: Create full-duplex channel pair on I2S_NUM_0
    //   Passing non-NULL for both tx and rx creates a linked duplex pair:
    //   they share the same I2S controller and clock signals.
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // Step 1: Create full-duplex channel pair on I2S_NUM_0
    //   Passing non-NULL for both tx and rx creates a linked duplex pair:
    //   they share the same I2S controller and clock signals.
    // -------------------------------------------------------------------------
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Zero-fill TX buffer when idle (prevents noise)
    
    // Tăng bộ đệm DMA để tránh khựng do thiếu hụt sample tạm thời
    chan_cfg.dma_desc_num = 6;     // Dùng 6 khối DMA
    chan_cfg.dma_frame_num = 512;  // Mỗi khối chứa 512 frames (Tổng = 24KB SRAM, ~192ms audio buffer)

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return;
    }

    // -------------------------------------------------------------------------
    // Step 2: Configure Standard I2S (Philips) mode
    //   - Sample rate: 16 kHz (suitable for voice)
    //   - Slot: 32-bit word, 24-bit data (matches INMP441 output format)
    //   - Mode: Mono — INMP441 with L/R=GND outputs on LEFT channel only
    //   - Full GPIO config is specified here; RX reuses BCLK+WS from TX.
    // -------------------------------------------------------------------------
    i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,  // 32-bit slot (INMP441 is 24-bit padded)
                        I2S_SLOT_MODE_STEREO),     // Stereo — ensures 50% duty cycle WS clock for MAX98357A
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,                 
            .bclk = (gpio_num_t)I2S_SPK_BCLK_PIN,      
            .ws   = (gpio_num_t)I2S_SPK_LRC_PIN,        
            .dout = (gpio_num_t)I2S_SPK_DIN_PIN,  
            .din  = (gpio_num_t)I2S_MIC_SD_PIN,   // Data in ← INMP441 SD
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // -------------------------------------------------------------------------
    // Step 3: Initialize TX channel (speaker) — sets up clock + GPIO
    // -------------------------------------------------------------------------
    err = i2s_channel_init_std_mode(s_tx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX channel init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // -------------------------------------------------------------------------
    // Step 4: Initialize RX channel (microphone) — reuses TX clock
    // -------------------------------------------------------------------------
    err = i2s_channel_init_std_mode(s_rx_chan, &i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX channel init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // -------------------------------------------------------------------------
    // Step 5: Enable both channels (starts DMA transfers)
    // -------------------------------------------------------------------------
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    // Khởi động 400KB RingBuffer trên PSRAM
    s_audio_ringbuf = xRingbufferCreate(400 * 1024, RINGBUF_TYPE_BYTEBUF);
    if (!s_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to create 400KB RingBuffer in PSRAM!");
    } else {
        extern void audio_play_task_func(void *arg);
        // Chạy trên Core 1 để không tranh CPU với WiFi/WebSocket (Core 0)
        // Giải quyết vấn đề âm thanh bị khựng do Core 0 bị quá tải
        xTaskCreatePinnedToCore(audio_play_task_func, "audio_play_task", 4096, NULL, 6, NULL, 1);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio I2S initialized OK ✓  (16kHz, 32-bit slot, Stereo Mode)");
    return;

cleanup:
    if (s_tx_chan) { i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    if (s_rx_chan) { i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    ESP_LOGE(TAG, "Audio initialization FAILED — check wiring!");
}

// =============================================================================
bool audio_is_initialized(void) {
    return s_initialized;
}

// =============================================================================
// audio_record — Record with Voice Activity Detection (VAD)
// =============================================================================
int audio_record(uint8_t *buffer, int max_len) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_record: not initialized");
        return 0;
    }

    ESP_LOGI(TAG, "Recording started (max %d bytes)...", max_len);

    // Each I2S read gives 32-bit samples; we downsample to 16-bit for storage
    const int DMA_BUF_SAMPLES = 256;
    const int DMA_BUF_BYTES   = DMA_BUF_SAMPLES * sizeof(int32_t);
    int32_t  *dma_buf         = (int32_t *)malloc(DMA_BUF_BYTES);
    if (!dma_buf) {
        ESP_LOGE(TAG, "audio_record: malloc failed");
        return 0;
    }

    int       total_written     = 0;
    int64_t   silence_start_ms  = 0;
    bool      silence_started   = false;
    int16_t  *out               = (int16_t *)buffer;
    int       max_samples_16bit = max_len / sizeof(int16_t);

    while (total_written < max_samples_16bit) {
        // Read 32-bit samples from INMP441 via I2S DMA
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, dma_buf, DMA_BUF_BYTES,
                                         &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) continue;
        int samples_read = bytes_read / sizeof(int32_t);
        
        // STEREO mode: xen kẽ LEFT, RIGHT. INMP441 chỉ phát trên LEFT, RIGHT = 0
        for (int i = 0; i < samples_read && total_written < max_samples_16bit; i += 2) { // i += 2 để skip RIGHT
            // >> 14: Giữ lại 18-bit cao, cho ra giá trị ~2x to hơn so với >>16
            // Lấy độ 24-bit data của INMP441
            int32_t value = dma_buf[i] >> 14;
            int16_t sample_16 = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? (int16_t)-INT16_MAX : (int16_t)value;
            
            out[total_written++] = sample_16;
        }

        // Push-to-talk: Kiểm tra nếu nhả nút BOOT thì dừng ghi âm ngay
        if (gpio_get_level(BUTTON_PIN) == 1) {
            ESP_LOGI(TAG, "Nút đã nhả, kết thúc ghi âm.");
            break;
        }
    }

    free(dma_buf);
    int bytes_recorded = total_written * sizeof(int16_t);
    ESP_LOGI(TAG, "Recording done: %d bytes (%.1f s at 16kHz mono PCM-16)",
             bytes_recorded, (float)total_written / AUDIO_SAMPLE_RATE);
    return bytes_recorded;
}

void audio_play(const uint8_t *data, int len) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_play: not initialized");
        return;
    }
    if (!data || len <= 0) return;
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "Playing %d bytes (%.1f s at 16kHz mono PCM-16)...",
             len, (float)(len / 2) / AUDIO_SAMPLE_RATE);

    // Upscale 16-bit PCM → 32-bit I2S samples (MAX98357A prefers full-scale signal)
    const int16_t *src          = (const int16_t *)data;
    int            sample_count = len / sizeof(int16_t);
    const int      CHUNK        = 128; // 128 mono samples -> 256 stereo samples
    int32_t        tx_buf[CHUNK * 2];

    for (int offset = 0; offset < sample_count; offset += CHUNK) {
        int n = (offset + CHUNK < sample_count) ? CHUNK : (sample_count - offset);
        for (int i = 0; i < n; i++) {
            int32_t val = (int32_t)src[offset + i] << 16;
            tx_buf[2 * i]     = val; // Left channel
            tx_buf[2 * i + 1] = val; // Right channel
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t),
                          &bytes_written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write (play) failed: %s (written %zu)", esp_err_to_name(err), bytes_written);
        }
    }

    // Flush with silence to prevent pop at end
    int32_t silence[128] = {0};
    size_t w = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write (silence) failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Playback complete ✓");
}

void audio_play_chunk(const uint8_t *data, int len) {
    if (!s_initialized || !data || len <= 0) return;
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    const int16_t *src          = (const int16_t *)data;
    int            sample_count = len / sizeof(int16_t);
    const int      CHUNK        = 128;
    int32_t        tx_buf[CHUNK * 2];

    for (int offset = 0; offset < sample_count; offset += CHUNK) {
        int n = (offset + CHUNK < sample_count) ? CHUNK : (sample_count - offset);
        for (int i = 0; i < n; i++) {
            int32_t val = (int32_t)src[offset + i] << 16;
            tx_buf[2 * i]     = val; 
            tx_buf[2 * i + 1] = val; 
        }
        size_t bytes_written = 0;
        i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t), &bytes_written, pdMS_TO_TICKS(200));
    }
    g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// =============================================================================
// audio_test_play_sine — TEST: Play 440 Hz sine wave for 2 seconds
//   EXPECTED OUTPUT: You hear a clear 440 Hz tone (note A4) from the speaker.
//   HOW TO VERIFY: Just listen — should be a steady, clean buzzing tone.
//   FAIL SYMPTOM: Silence → check MAX98357A DIN wire on GPIO 47.
// =============================================================================
void audio_test_play_sine(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_test_play_sine: call audio_init() first!");
        return;
    }

    ESP_LOGI(TAG, "=== TEST: Playing 440 Hz sine wave for 2 seconds ===");

    const int FREQ_HZ      = 440;
    const int DURATION_S   = 1;      // Reduce to 1 second
    const int TOTAL_SMPLS  = AUDIO_SAMPLE_RATE * DURATION_S;
    const int CHUNK        = 128; 
    const int AMPLITUDE    = 4000;   // Giảm volume (từ 16000 xuống 4000) để tránh tụt áp
    int32_t   tx_buf[CHUNK * 2];
    double    phase        = 0.0;
    double    phase_inc    = 2.0 * M_PI * FREQ_HZ / AUDIO_SAMPLE_RATE;

    for (int offset = 0; offset < TOTAL_SMPLS; offset += CHUNK) {
        int n = (offset + CHUNK < TOTAL_SMPLS) ? CHUNK : (TOTAL_SMPLS - offset);
        for (int i = 0; i < n; i++) {
            int16_t sample = (int16_t)(AMPLITUDE * sin(phase));
            int32_t val = (int32_t)sample << 16;
            tx_buf[2 * i]     = val; // Left channel
            tx_buf[2 * i + 1] = val; // Right channel
            phase += phase_inc;
        }
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, tx_buf, n * 2 * sizeof(int32_t),
                          &bytes_written, pdMS_TO_TICKS(500));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write (sine) failed: %s (written %zu)", esp_err_to_name(err), bytes_written);
        }
    }

    // Flush silence to prevent pop
    int32_t silence[128] = {0};
    size_t w = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write (sine silence) failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "=== TEST COMPLETE: Did you hear 440 Hz tone? ===");
}

// =============================================================================
// audio_test_record_to_sd — TEST: Record 3s from mic → /sdcard/test_mic.raw
//   EXPECTED OUTPUT: /sdcard/test_mic.raw (~96 KB, raw PCM-16 mono 16kHz)
//   HOW TO VERIFY: Copy to PC → open in Audacity:
//     File > Import > Raw Data > 16kHz, 16-bit, Mono, Little-Endian
//   PASS: You hear your voice in the recording.
//   FAIL: File missing → check INMP441 SD wire on GPIO 48.
//         File full of zeros → check INMP441 VDD (3.3V) and L/R → GND.
// =============================================================================
void audio_test_record_to_sd(void) {
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_test_record_to_sd: call audio_init() first!");
        return;
    }

    ESP_LOGI(TAG, "=== Recording from mic (auto stops after 0.5s silence) ===");
    
    // Ghi tối đa 10 giây

    const int BUF_SIZE = AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t);

    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        pcm_buf = (uint8_t *)malloc(BUF_SIZE);
        if (!pcm_buf) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for recording buffer", BUF_SIZE);
            return;
        }
    }

    // Use the built-in VAD record function
    int bytes_recorded = audio_record(pcm_buf, BUF_SIZE);
    
    if (bytes_recorded <= 0) {
        ESP_LOGE(TAG, "Recording failed or empty.");
        free(pcm_buf);
        return;
    }

    // Write raw PCM to SD card for UI testing
    const char *filepath = "/sdcard/test_mic.raw";
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing! SD mounted?", filepath);
        free(pcm_buf);
        return;
    }

    size_t written = fwrite(pcm_buf, 1, bytes_recorded, f);
    fclose(f);
    free(pcm_buf);
    if (written == (size_t)bytes_recorded) {
        ESP_LOGI(TAG, "Saved RAW PCM file: %d bytes to %s", (int)written, filepath);
    } else {
        ESP_LOGE(TAG, "Only wrote %d/%d bytes to SD", (int)written, bytes_recorded);
    }
}

// =============================================================================
// audio_test_record_toggle - TEST: Toggle manual recording from mic
// =============================================================================
bool g_is_test_audio_active = false;
static uint8_t *s_test_pcm_buf = NULL;
static int s_test_pcm_written = 0;
static TaskHandle_t s_test_record_task_handle = NULL;

static void test_record_task(void *arg) {

    const int MAX_TEST_BUF_SIZE = AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t);

    s_test_pcm_buf = (uint8_t *)heap_caps_malloc(MAX_TEST_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_test_pcm_buf) s_test_pcm_buf = (uint8_t *)malloc(MAX_TEST_BUF_SIZE);
    
    if (!s_test_pcm_buf) {
        ESP_LOGE(TAG, "test_record_task: malloc failed");
        g_is_test_audio_active = false;
        s_test_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "=== Manual Recording started (max %ds) ===", MAX_RECORD_SECONDS);

    const int DMA_BUF_SAMPLES = 256;
    const int DMA_BUF_BYTES   = DMA_BUF_SAMPLES * sizeof(int32_t);
    int32_t  *dma_buf         = (int32_t *)malloc(DMA_BUF_BYTES);
    if (!dma_buf) {
        free(s_test_pcm_buf);
        s_test_pcm_buf = NULL;
        g_is_test_audio_active = false;
        s_test_record_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_test_pcm_written = 0;
    int max_samples_16bit = MAX_TEST_BUF_SIZE / sizeof(int16_t);
    int16_t *out = (int16_t *)s_test_pcm_buf;

    while (g_is_test_audio_active && s_test_pcm_written < max_samples_16bit) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, dma_buf, DMA_BUF_BYTES, &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) continue;
        
        int samples_read = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples_read && s_test_pcm_written < max_samples_16bit; i += 2) {
            int32_t value = dma_buf[i] >> 14;
            out[s_test_pcm_written++] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? (int16_t)-INT16_MAX : (int16_t)value;
        }
    }

    free(dma_buf);
    g_is_test_audio_active = false;
    
    int bytes_recorded = s_test_pcm_written * sizeof(int16_t);
    if (bytes_recorded > 0) {
        ESP_LOGI(TAG, "=== Uploading %d bytes to Cloud ===", bytes_recorded);
        network_upload_audio_to_cloud(s_test_pcm_buf, bytes_recorded);
    } else {
        ESP_LOGW(TAG, "No audio recorded to upload.");
    }

    free(s_test_pcm_buf);
    s_test_pcm_buf = NULL;
    ESP_LOGI(TAG, "=== TEST COMPLETE ===");
    s_test_record_task_handle = NULL;
    vTaskDelete(NULL);
}

void audio_test_record_toggle(void) {
    if (!s_initialized) return;
    
    if (g_is_test_audio_active) {
        ESP_LOGI(TAG, "audio_test_record_toggle: Stopping record");
        g_is_test_audio_active = false;
    } else {
        ESP_LOGI(TAG, "audio_test_record_toggle: Starting record");
        g_is_test_audio_active = true;
        // 8192 is enough for TLS, 16384 might fail to allocate due to memory fragmentation
        BaseType_t ret = xTaskCreate(test_record_task, "test_rec_tsk", 8192, NULL, 5, &s_test_record_task_handle);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create test_record_task (err=%d). Insufficient memory?", ret);
            g_is_test_audio_active = false;
        }
    }
}

int audio_test_is_recording(void) {
    return g_is_test_audio_active ? 1 : 0;
}

// =============================================================================
// audio_feed_ringbuffer & audio_play_task_func
// =============================================================================
void audio_feed_ringbuffer(const uint8_t *data, size_t len) {
    if (s_audio_ringbuf) {
        xRingbufferSend(s_audio_ringbuf, (void*)data, len, pdMS_TO_TICKS(100));
    }
}

void audio_play_task_func(void *arg) {
    ESP_LOGI(TAG, "Audio Play Task Started (with Pre-buffering).");
    bool prebuffering = true;
    size_t last_used_size = 0;
    int idle_ticks = 0;
    
    while (1) {
        if (prebuffering) {
            size_t free_size = xRingbufferGetCurFreeSize(s_audio_ringbuf);
            size_t used_size = (400 * 1024) - free_size;
            
            if (used_size > 120000) { // 120KB (~3.75 giay audio)
                prebuffering = false;
                ESP_LOGI(TAG, "Pre-buffering complete (%zu bytes). Starting playback.", used_size);
            } else if (used_size > 2000) { // Co data trong buffer
                if (used_size == last_used_size) {
                    idle_ticks++;
                    if (idle_ticks > 75) { // 1500ms khong co data moi -> chac la het file
                        prebuffering = false;
                        ESP_LOGI(TAG, "Pre-buffering timeout/EOF (%zu bytes). Starting playback.", used_size);
                    }
                } else {
                    idle_ticks = 0;
                }
            } else {
                idle_ticks = 0;
            }
            last_used_size = used_size;
            
            if (prebuffering) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }

        size_t item_size;
        // Kéo 16KB mỗi lần (tăng từ 4096) để giảm số lần chuyển tiếp, chống giật
        void *item = xRingbufferReceiveUpTo(s_audio_ringbuf, &item_size, pdMS_TO_TICKS(20), 16384);
        if (item) {
            g_is_audio_playing = true;
            g_last_play_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            audio_play_chunk((const uint8_t*)item, item_size);
            vRingbufferReturnItem(s_audio_ringbuf, item);
        } else {
            g_is_audio_playing = false;
            prebuffering = true; // Bat lai che do pre-buffering cho cau hoi tiep theo
            idle_ticks = 0;
        }
    }
}

// =============================================================================
// audio_stream_to_ws
// [DEPRECATED] HTTP POST is now used in web_ui instead of WS continuous streaming
// =============================================================================
/*
void audio_stream_to_ws(void *client_handle) {
    // ...
}
*/
