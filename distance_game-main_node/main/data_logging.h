#ifndef DATA_LOGGING_H
#define DATA_LOGGING_H
#include <esp_err.h>

esp_err_t mount_filesystem();
esp_err_t write_log(const game_session_result_t *session);
esp_err_t read_log(game_session_result_t *buffer, size_t buffer_size);
esp_err_t clear_log();

#endif // DATA_LOGGING_H