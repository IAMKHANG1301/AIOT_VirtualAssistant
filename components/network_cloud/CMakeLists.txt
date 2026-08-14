idf_component_register(SRCS "network.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp_websocket_client espressif__esp_websocket_client esp_wifi nvs_flash esp-tls mbedtls peripheral_config json esp_event)
