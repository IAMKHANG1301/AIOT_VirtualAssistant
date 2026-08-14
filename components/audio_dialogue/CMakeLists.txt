idf_component_register(SRCS "audio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer fatfs peripheral_config network_cloud)

# Link math library for sin() used in sine wave test
target_link_libraries(${COMPONENT_LIB} INTERFACE m)

