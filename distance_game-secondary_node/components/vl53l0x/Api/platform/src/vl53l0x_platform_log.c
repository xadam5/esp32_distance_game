/*
 * COPYRIGHT (C) STMicroelectronics 2015. All rights reserved.
 *
 * This software is the confidential and proprietary information of
 * STMicroelectronics ("Confidential Information").  You shall not
 * disclose such Confidential Information and shall use it only in
 * accordance with the terms of the license agreement you entered into
 * with STMicroelectronics
 *
 * Programming Golden Rule: Keep it Simple!
 *
 */

/*!
 * \file   VL53L0X_platform_log.c
 * \brief  Code function defintions for Ewok Platform Layer (ESP32 version)
 *
 */

#include <stdio.h>    // sprintf(), vsnprintf(), printf()
#include <string.h>
#include <stdarg.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "vl53l0x_i2c_platform.h"
#include "vl53l0x_def.h"
#include "vl53l0x_platform_log.h"

static const char *TAG = "VL53L0X_LOG";

#define STATUS_OK              0x00

char debug_string[VL53L0X_MAX_STRING_LENGTH_PLT];

char *_trace_filename = NULL;
FILE *_tracefile = NULL;

uint32_t _trace_level = TRACE_LEVEL_WARNING;
uint32_t _trace_modules = TRACE_MODULE_NONE;
uint32_t _trace_functions = TRACE_FUNCTION_NONE;

int32_t VL53L0X_trace_config(char *filename, uint32_t modules, uint32_t level, uint32_t functions)
{
    (void)filename;

    _trace_functions = functions;
    _trace_level = level;
    _trace_modules = modules;

    return STATUS_OK;
}

void trace_print_module_function(uint32_t module, uint32_t level, uint32_t function, const char *format, ...)
{
    if (((level <= _trace_level) && ((module & _trace_modules) > 0)) || ((function & _trace_functions) > 0))
    {
        va_list arg_list;
        char message[VL53L0X_MAX_STRING_LENGTH_PLT];

        va_start(arg_list, format);
        vsnprintf(message, VL53L0X_MAX_STRING_LENGTH_PLT, format, arg_list);
        va_end(arg_list);

        ESP_LOGI(TAG, "%s", message);
    }
}

// Stub functions for GPIO and timer operations
// These are placeholders for ESP32 GPIO control if needed

int32_t VL53L0X_set_gpio(uint8_t level)
{
    int32_t status = STATUS_OK;
    ESP_LOGV(TAG, "Set GPIO = %d", level);
    return status;
}

int32_t VL53L0X_get_gpio(uint8_t *plevel)
{
    int32_t status = STATUS_OK;
    if (plevel != NULL)
    {
        ESP_LOGV(TAG, "Get GPIO = %d", *plevel);
    }
    return status;
}

int32_t VL53L0X_release_gpio(void)
{
    int32_t status = STATUS_OK;
    ESP_LOGV(TAG, "Release GPIO");
    return status;
}

int32_t VL53L0X_get_timer_frequency(int32_t *ptimer_freq_hz)
{
    if (ptimer_freq_hz != NULL)
    {
        *ptimer_freq_hz = 1000;  // 1 kHz timer
    }
    return STATUS_OK;
}

int32_t VL53L0X_get_timer_value(int32_t *ptimer_count)
{
    if (ptimer_count != NULL)
    {
        *ptimer_count = (int32_t)(xTaskGetTickCount() % 0x7FFFFFFF);
    }
    return STATUS_OK;
}

