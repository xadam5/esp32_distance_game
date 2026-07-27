idf_component_register(SRCS "distance_game.c" "gpio_driver.c" "ble_driver.c"
                    INCLUDE_DIRS "."
                    REQUIRES vl53l0x esp_event nvs_flash bt)
