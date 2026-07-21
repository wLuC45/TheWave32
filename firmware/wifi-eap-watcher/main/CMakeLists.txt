idf_component_register(
  SRCS "wifi_eap_watcher.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi esp_timer nvs_flash esp_event freertos log
)
