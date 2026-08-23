#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "nvs.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <esp_event.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_random.h"

#include "ble_driver.h"
#include "gpio_driver.h"
#include "server.h"
#include "wifi_connection.h"
#include "game_types.h"
#include "data_logging.h"

const char* TAG = "DISTANCE_GAME";

#define MEASUREMENT_WAIT_SLICE_MS 200

// Change these to your own SSID and password
const char* MY_SSID = "SSID";
const char* MY_PASS = "PASSWORD";

typedef enum {
    DISCONNECTED,
    IDLE,
    BETWEEN_ROUNDS,
    ACTIVE_ROUND,
    ENDING,
} Controller_state;

Controller_state current_state = DISCONNECTED;
Controller_state previous_state = DISCONNECTED;
static TaskHandle_t game_session_task_handle = NULL;
static volatile bool game_session_abort_requested = false;
static volatile bool game_session_end_requested = false;
static volatile bool game_session_resume_requested = false;
static http_game_status_t http_game_status = {0};

static void handle_remote_game_end_signal(void)
{
    game_session_end_requested = true;
    game_session_abort_requested = true;
    previous_state = IDLE;
    current_state = IDLE;
}

static int round_abs_diff(int value, int target)
{
    return (value >= target) ? (value - target) : (target - value);
}

static bool decode_measurement_value(const uint8_t *data, uint16_t len, int *out_value)
{
    if (!data || !out_value || len != sizeof(uint16_t)) {
        return false;
    }

    uint16_t value = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    *out_value = (int)value;
    return true;
}

static bool wait_for_both_measurements(int32_t timeout_ms, int *out_p1, int *out_p2)
{
    int p1 = -1;
    int p2 = -1;
    int32_t elapsed_ms = 0;

    while (!game_session_abort_requested && (timeout_ms < 0 || elapsed_ms < timeout_ms)) {
        int32_t wait_ms = MEASUREMENT_WAIT_SLICE_MS;
        if (timeout_ms >= 0) {
            int32_t remaining_ms = timeout_ms - elapsed_ms;
            if (remaining_ms <= 0) {
                break;
            }
            if (remaining_ms < wait_ms) {
                wait_ms = remaining_ms;
            }
        }

        uint16_t uuid = 0;
        uint16_t len = 0;
        uint8_t value[512];
        if (!ble_wait_for_notification(wait_ms, &uuid, value, &len)) {
            elapsed_ms += wait_ms;
            continue;
        }

        if (uuid == 0xFF03) {
            ESP_LOGI(TAG, "Remote game-end signal received");
            handle_remote_game_end_signal();
            return false;
        }

        int measurement = -1;
        if (!decode_measurement_value(value, len, &measurement)) {
            ESP_LOGW(TAG, "Unable to decode measurement payload for uuid=0x%04x", uuid);
            continue;
        }

        if (uuid == 0xFF01) {
            p1 = measurement;
            ESP_LOGI(TAG, "Received measurement for Player 1: %d", p1);
        } else if (uuid == 0xFF02) {
            p2 = measurement;
            ESP_LOGI(TAG, "Received measurement for Player 2: %d", p2);
        } else {
            ESP_LOGW(TAG, "Ignoring unexpected measurement uuid=0x%04x", uuid);
        }

        if (p1 >= 0 && p2 >= 0) {
            break;
        }
    }

    if (out_p1) {
        *out_p1 = p1;
    }
    if (out_p2) {
        *out_p2 = p2;
    }

    return (p1 >= 0 && p2 >= 0 && !game_session_abort_requested);
}

static bool wait_for_round_resume(void)
{
    while (!game_session_abort_requested && !game_session_end_requested && !game_session_resume_requested) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool can_continue = !game_session_abort_requested && !game_session_end_requested;
    game_session_resume_requested = false;
    return can_continue;
}

static void reset_http_game_status(void)
{
    memset(&http_game_status, 0, sizeof(http_game_status));
    http_game_status.active = true;
    http_game_status.waiting_for_round = true;
    http_game_status.total_rounds = GAME_ROUNDS_PER_MATCH;
    http_game_status.target_number = -1;
}

static void update_http_game_status_for_new_round(size_t round_index, int target_number)
{
    http_game_status.current_round = (uint32_t)(round_index + 1U);
    http_game_status.waiting_for_round = true;
    http_game_status.round_result_ready = false;
    http_game_status.final_result_ready = false;
    http_game_status.target_number = target_number;
    http_game_status.p1_measurement = 0;
    http_game_status.p2_measurement = 0;
    http_game_status.p1_distance = 0;
    http_game_status.p2_distance = 0;
    http_game_status.p1_score = 0;
    http_game_status.p2_score = 0;
}

static void update_http_game_status_for_round(size_t round_index, const game_round_result_t *round, int total_p1_score, int total_p2_score)
{
    if (!round) {
        return;
    }

    http_game_status.current_round = (uint32_t)(round_index + 1U);
    http_game_status.waiting_for_round = false;
    http_game_status.round_result_ready = true;
    http_game_status.final_result_ready = false;
    http_game_status.target_number = round->target_number;
    http_game_status.p1_measurement = round->p1_measurement;
    http_game_status.p2_measurement = round->p2_measurement;
    http_game_status.p1_distance = round->p1_distance;
    http_game_status.p2_distance = round->p2_distance;
    http_game_status.p1_score = round->p1_score;
    http_game_status.p2_score = round->p2_score;
    http_game_status.total_p1_score = total_p1_score;
    http_game_status.total_p2_score = total_p2_score;
}

static void mark_http_game_finished(bool completed)
{
    http_game_status.waiting_for_round = false;
    http_game_status.round_result_ready = false;
    http_game_status.final_result_ready = completed;
    http_game_status.completed = completed;
    http_game_status.active = !completed;
}

static bool run_game_round(game_session_result_t *session, size_t round_index)
{
    if (!session || round_index >= (size_t)GAME_ROUNDS_PER_MATCH) {
        return false;
    }

    if (game_session_abort_requested || current_state == ENDING) {
        return false;
    }

    current_state = ACTIVE_ROUND;
    previous_state = BETWEEN_ROUNDS;

    int target_number = 40 + (int)(esp_random() % 361U);
    ESP_LOGI(TAG, "--------------------------------");
    ESP_LOGI(TAG, "Round %u target distance: %d mm", (unsigned)(round_index + 1U), target_number);
    ESP_LOGI(TAG, "--------------------------------");
    update_http_game_status_for_new_round(round_index, target_number);

    esp_err_t w = ble_write_cmd_start(1);
    if (w != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write start command via BLE: %s", esp_err_to_name(w));
        return false;
    }

    int p1 = -1;
    int p2 = -1;
    bool got_both = wait_for_both_measurements(60000, &p1, &p2);
    if (!got_both) {
        ESP_LOGW(TAG, "Round %u did not receive both measurements", (unsigned)(round_index + 1U));
        return false;
    }

    game_round_result_t *round = &session->rounds[round_index];
    round->target_number = target_number;
    round->p1_measurement = p1;
    round->p2_measurement = p2;
    round->p1_distance = round_abs_diff(p1, target_number);
    round->p2_distance = round_abs_diff(p2, target_number);
    round->p1_score = 0;
    round->p2_score = 0;
    round->valid = true;

    if (round->p1_distance < round->p2_distance) {
        round->p1_score = 1;
        session->total_p1_score += 1;
    } else if (round->p2_distance < round->p1_distance) {
        round->p2_score = 1;
        session->total_p2_score += 1;
    } else {
        ESP_LOGI(TAG, "Round %u ended in a tie", (unsigned)(round_index + 1U));
    }

    ESP_LOGI(TAG,
             "Round %u results: target=%d p1=%d (diff=%d) p2=%d (diff=%d) score=%d:%d",
             (unsigned)(round_index + 1U),
             round->target_number,
             round->p1_measurement,
             round->p1_distance,
             round->p2_measurement,
             round->p2_distance,
             round->p1_score,
             round->p2_score);

    update_http_game_status_for_round(round_index, round, session->total_p1_score, session->total_p2_score);
    previous_state = ACTIVE_ROUND;
    current_state = (round_index + 1U < GAME_ROUNDS_PER_MATCH) ? BETWEEN_ROUNDS : IDLE;
    return true;
}

static void game_session_task(void *arg)
{
    game_session_result_t session = {0};
    reset_http_game_status();

    for (size_t round_index = 0; round_index < GAME_ROUNDS_PER_MATCH; ++round_index) {
        if (!run_game_round(&session, round_index)) {
            session.aborted = true;
            break;
        }

        if (round_index + 1U < GAME_ROUNDS_PER_MATCH) {
            ESP_LOGI(TAG, "Waiting for clicking on button on website to continue to the next round");
            game_session_resume_requested = false;
            current_state = BETWEEN_ROUNDS;
            previous_state = ACTIVE_ROUND;
            if (!wait_for_round_resume()) {
                session.aborted = true;
                break;
            }
            http_game_status.waiting_for_round = true;
            http_game_status.round_result_ready = false;
        }
    }

    esp_err_t end_ret = ble_write_cmd_end(1);
    if (end_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send end command via BLE: %s", esp_err_to_name(end_ret));
    }

    if (game_session_end_requested || current_state == ENDING) {
        session.aborted = true;
        ESP_LOGI(TAG, "Ending phase reached; skipping game data save and returning to IDLE");
        current_state = IDLE;
    } else if (!session.aborted && !game_session_abort_requested) {
        session.completed = true;
        (void)save_game_data(&session);
        current_state = IDLE;
    } else {
        ESP_LOGW(TAG, "Game session aborted; skipping game data save");
        current_state = ble_is_connected() ? IDLE : DISCONNECTED;
    }

    if (!session.aborted && !game_session_abort_requested) {
        mark_http_game_finished(true);
    } else {
        http_game_status.aborted = true;
        http_game_status.active = false;
        http_game_status.waiting_for_round = false;
        http_game_status.round_result_ready = false;
        http_game_status.final_result_ready = false;
    }

    previous_state = current_state;
    game_session_abort_requested = false;
    game_session_end_requested = false;
    game_session_resume_requested = false;
    game_session_task_handle = NULL;
    vTaskDelete(NULL);
}

static void ble_notification_handler(uint16_t uuid16, const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;

    if (uuid16 == 0xFF03) {
        ESP_LOGI(TAG, "Game-end notification received via BLE callback");
        handle_remote_game_end_signal();
    }
}

void start_game_session(void)
{
    if (game_session_task_handle != NULL) {
        ESP_LOGW(TAG, "Game session already running");
        return;
    }

    game_session_abort_requested = false;
    game_session_resume_requested = false;
    if (xTaskCreate(game_session_task, "game_session", 4096, NULL, 10, &game_session_task_handle) != pdPASS) {
        game_session_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create game session task");
    }
}

void start_game_session_from_http(void)
{
    start_game_session();
}

void end_game_session_from_http(void)
{
    game_session_end_requested = true;
    game_session_abort_requested = true;
    previous_state = IDLE;
    current_state = IDLE;
}

void continue_game_session_from_http(void)
{
    if (http_game_status.active && http_game_status.round_result_ready && !http_game_status.completed && !http_game_status.aborted) {
        game_session_resume_requested = true;
    }
}

void get_game_status_for_http(http_game_status_t *status)
{
    if (status) {
        *status = http_game_status;
    }
}

static void ble_connection_monitor_task(void *arg)
{
    bool last_connected = false;

    while (1) {
        bool connected = ble_is_connected();
        if (connected != last_connected) {
            ESP_LOGI(TAG, "BLE connection state changed: %s", connected ? "connected" : "disconnected");
            if (!connected) {
                game_session_abort_requested = true;
                if (current_state != ENDING) {
                    current_state = DISCONNECTED;
                }
            } else if (current_state == DISCONNECTED) {
                current_state = IDLE;
            }
            last_connected = connected;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
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
            case ACTIVE_ROUND:
                led_toggle(GREEN);
                led_toggle(BLUE);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    mount_filesystem();

    gpio_setup();

    ble_init();
    ble_register_notify_cb(ble_notification_handler);

    wifi_init_sta(MY_SSID, MY_PASS);
    while (!wifi_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    if (start_server() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server!");
        return;
    }

    xTaskCreate(ble_connection_monitor_task, "connection_monitor", 2048, NULL, 9, NULL);

    xTaskCreate(led_handler_task, "led_handler", 2048, NULL, 10, NULL);
}
