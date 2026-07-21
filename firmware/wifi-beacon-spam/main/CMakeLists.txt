idf_component_register(
  SRCS "wifi_beacon_spam.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi nvs_flash esp_event freertos log esp_hw_support
)
