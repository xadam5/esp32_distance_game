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
 * \file   VL53L0X_platform.c
 * \brief  Code function defintions for Ewok Platform Layer (ESP32 version)
 *
 */

#include <stdio.h>    // sprintf(), vsnprintf(), printf()
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c.h>
#include <esp_log.h>

#include "vl53l0x_i2c_platform.h" 
#include "vl53l0x_def.h"
#include "vl53l0x_platform_log.h"

static const char *TAG = "VL53L0X";

// ESP32 I2C configuration (these should match your project's I2C setup)
// Default: I2C_NUM_0, GPIO22 (SCL), GPIO21 (SDA), 400kHz
#define I2C_MASTER_NUM          I2C_NUM_0
#define I2C_MASTER_FREQ_HZ      400000
#define I2C_MASTER_TX_BUF_DISABLE 0   /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0   /*!< I2C master doesn't need buffer */
#define VL53L0X_MAX_I2C_XFER_SIZE   64

#define STATUS_OK              0x00
#define STATUS_FAIL            0x01

static bool_t i2c_initialized = false;

/**
 * @brief Initialize I2C master for ESP32
 * 
 * @param comms_type  - selects between I2C and SPI (for ESP32, only I2C is supported)
 * @param comms_speed_khz - I2C speed in kHz
 * @return status - 0 = ok, 1 = error
 */
int32_t VL53L0X_comms_initialise(uint8_t comms_type, uint16_t comms_speed_khz)
{
    int32_t status = STATUS_OK;
    uint32_t freq_hz = (uint32_t)comms_speed_khz * 1000;

    if (i2c_initialized)
    {
        ESP_LOGW(TAG, "I2C already initialized");
        return STATUS_OK;
    }

    // Configure I2C master
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_9,           // SDA on GPIO21
        .scl_io_num = GPIO_NUM_10,           // SCL on GPIO22
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq_hz,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(ret));
        return STATUS_FAIL;
    }

    // Install I2C driver
    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 
                             I2C_MASTER_RX_BUF_DISABLE, 
                             I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return STATUS_FAIL;
    }

    i2c_initialized = true;
    ESP_LOGI(TAG, "I2C initialized at %u kHz", comms_speed_khz);

    return status;
}

/**
 * @brief Close I2C communications
 * @return status - 0 = ok, 1 = error
 */
int32_t VL53L0X_comms_close(void)
{
    int32_t status = STATUS_OK;

    if (!i2c_initialized)
    {
        return STATUS_OK;
    }

    esp_err_t ret = i2c_driver_delete(I2C_MASTER_NUM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C driver delete failed: %s", esp_err_to_name(ret));
        status = STATUS_FAIL;
    }
    else
    {
        i2c_initialized = false;
        ESP_LOGI(TAG, "I2C closed");
    }

    return status;
}

/**
 * @brief Write multiple bytes via I2C
 * 
 * @param address - I2C device address (7-bit)
 * @param index - Register address to write to
 * @param pdata - Pointer to data buffer
 * @param count - Number of bytes to write
 * @return status - 0 = ok, 1 = error
 */
int32_t VL53L0X_write_multi(uint8_t address, uint8_t index, uint8_t *pdata, int32_t count)
{
    int32_t status = STATUS_OK;

    // Prepare write buffer: first byte is register address, followed by data
    uint8_t write_buffer[VL53L0X_MAX_I2C_XFER_SIZE + 1];
    
    if ((count + 1) > VL53L0X_MAX_I2C_XFER_SIZE)
    {
        ESP_LOGE(TAG, "Write count exceeds buffer size");
        return STATUS_FAIL;
    }

    write_buffer[0] = index;
    memcpy(&write_buffer[1], pdata, count);

    // Perform I2C write
    esp_err_t ret = i2c_master_write_to_device(I2C_MASTER_NUM, address, 
                                               write_buffer, count + 1, 
                                               pdMS_TO_TICKS(100));
    
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C write failed: %s (addr=0x%02x, reg=0x%02x, count=%d)", 
                 esp_err_to_name(ret), address, index, count);
        status = STATUS_FAIL;
    }
    else
    {
        ESP_LOGV(TAG, "I2C write: addr=0x%02x, reg=0x%02x, bytes=%d", address, index, count);
    }

    return status;
}

/**
 * @brief Read multiple bytes via I2C
 * 
 * @param address - I2C device address (7-bit)
 * @param index - Register address to read from
 * @param pdata - Pointer to data buffer for storing read data
 * @param count - Number of bytes to read
 * @return status - 0 = ok, 1 = error
 */
int32_t VL53L0X_read_multi(uint8_t address, uint8_t index, uint8_t *pdata, int32_t count)
{
    int32_t status = STATUS_OK;

    if (count > VL53L0X_MAX_I2C_XFER_SIZE)
    {
        ESP_LOGE(TAG, "Read count exceeds buffer size");
        return STATUS_FAIL;
    }

    // Perform I2C write-read: write register address, then read data
    esp_err_t ret = i2c_master_write_read_device(I2C_MASTER_NUM, address,
                                                 &index, 1,
                                                 pdata, count,
                                                 pdMS_TO_TICKS(100));
    
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C read failed: %s (addr=0x%02x, reg=0x%02x, count=%d)", 
                 esp_err_to_name(ret), address, index, count);
        status = STATUS_FAIL;
    }
    else
    {
        ESP_LOGV(TAG, "I2C read: addr=0x%02x, reg=0x%02x, bytes=%d", address, index, count);
    }

    return status;
}


int32_t VL53L0X_write_byte(uint8_t address, uint8_t index, uint8_t data)
{
    int32_t status = STATUS_OK;
    const int32_t cbyte_count = 1;

    status = VL53L0X_write_multi(address, index, &data, cbyte_count);

    return status;

}


int32_t VL53L0X_write_word(uint8_t address, uint8_t index, uint16_t data)
{
    int32_t status = STATUS_OK;

    uint8_t  buffer[BYTES_PER_WORD];

    // Split 16-bit word into MS and LS uint8_t
    buffer[0] = (uint8_t)(data >> 8);
    buffer[1] = (uint8_t)(data &  0x00FF);

    status = VL53L0X_write_multi(address, index, buffer, BYTES_PER_WORD);

    return status;

}


int32_t VL53L0X_write_dword(uint8_t address, uint8_t index, uint32_t data)
{
    int32_t status = STATUS_OK;
    uint8_t  buffer[BYTES_PER_DWORD];

    // Split 32-bit word into MS ... LS bytes
    buffer[0] = (uint8_t) (data >> 24);
    buffer[1] = (uint8_t)((data &  0x00FF0000) >> 16);
    buffer[2] = (uint8_t)((data &  0x0000FF00) >> 8);
    buffer[3] = (uint8_t) (data &  0x000000FF);

    status = VL53L0X_write_multi(address, index, buffer, BYTES_PER_DWORD);

    return status;

}


int32_t VL53L0X_read_byte(uint8_t address, uint8_t index, uint8_t *pdata)
{
    int32_t status = STATUS_OK;
    int32_t cbyte_count = 1;

    status = VL53L0X_read_multi(address, index, pdata, cbyte_count);

    return status;

}


int32_t VL53L0X_read_word(uint8_t address, uint8_t index, uint16_t *pdata)
{
    int32_t  status = STATUS_OK;
	uint8_t  buffer[BYTES_PER_WORD];

    status = VL53L0X_read_multi(address, index, buffer, BYTES_PER_WORD);
	*pdata = ((uint16_t)buffer[0]<<8) + (uint16_t)buffer[1];

    return status;

}

int32_t VL53L0X_read_dword(uint8_t address, uint8_t index, uint32_t *pdata)
{
    int32_t status = STATUS_OK;
	uint8_t  buffer[BYTES_PER_DWORD];

    status = VL53L0X_read_multi(address, index, buffer, BYTES_PER_DWORD);
    *pdata = ((uint32_t)buffer[0]<<24) + ((uint32_t)buffer[1]<<16) + ((uint32_t)buffer[2]<<8) + (uint32_t)buffer[3];

    return status;

}

/**
 * @brief Platform wait function (microseconds)
 * 
 * @param wait_us - Wait time in microseconds
 * @return status - 0 = ok
 */
int32_t VL53L0X_platform_wait_us(int32_t wait_us)
{
    int32_t status = STATUS_OK;
    uint32_t wait_ms = (wait_us + 999) / 1000;  // Round up to next millisecond

    // Use FreeRTOS task delay instead of Windows events
    if (wait_ms > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    ESP_LOGV(TAG, "Wait us: %d", wait_us);

    return status;
}

/**
 * @brief Platform wait function (milliseconds)
 * 
 * @param wait_ms - Wait time in milliseconds
 * @return status - 0 = ok
 */
int32_t VL53L0X_wait_ms(int32_t wait_ms)
{
    int32_t status = STATUS_OK;

    // Use FreeRTOS task delay instead of Windows events
    if (wait_ms > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    ESP_LOGV(TAG, "Wait ms: %d", wait_ms);

    return status;
}


int32_t VL53L0X_set_gpio(uint8_t level)
{
    int32_t status = STATUS_OK;
    //status = VL53L0X_set_gpio_sv(level);
#ifdef VL53L0X_LOG_ENABLE
    trace_i2c("// Set GPIO = %d;\n", level);
#endif

    return status;

}


int32_t VL53L0X_get_gpio(uint8_t *plevel)
{
    int32_t status = STATUS_OK;
#ifdef VL53L0X_LOG_ENABLE
    trace_i2c("// Get GPIO = %d;\n", *plevel);
#endif
    return status;
}


int32_t VL53L0X_release_gpio(void)
{
    int32_t status = STATUS_OK;
#ifdef VL53L0X_LOG_ENABLE
    trace_i2c("// Releasing force on GPIO\n");
#endif
    return status;

}

/**
 * @brief Cycle sensor power (perform a power cycle)
 * @return status - 0 = ok
 */
int32_t VL53L0X_cycle_power(void)
{
    int32_t status = STATUS_OK;
    
    // For ESP32: Implement GPIO-based power cycle if needed
    // This would typically toggle a GPIO pin connected to the sensor's power control
    // For now, we just log and return success
    
    ESP_LOGV(TAG, "Cycle sensor power");
    
    return status;
}


int32_t VL53L0X_get_timer_frequency(int32_t *ptimer_freq_hz)
{
       *ptimer_freq_hz = 0;
       return STATUS_FAIL;
}


int32_t VL53L0X_get_timer_value(int32_t *ptimer_count)
{
       *ptimer_count = 0;
       return STATUS_FAIL;
}
