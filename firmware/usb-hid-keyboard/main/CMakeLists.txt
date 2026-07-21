idf_component_register(
  SRCS "usb_hid_keyboard.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common nvs_flash freertos log esp_tinyusb
)
