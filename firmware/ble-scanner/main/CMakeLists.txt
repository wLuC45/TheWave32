idf_component_register(
  SRCS "ble_scanner.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common bt nvs_flash freertos log
)
