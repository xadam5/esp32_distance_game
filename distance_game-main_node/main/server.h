#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include <esp_log.h>

extern const char *TAG;


esp_err_t start_server();