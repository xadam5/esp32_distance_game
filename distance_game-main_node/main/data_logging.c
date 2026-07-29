#include <esp_log.h>
#include <esp_vfs_fat.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

const char *TAG = "data_logging";
const char *BASE_PATH = "/storage";
const char *LOG_FILE_NAME = "/storage/log.bin";

extern const int GAME_ROUNDS_PER_MATCH;
#define GAME_HISTORY_SLOT_COUNT 20

typedef struct {
    uint8_t capacity;
    uint8_t num_games;
} log_header_t;

typedef struct {
    int target_number;
    int p1_measurement;
    int p2_measurement;
    int p1_distance;
    int p2_distance;
    int p1_score;
    int p2_score;
    bool valid;
} game_round_result_t;

typedef struct {
    game_round_result_t rounds[GAME_ROUNDS_PER_MATCH];
    int total_p1_score;
    int total_p2_score;
    bool aborted;
    bool completed;
} game_session_result_t;

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

esp_err_t log_data(game_session_result_t *session) {
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
        fwrite(&header, sizeof(log_header_t), 1, f);
    } else {
        if (fread(&header, sizeof(log_header_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to read log header");
            fclose(f);
            return ESP_FAIL;
        }

        if (header.capacity == 0) {
            header.capacity = GAME_HISTORY_SLOT_COUNT;
        }

        if (header.num_games < header.capacity) {
            header.num_games++;
        } else {
            ESP_LOGW(TAG, "Log capacity full");
            return ESP_ERR_NO_MEM;
        }

        rewind(f);
        fwrite(&header, sizeof(log_header_t), 1, f);
    }

    fseek(f, 0, SEEK_END);
    fwrite(session, sizeof(game_session_result_t), 1, f);

    fclose(f);
    return ESP_OK;
}

esp_err_t read_log(game_session_result_t *buffer, size_t buffer_size) {
    FILE *f = fopen(LOG_FILE_NAME, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open log file for reading");
        return ESP_FAIL;
    }

    log_header_t header;
    if (fread(&header, sizeof(log_header_t), 1, f) != 1) {
        ESP_LOGE(TAG, "Failed to read log header");
        fclose(f);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Log capacity: %d, Number of games: %d", header.capacity, header.num_games);

    size_t games_to_read = (header.num_games < buffer_size) ? header.num_games : buffer_size;
    for (size_t i = 0; i < games_to_read; i++) {
        if (fread(&buffer[i], sizeof(game_session_result_t), 1, f) != 1) {
            ESP_LOGE(TAG, "Failed to read game session %zu", i);
            fclose(f);
            return ESP_FAIL;
        }
    }

    if (header.num_games > buffer_size) {
        ESP_LOGW(TAG, "Buffer too small to hold all %u games", header.num_games);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t clear_log() {
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