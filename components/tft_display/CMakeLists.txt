idf_component_register(
    SRCS "tft_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd lvgl freertos peripheral_config
)
