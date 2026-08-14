// =============================================================================
// AIoT Peripheral — Main Entry Point
// ESP32-S3 PHỤ: Audio I2S + TFT ST7789 + WiFi + UART liên mạch
//
// Nhiệm vụ:
//   1. Khởi tạo Audio (INMP441 Mic + MAX98357A Loa)
//   2. Khởi tạo Màn hình TFT ST7789 + LVGL
//   3. Kết nối WiFi (cùng credentials với mạch chính)
//   4. Lắng nghe lệnh UART từ mạch chính (display, play)
//   5. Nút BOOT (GPIO 0): nhấn → ghi âm → gửi Cloud → phát kết quả
// =============================================================================

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#include "peripheral_config.h"
#include "audio.h"
#include "tft_driver.h"
#include "network.h"

static const char *TAG = "PERIPHERAL_MAIN";

// =============================================================================
// UART LIÊN MẠCH — Nhận lệnh từ ESP32-S3 Chính
// Protocol JSON đơn giản:
//   Chính → Phụ: {"cmd":"display","line1":"...","line2":"...","color":"green"}
//   Chính → Phụ: {"cmd":"play","data":"<base64 pcm>"}   (tương lai)
//   Phụ → Chính: {"event":"btn_press"}                   (khi nhấn nút)
//   Phụ → Chính: {"event":"query_done","result":"..."}   (sau khi Cloud trả về)
// =============================================================================

static void uart_init_inter(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = UART_INTER_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_INTER_NUM, UART_INTER_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_INTER_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_INTER_NUM, UART_INTER_TX_PIN, UART_INTER_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART inter-board init: TX=GPIO%d, RX=GPIO%d, baud=%d",
             UART_INTER_TX_PIN, UART_INTER_RX_PIN, UART_INTER_BAUD);
}

static void uart_send_event(const char *json_str)
{
    uart_write_bytes(UART_INTER_NUM, json_str, strlen(json_str));
    uart_write_bytes(UART_INTER_NUM, "\n", 1);  // Dấu phân cách dòng
}

// =============================================================================
// UART RECEIVER TASK — Chạy ngầm, xử lý lệnh từ mạch Chính
// =============================================================================
static void uart_receiver_task(void *arg)
{
    uint8_t *rx_buf = (uint8_t *)malloc(UART_INTER_BUF_SIZE);
    if (!rx_buf) {
        ESP_LOGE(TAG, "uart_receiver_task: malloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int len = uart_read_bytes(UART_INTER_NUM, rx_buf, UART_INTER_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        if (len > 0) {
            rx_buf[len] = '\0';
            ESP_LOGI(TAG, "UART RX from Master: %s", (char *)rx_buf);

            // --- Phân tích lệnh đơn giản (tìm key "cmd") ---
            if (strstr((char *)rx_buf, "\"cmd\":\"display\"")) {
                // Trích xuất line1, line2, color (parse thô)
                // TODO: dùng cJSON để parse chính xác hơn khi cần
                char line1[64] = "AIoT Receptionist";
                char line2[64] = "";
                char *p1 = strstr((char *)rx_buf, "\"line1\":\"");
                char *p2 = strstr((char *)rx_buf, "\"line2\":\"");
                if (p1) { p1 += 9; char *end = strchr(p1, '"'); if (end) { int n = end - p1; if (n > 63) n = 63; strncpy(line1, p1, n); line1[n] = '\0'; } }
                if (p2) { p2 += 9; char *end = strchr(p2, '"'); if (end) { int n = end - p2; if (n > 63) n = 63; strncpy(line2, p2, n); line2[n] = '\0'; } }

                tft_color_t color = TFT_COLOR_WHITE;
                if (strstr((char *)rx_buf, "\"color\":\"green\""))  color = TFT_COLOR_GREEN;
                if (strstr((char *)rx_buf, "\"color\":\"red\""))    color = TFT_COLOR_RED;

                tft_update_ui(color, line1, line2, NULL);
                ESP_LOGI(TAG, "TFT updated: [%s] [%s]", line1, line2);
            }
            // TODO: Xử lý cmd "play" khi cần phát audio từ mạch chính
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(rx_buf);
    vTaskDelete(NULL);
}

// =============================================================================
// VOICE QUERY TASK - Ghi âm -> Cloud -> Phát loa (chạy khi nhấn nút BOOT)
// =============================================================================
static TaskHandle_t s_voice_task = NULL;

static void voice_query_task(void *arg)
{
    ESP_LOGI(TAG, "=== [BOOT] Voice Query: Bắt đầu ghi âm ===");
    tft_update_ui(TFT_COLOR_WHITE, "Dang ghi am...", "Nhan BOOT de dung", NULL);

    // 1. Cấp phát buffer ghi âm trong PSRAM
    const int buf_size = AUDIO_MAX_BUF_SIZE;
    uint8_t *pcm_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) pcm_buf = (uint8_t *)malloc(buf_size);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "voice_query_task: Cannot allocate record buffer!");
        tft_update_ui(TFT_COLOR_RED, "LOI: Khong du RAM", "", NULL);
        s_voice_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 2. Ghi âm với VAD (tự dừng khi im lặng 1.5s hoặc hết 10s)
    int bytes_recorded = audio_record(pcm_buf, buf_size);

    if (bytes_recorded <= 0) {
        ESP_LOGW(TAG, "Ghi âm thất bại hoặc không có tiếng nói.");
        tft_update_ui(TFT_COLOR_RED, "Khong nghe thay", "Thu lai", NULL);
        free(pcm_buf);
        s_voice_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Ghi âm xong: %d bytes. Đang gửi lên Cloud...", bytes_recorded);
    tft_update_ui(TFT_COLOR_WHITE, "Dang gui Cloud...", "Vui long cho", NULL);

    // 3. Gửi lên Cloud — Cloud trả về audio TTS để phát loa
    //    (Hàm này sẽ tự phát audio trả về qua audio_play bên trong network.c)
    esp_err_t ret = network_upload_audio_to_cloud(pcm_buf, (size_t)bytes_recorded);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Cloud xử lý thành công.");
        tft_update_ui(TFT_COLOR_GREEN, "Hoan tat!", "", NULL);
        // Thông báo về mạch chính (optional)
        uart_send_event("{\"event\":\"query_done\"}");
    } else {
        ESP_LOGE(TAG, "Gửi Cloud thất bại.");
        tft_update_ui(TFT_COLOR_RED, "LOI Cloud", "Kiem tra WiFi", NULL);
    }

    free(pcm_buf);
    s_voice_task = NULL;
    vTaskDelete(NULL);
}

// =============================================================================
// app_main — Entry Point của Mạch Phụ
// =============================================================================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  AIoT Peripheral — Audio + TFT + WiFi + UART");
    ESP_LOGI(TAG, "=================================================");

    // 1. Audio I2S (INMP441 Mic + MAX98357A Loa)
    audio_init();
    // Bỏ tiếng bíp khởi động để tránh sập nguồn do tụt áp
    // extern void audio_test_play_sine(void);
    // audio_test_play_sine();

    // 2. Màn hình TFT ST7789 + LVGL
    tft_lcd_init();
    tft_update_ui(TFT_COLOR_WHITE, "Khoi dong...", "Dang ket noi WiFi", NULL);

    // 3. Kết nối WiFi — dùng cùng credentials với mạch chính (trong secrets.h)
    network_init();
    if (network_is_connected()) {
        ESP_LOGI(TAG, "WiFi Connected!");
        tft_update_ui(TFT_COLOR_GREEN, "WiFi OK", "San sang", NULL);
    } else {
        ESP_LOGW(TAG, "WiFi connection failed!");
        tft_update_ui(TFT_COLOR_RED, "LOI WiFi", "Kiem tra mat khau", NULL);
    }

    // 4. UART liên mạch — lắng nghe lệnh từ ESP32-S3 Chính
    uart_init_inter();
    xTaskCreate(uart_receiver_task, "uart_rx", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "UART inter-board listener started.");

    // 5. Cấu hình nút BOOT (GPIO 0)
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    int last_btn_state = 1;

    ESP_LOGI(TAG, "Ready! Nhấn nút BOOT để ghi âm và hỏi Cloud.");

    // 6. Vòng lặp chính — quét nút BOOT
    while (1) {
        int btn = gpio_get_level(BUTTON_PIN);

        if (btn == 0 && last_btn_state == 1) {
            // Debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_PIN) == 0) {
                ESP_LOGI(TAG, "BOOT button pressed!");

                if (s_voice_task == NULL) {
                    // Chưa có task ghi âm -> Bắt đầu ghi âm mới
                    xTaskCreate(voice_query_task, "voice_query", 8192, NULL, 5, &s_voice_task);
                } else {
                    ESP_LOGI(TAG, "Đang ghi âm, vui lòng đợi...");
                }

                // Đợi nhả nút
                while (gpio_get_level(BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        last_btn_state = btn;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
