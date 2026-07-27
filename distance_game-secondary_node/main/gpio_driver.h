#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <stdio.h>
#include "driver/gpio.h"

extern const gpio_num_t BUTTON1_GPIO;
extern const gpio_num_t BUTTON2_GPIO;
extern const gpio_num_t BUTTON3_GPIO;

typedef enum {
    RED,
    YELLOW,
    GREEN,
    BLUE
} LEDS_Color;

void gpio_setup(void);

void led_set(LEDS_Color color, bool on);

void led_toggle(LEDS_Color color);

#endif // GPIO_DRIVER_H