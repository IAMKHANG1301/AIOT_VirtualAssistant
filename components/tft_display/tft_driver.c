#include "tft_driver.h"
#include "peripheral_config.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

static const char *TAG = "TFT_DRIVER";

#define LCD_HOST       SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_H_RES      240
#define LCD_V_RES      320

// LVGL elements
static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      // contains callback functions
static lv_obj_t * label_line1;
static lv_obj_t * label_line2;
static lv_obj_t * label_line3;
static SemaphoreHandle_t lvgl_mux = NULL;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_tick_task(void *arg)
{
    uint32_t tick_period_ms = 10;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(tick_period_ms));
        lv_tick_inc(tick_period_ms);
    }
}

static void lvgl_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (lvgl_mux) xSemaphoreTake(lvgl_mux, portMAX_DELAY);
        lv_task_handler();
        if (lvgl_mux) xSemaphoreGive(lvgl_mux);
    }
}

esp_err_t tft_lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus for ST7789");
#if defined(TFT_SPI_MOSI) && TFT_SPI_MOSI >= 0
    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_SPI_SCLK,
        .mosi_io_num = TFT_SPI_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 2 * sizeof(uint16_t),
    };
    // Initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_DC_PIN,
        .cs_gpio_num = TFT_CS_PIN,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST_PIN,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    
    // Reset the display
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // Set inversion, swap xy, mirror depending on screen rotation if needed
    esp_lcd_panel_invert_color(panel_handle, true);
    // Display on
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Turn on backlight
    ESP_LOGI(TAG, "Turn on backlight");
    gpio_set_direction((gpio_num_t)TFT_BLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TFT_BLK_PIN, 1);

    // --- LVGL Setup ---
    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();
    
    // Create drawing buffer (Giảm xuống cực tiểu 2 dòng để trả lại toàn bộ RAM nội cho Wi-Fi/Audio)
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LCD_H_RES * 2 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(LCD_H_RES * 2 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_H_RES * 2);

    // Register display driver to LVGL
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

    lvgl_mux = xSemaphoreCreateMutex();

    // Create tick and handler tasks
    xTaskCreate(lvgl_tick_task, "LVGL Tick", 2048, NULL, 5, NULL);
    xTaskCreate(lvgl_task, "LVGL Task", 4096, NULL, 4, NULL);

    // Create UI Elements
    if (lvgl_mux) xSemaphoreTake(lvgl_mux, portMAX_DELAY);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    label_line1 = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label_line1, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_line1, LCD_H_RES - 10);
    lv_obj_align(label_line1, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(label_line1, lv_color_white(), 0);
    lv_label_set_text(label_line1, "");

    label_line2 = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label_line2, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_line2, LCD_H_RES - 10);
    lv_obj_align(label_line2, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(label_line2, lv_color_white(), 0);
    lv_label_set_text(label_line2, "");

    label_line3 = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(label_line3, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_line3, LCD_H_RES - 10);
    lv_obj_align(label_line3, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_color(label_line3, lv_color_white(), 0);
    lv_label_set_text(label_line3, "");
    if (lvgl_mux) xSemaphoreGive(lvgl_mux);

    ESP_LOGI(TAG, "TFT initialization done.");
    return ESP_OK;
#else
    ESP_LOGE(TAG, "TFT SPI MOSI not defined.");
    return ESP_FAIL;
#endif
}

void tft_update_ui(tft_color_t color, const char* line1, const char* line2, const char* line3)
{
    if (!lvgl_mux) return;
    
    xSemaphoreTake(lvgl_mux, portMAX_DELAY);

    lv_color_t lv_c = lv_color_white();
    if (color == TFT_COLOR_RED) lv_c = lv_palette_main(LV_PALETTE_RED);
    else if (color == TFT_COLOR_GREEN) lv_c = lv_palette_main(LV_PALETTE_GREEN);

    if (line1) {
        lv_label_set_text(label_line1, line1);
        lv_obj_set_style_text_color(label_line1, lv_c, 0);
    } else {
        lv_label_set_text(label_line1, "");
    }

    if (line2) {
        lv_label_set_text(label_line2, line2);
        lv_obj_set_style_text_color(label_line2, lv_c, 0);
    } else {
        lv_label_set_text(label_line2, "");
    }

    if (line3) {
        lv_label_set_text(label_line3, line3);
        lv_obj_set_style_text_color(label_line3, lv_c, 0);
    } else {
        lv_label_set_text(label_line3, "");
    }
    
    xSemaphoreGive(lvgl_mux);
}
