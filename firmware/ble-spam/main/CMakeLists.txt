idf_component_register(
  SRCS "ble_spam.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common bt nvs_flash freertos log esp_hw_support
)
