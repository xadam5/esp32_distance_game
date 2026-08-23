#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include <esp_log.h>

typedef struct {
    bool active;
    bool waiting_for_round;
    bool round_result_ready;
    bool final_result_ready;
    bool completed;
    bool aborted;
    uint32_t current_round;
    uint32_t total_rounds;
    int target_number;
    int p1_measurement;
    int p2_measurement;
    int p1_distance;
    int p2_distance;
    int p1_score;
    int p2_score;
    int total_p1_score;
    int total_p2_score;
} http_game_status_t;

void start_game_session_from_http(void);
void end_game_session_from_http(void);
void continue_game_session_from_http(void);
void get_game_status_for_http(http_game_status_t *status);
esp_err_t start_server();