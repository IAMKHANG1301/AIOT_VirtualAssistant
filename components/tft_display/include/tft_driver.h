#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Font colors
typedef enum {
    TFT_COLOR_WHITE,
    TFT_COLOR_RED,
    TFT_COLOR_GREEN
} tft_color_t;

#ifdef __cplusplus
extern "C" {
#endif

// Initialize SPI, esp_lcd, and LVGL
esp_err_t tft_lcd_init(void);

// High level UI update function
// Pass NULL to line2/line3 if not needed
void tft_update_ui(tft_color_t color, const char* line1, const char* line2, const char* line3);

#ifdef __cplusplus
}
#endif

#endif // TFT_DRIVER_H
