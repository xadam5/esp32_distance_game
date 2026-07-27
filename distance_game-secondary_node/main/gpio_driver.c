#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <esp_event.h>
#include <esp_log.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include <stdio.h>

static const gpio_num_t LED1_GPIO = 17;
static const gpio_num_t LED2_GPIO = 15;
static const gpio_num_t LED3_GPIO = 45;
static const gpio_num_t LED4_GPIO = 46;
volatile bool led1_state = false;
volatile bool led2_state = false;
volatile bool led3_state = false;
volatile bool led4_state = false;
const gpio_num_t BUTTON1_GPIO = 18;
const gpio_num_t BUTTON2_GPIO = 16;
const gpio_num_t BUTTON3_GPIO = 42;

typedef enum {
    RED,
    YELLOW,
    GREEN,
    BLUE
} LEDS_Color;

static void set_led_level(gpio_num_t gpio, bool on) {
    gpio_set_level(gpio, on ? 1 : 0);
}

void gpio_setup(void) {
    gpio_reset_pin(LED1_GPIO);
    gpio_set_direction(LED1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(LED1_GPIO, GPIO_DRIVE_CAP_3);
    gpio_reset_pin(LED2_GPIO);
    gpio_set_direction(LED2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(LED2_GPIO, GPIO_DRIVE_CAP_3);
    gpio_reset_pin(LED3_GPIO);
    gpio_set_direction(LED3_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(LED3_GPIO, GPIO_DRIVE_CAP_3);
    gpio_reset_pin(LED4_GPIO);
    gpio_set_direction(LED4_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(LED4_GPIO, GPIO_DRIVE_CAP_3);

    gpio_reset_pin(BUTTON1_GPIO);
    gpio_set_direction(BUTTON1_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(BUTTON2_GPIO);
    gpio_set_direction(BUTTON2_GPIO, GPIO_MODE_INPUT);
    gpio_reset_pin(BUTTON3_GPIO);
    gpio_set_direction(BUTTON3_GPIO, GPIO_MODE_INPUT);
    gpio_config_t button_cfg = {
        .pin_bit_mask = (1ULL << BUTTON1_GPIO) | (1ULL << BUTTON2_GPIO) | (1ULL << BUTTON3_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&button_cfg));
}

void led_set(LEDS_Color color, bool on) {
    set_led_level(LED1_GPIO, color == GREEN && on);
    set_led_level(LED2_GPIO, color == YELLOW && on);
    set_led_level(LED3_GPIO, color == RED && on);
    set_led_level(LED4_GPIO, color == BLUE && on);
}

void led_toggle(LEDS_Color color) {
    switch (color) {
        case GREEN:
            led1_state = !led1_state;
            set_led_level(LED1_GPIO, led1_state);
            break;
        case YELLOW:
            led2_state = !led2_state;
            set_led_level(LED2_GPIO, led2_state);
            break;
        case RED:
            led3_state = !led3_state;
            set_led_level(LED3_GPIO, led3_state);
            break;
        case BLUE:
            led4_state = !led4_state;
            set_led_level(LED4_GPIO, led4_state);
            break;
    }
}
