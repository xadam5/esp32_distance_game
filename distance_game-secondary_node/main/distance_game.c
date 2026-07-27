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
#include "nvs_flash.h"
#include "gpio_driver.h"
#include "vl53l0x_api.h"
#include "ble_driver.h"

const char* TAG = "distance_game";

#define BUTTON_QUEUE_SIZE 10
#define DEBOUNCE_TIME_MS 50

typedef struct {
    gpio_num_t pin;
    uint32_t timestamp;
} button_event_t;

static QueueHandle_t button_queue = NULL;

typedef enum {
    DISCONNECTED,
    IDLE,
    ENDING,
    BETWEEN_ROUNDS,
    FST_PLAYER,
    SND_PLAYER
} Game_state;

Game_state current_state = DISCONNECTED;
Game_state previous_state = DISCONNECTED;

volatile uint16_t current_distance_mm = 0;
volatile uint16_t fst_player_guess_mm = 0;
volatile uint16_t snd_player_guess_mm = 0;

static void IRAM_ATTR button_isr_handler(void* arg) {
    button_event_t evt;
    evt.pin = (gpio_num_t)arg;
    evt.timestamp = xTaskGetTickCountFromISR();
    xQueueSendFromISR(button_queue, &evt, NULL);
}

static void led_handler_task(void* arg) {
    while (1) {
        switch (current_state) {
            case DISCONNECTED:
                led_set(RED, false);
                break;
            case IDLE:
                led_set(RED, true);
                break;
            case ENDING:
                led_set(RED, true);
                led_toggle(YELLOW);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case BETWEEN_ROUNDS:
                led_set(YELLOW, true);
                break;
            case FST_PLAYER:
                led_set(GREEN, true);
                break;
            case SND_PLAYER:
                led_set(BLUE, true);
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void button_handler_task(void* arg) {
    button_event_t evt;
    static uint32_t last_time[3] = {0, 0, 0};
    int button_idx = 0;
    
    // Map GPIO pins to indices for debounce tracking
    gpio_num_t button_pins[3] = {BUTTON1_GPIO, BUTTON2_GPIO, BUTTON3_GPIO};
    
    while (1) {
        if (xQueueReceive(button_queue, &evt, portMAX_DELAY)) {
            // Find button index
            for (int i = 0; i < 3; i++) {
                if (button_pins[i] == evt.pin) {
                    button_idx = i;
                    break;
                }
            }
            
            uint32_t time_diff = (evt.timestamp - last_time[button_idx]) * portTICK_PERIOD_MS;
            if (time_diff >= DEBOUNCE_TIME_MS) {
                last_time[button_idx] = evt.timestamp;
                
                // Handle button press
                ESP_LOGI(TAG, "Button %d pressed", button_idx + 1);
                
                switch (current_state) {
                    case FST_PLAYER:
                        if (button_idx == 1) {
                            current_state = ENDING;
                            previous_state = FST_PLAYER;
                        }

                        if (button_idx == 0) {
                            fst_player_guess_mm = current_distance_mm;
                            current_state = SND_PLAYER;
                            previous_state = FST_PLAYER;
                        }
                        break;
                    case SND_PLAYER:
                        if (button_idx == 1) {
                            current_state = ENDING;
                            previous_state = SND_PLAYER;
                        }

                        if (button_idx == 2) {
                            snd_player_guess_mm = current_distance_mm;
                            ble_driver_send_measurements(fst_player_guess_mm, snd_player_guess_mm);
                            ESP_LOGI(TAG, "Player 1 guess: %d mm, Player 2 guess: %d mm", fst_player_guess_mm, snd_player_guess_mm);
                            current_state = BETWEEN_ROUNDS;
                            previous_state = SND_PLAYER;
                        }
                        break;
                    case ENDING:
                        if (button_idx == 1) {
                            ble_driver_send_game_ending(true);
                            current_state = IDLE;
                            previous_state = ENDING;
                        } else {
                            // continue game, return to previous state
                            current_state = previous_state;
                        }
                        break;
                    case BETWEEN_ROUNDS:
                        if (button_idx == 1) {
                            current_state = ENDING;
                            previous_state = BETWEEN_ROUNDS;
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void ble_handler_task(void* arg) {
    while (1) {
        if (ble_driver_is_connected()) {
            if (current_state == DISCONNECTED) {
                current_state = IDLE;
                previous_state = IDLE;
            }
            if (ble_driver_is_round_Start_requested()) {
                previous_state = current_state;
                current_state = FST_PLAYER;
                ble_driver_clear_round_Start_requested();
            }
            if (ble_driver_is_game_end_requested()) {
                previous_state = current_state;
                current_state = IDLE;
                ble_driver_clear_game_end_requested();
            }
        } else {
            current_state = DISCONNECTED;
            previous_state = DISCONNECTED;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

VL53L0X_Dev_t VL53L0X_init(void) {
    VL53L0X_comms_initialise(I2C, 400);  // 400 kHz I2C
    
    VL53L0X_Dev_t device;
    device.I2cDevAddr = 0x29;  // Default address
    device.comms_type = I2C;
    device.comms_speed_khz = 400;
    
    VL53L0X_StaticInit(&device);
    VL53L0X_WaitDeviceBooted(&device);
    VL53L0X_DataInit(&device);
    
    VL53L0X_SetDeviceMode(&device, VL53L0X_DEVICEMODE_SINGLE_RANGING);
    VL53L0X_SetMeasurementTimingBudgetMicroSeconds(&device, 33000);  // 33ms per measurement

    return device;
}

static void vl53l0x_task(void* arg) {
    VL53L0X_Dev_t device = VL53L0X_init();

    while (1) {
        VL53L0X_RangingMeasurementData_t measurement;
        VL53L0X_Error status = VL53L0X_PerformSingleRangingMeasurement(&device,
                                &measurement);
        
        if (status == VL53L0X_ERROR_NONE) {
            current_distance_mm = measurement.RangeMilliMeter;
            
            // Clear interrupt for next measurement
            VL53L0X_ClearInterruptMask(&device, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait 100ms between measurements
    }
    
    // Cleanup
    VL53L0X_comms_close();
}

void app_main(void) {
    gpio_setup();

    button_queue = xQueueCreate(BUTTON_QUEUE_SIZE, sizeof(button_event_t));
    if (!button_queue) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return;
    }

    esp_err_t ret;

    /* Initialize NVS. */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    ble_setup();

    // Install GPIO ISR service
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    
    // Attach ISR handler for each button
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON1_GPIO, button_isr_handler, (void*)BUTTON1_GPIO));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON2_GPIO, button_isr_handler, (void*)BUTTON2_GPIO));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON3_GPIO, button_isr_handler, (void*)BUTTON3_GPIO));

    xTaskCreate(ble_handler_task, "ble_handler", 2048, NULL, 10, NULL);
    xTaskCreate(led_handler_task, "led_handler", 2048, NULL, 10, NULL);
    xTaskCreate(button_handler_task, "button_handler", 2048, NULL, 10, NULL);
    xTaskCreate(vl53l0x_task, "vl53l0x_task", 4096, NULL, 10, NULL);
}