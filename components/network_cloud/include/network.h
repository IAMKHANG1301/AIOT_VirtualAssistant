#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize NVS and connect to Wi-Fi
 */
void network_init(void);

/**
 * @brief Check if Wi-Fi has IP and is connected
 */
bool network_is_connected(void);

/**
 * @brief Send security alert to Discord with an image
 * @param image_buffer Pointer to JPEG image data (NULL if only text)
 * @param image_len Length of the image data
 * @param message The alert message text
 */
void network_send_discord_alert(const uint8_t *image_buffer, size_t image_len, const char *message);

/**
 * @brief Send access log to the Web Dashboard (JSON)
 * @param id Person ID (or "Stranger")
 * @param status "Success" or "Failed"
 */
void network_send_access_log(const char *id, const char *status);

/**
 * @brief Upload an image to Hugging Face
 * @param filepath Path to the image file (e.g. /sdcard/snapshot.jpg)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t upload_image_to_hf(const char* filepath);

/**
 * @brief Upload an audio file to Hugging Face
 * @param filepath Path to the audio file (e.g. /sdcard/test_mic.raw)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t upload_audio_to_hf(const char* filepath);

/**
 * @brief Upload raw PCM audio buffer to Hugging Face WebSocket directly from RAM
 */
esp_err_t upload_audio_to_hf_buffer(const uint8_t *pcm_buf, size_t pcm_size);

void* hf_ws_connect(void);
void hf_ws_disconnect(void *client);
void hf_ws_send_text(void *client_handle, const char *text);

/**
 * @brief Gửi file âm thanh WAV lên Hugging Face Space để xử lý toàn trình (STT -> LLM -> TTS)
 * @param audio_in Con trỏ chứa dữ liệu âm thanh thu từ Micro
 * @param in_len Chiều dài dữ liệu âm thanh thu được
 * @param out_guest_text Buffer để nhận lại nội dung Text mà khách đã nói (để gửi Discord)
 * @param max_text_len Độ dài tối đa của chuỗi text
 * @param audio_out Con trỏ chứa dữ liệu âm thanh phản hồi (TTS) để phát ra loa
 * @param out_len Biến lưu độ dài file âm thanh trả về
 * @param max_out_len Giới hạn bộ đệm loa
 * @return true nếu gọi API thành công
 */
bool network_hf_process_audio(const uint8_t *audio_in, size_t in_len, 
                              char *out_guest_text, size_t max_text_len,
                              uint8_t *audio_out, size_t *out_len, size_t max_out_len);

esp_err_t network_upload_audio_to_cloud(const uint8_t *pcm_buf, size_t pcm_size);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_H
