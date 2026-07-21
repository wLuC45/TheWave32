idf_component_register(
  SRCS "wifi_sniffer.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi nvs_flash esp_event esp_timer freertos log
)
