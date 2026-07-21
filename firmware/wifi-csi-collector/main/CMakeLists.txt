idf_component_register(
  SRCS "wifi_csi_collector.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi nvs_flash esp_event freertos log esp_timer
)
