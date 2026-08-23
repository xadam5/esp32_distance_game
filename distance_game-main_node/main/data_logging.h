#ifndef DATA_LOGGING_H
#define DATA_LOGGING_H
#include <esp_err.h>
#include <stddef.h>
#include "game_types.h"

extern const int GAME_HISTORY_SLOT_COUNT;

esp_err_t mount_filesystem();
esp_err_t save_game_data(const game_session_result_t *session);
esp_err_t read_all_games_log(game_session_result_t *buffer, size_t buffer_size, size_t *out_num_games);
esp_err_t clear_all_games_log();

#endif // DATA_LOGGING_H