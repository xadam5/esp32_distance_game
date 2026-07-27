idf_component_register(SRCS "ble_driver.c" "distance_game-main_node.c" "gpio_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES driver esp_event freertos nvs_flash bt esp_system)
