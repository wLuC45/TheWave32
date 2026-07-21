idf_component_register(
  SRCS "net_port_scanner.c"
  INCLUDE_DIRS "."
  REQUIRES tw32_common esp_wifi nvs_flash esp_event esp_netif lwip freertos log esp_timer
)
