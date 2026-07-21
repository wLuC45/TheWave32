idf_component_register(
  SRCS "ble_hid_keyboard.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common bt nvs_flash freertos log
)
