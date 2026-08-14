#ifndef PERIPHERAL_CONFIG_H
#define PERIPHERAL_CONFIG_H

#include "driver/gpio.h"

// =============================================================================
// UART LIÊN MẠCH (Giao tiếp với ESP32-S3 Chính)
// Mạch Chính: GPIO 1 (TX) → Mạch Phụ: GPIO 35 (RX)
// Mạch Chính: GPIO 48 (RX) ← Mạch Phụ: GPIO 36 (TX)
// =============================================================================
#define UART_INTER_NUM       UART_NUM_1
#define UART_INTER_TX_PIN    GPIO_NUM_17   // Phụ TX -> Chính RX (48)
#define UART_INTER_RX_PIN    GPIO_NUM_18   // Phụ RX -> Chính TX (1)
#define UART_INTER_BAUD      115200
#define UART_INTER_BUF_SIZE  1024

// =============================================================================
// AUDIO I2S (Copy y hệt mạch chính)
// INMP441 Mic + MAX98357A Loa — Full-Duplex Shared Bus
// =============================================================================
#define I2S_MIC_SCK_PIN      GPIO_NUM_3
#define I2S_MIC_WS_PIN       GPIO_NUM_14
#define I2S_MIC_SD_PIN       GPIO_NUM_48

#define I2S_SPK_BCLK_PIN     GPIO_NUM_3
#define I2S_SPK_LRC_PIN      GPIO_NUM_14
#define I2S_SPK_DIN_PIN      GPIO_NUM_1

#define AUDIO_I2S_NUM        I2S_NUM_0
#define AUDIO_SAMPLE_RATE    16000

// =============================================================================
// TFT ST7789 240x320 (Copy y hệt mạch chính)
// =============================================================================
// TFT ST7789 240x320 (ESP32-S3)
#define TFT_SPI_MOSI         GPIO_NUM_11
#define TFT_SPI_SCLK         GPIO_NUM_12
#define TFT_DC_PIN           GPIO_NUM_13
#define TFT_CS_PIN           -1
#define TFT_RST_PIN          GPIO_NUM_10
#define TFT_BLK_PIN          GPIO_NUM_4

// =============================================================================
// NÚT NHẤN BOOT (GPIO 0 — nút vật lý trên board)
// =============================================================================
#define BUTTON_PIN           GPIO_NUM_0

// =============================================================================
// AUDIO RECORDING
// =============================================================================
#define MAX_RECORD_SECONDS   25
#define AUDIO_MAX_BUF_SIZE   (AUDIO_SAMPLE_RATE * MAX_RECORD_SECONDS * sizeof(int16_t))  // 320KB

#endif // PERIPHERAL_CONFIG_H
