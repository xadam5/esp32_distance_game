#include "data_logging.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TAG "DATA_LOGGING"
const char *BASE_PATH = "/storage";
const char *LOG_FILE_NAME = "/storage/log.bin";

const int GAME_HISTORY_SLOT_COUNT = 20;

typedef struct {
    uint8_t capacity;
    uint8_t num_games;
} log_header_t;

esp_err_t mount_filesystem() {
    static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

    const esp_vfs_fat_mount_config_t mount_config = {
            .max_files = 2,
            .format_if_mount_failed = true,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE // Set to cluster size.
    };

    // Mount FATFS filesystem located on "storage" partition in read-write mode
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(BASE_PATH, "storage",
                                                    &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t save_game_data(const game_session_result_t *session) {
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    log_header_t header = {0};
    FILE *f = fopen(LOG_FILE_NAME, "r+b");

    if (f == NULL) {
        f = fopen(LOG_FILE_NAME, "w+b");
        if (f == NULL) {
            perror("fopen");
            ESP_LOGE(TAG, "Failed to open file for writing");
            return ESP_FAIL;
        }
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size == 0) {
        header.capacity = GAME_HISTORY_SLOT_COUNT;
        header.num_games = 1;
        if (fwrite(&header, sizeof(log_header_t), 1, f) != 1) {
            fclose(f);
            return ESP_FAIL;
        }
    } else {
        if (fread(&header, sizeof(log_header_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to read log header");
            fclose(f);
            return ESP_FAIL;
        }

        if (header.capacity == 0) {
            header.capacity = GAME_HISTORY_SLOT_COUNT;
        }

        if (header.num_games >= header.capacity) {
            ESP_LOGW(TAG, "Log capacity full");
            fclose(f);
            return ESP_ERR_NO_MEM;
        }

        header.num_games++;
        rewind(f);
        if (fwrite(&header, sizeof(log_header_t), 1, f) != 1) {
            fclose(f);
            return ESP_FAIL;
        }
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    if (fwrite(session, sizeof(game_session_result_t), 1, f) != 1) {
        fclose(f);
        return ESP_FAIL;
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t read_all_games_log(game_session_result_t *buffer, size_t buffer_size, size_t *out_num_games) {
    if (buffer == NULL || buffer_size == 0) {
        if (out_num_games != NULL) {
            *out_num_games = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }

    memset(buffer, 0, buffer_size * sizeof(game_session_result_t));

    FILE *f = fopen(LOG_FILE_NAME, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open log file for reading");
        return ESP_FAIL;
    }

    log_header_t header = {0};
    if (fread(&header, sizeof(log_header_t), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read log header");
        fclose(f);
        return ESP_FAIL;
    }

    if (header.capacity == 0) {
        header.capacity = GAME_HISTORY_SLOT_COUNT;
    }

    long file_size = 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    file_size = ftell(f);
    rewind(f);

    if (file_size < 0) {
        fclose(f);
        return ESP_FAIL;
    }

    if (fseek(f, (long)sizeof(log_header_t), SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t expected_records = 0;
    if (file_size >= (long)sizeof(log_header_t)) {
        expected_records = (size_t)((file_size - sizeof(log_header_t)) / sizeof(game_session_result_t));
    }

    if (header.num_games > header.capacity || header.num_games > expected_records) {
        ESP_LOGW(TAG, "History log appears corrupted; resetting log file");
        fclose(f);
        clear_all_games_log();
        if (out_num_games != NULL) {
            *out_num_games = 0;
        }
        return ESP_OK;
    }

    size_t games_to_read = (header.num_games < buffer_size) ? header.num_games : buffer_size;
    for (size_t i = 0; i < games_to_read; i++) {
        if (fread(&buffer[i], sizeof(game_session_result_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to read game session %zu", i);
            fclose(f);
            return ESP_FAIL;
        }
    }

    if (out_num_games) {
        *out_num_games = games_to_read;
    }

    if (header.num_games > buffer_size) {
        ESP_LOGW(TAG, "Buffer too small to hold all %u games", header.num_games);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t clear_all_games_log() {
    FILE *f = fopen(LOG_FILE_NAME, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open log file for clearing");
        return ESP_FAIL;
    }

    log_header_t header = {0};
    header.capacity = GAME_HISTORY_SLOT_COUNT;
    header.num_games = 0;

    fwrite(&header, sizeof(log_header_t), 1, f);
    fclose(f);
    return ESP_OK;
}