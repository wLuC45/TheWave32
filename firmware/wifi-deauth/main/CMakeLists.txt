idf_component_register(
    SRCS wifi_deauth.c
    INCLUDE_DIRS "."
    REQUIRES tw32_common esp_wifi nvs_flash esp_event freertos log
)

# --wrap is set globally in ../CMakeLists.txt via idf_build_set_property;
# target_link_options here doesn't propagate to the final link command.
