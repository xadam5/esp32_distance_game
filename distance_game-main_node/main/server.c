#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_http_server.h"
#include "data_logging.h"

#define TAG "SERVER"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char history_html_start[] asm("_binary_history_html_start");
extern const char history_html_end[] asm("_binary_history_html_end");
extern const char game_html_start[] asm("_binary_game_html_start");
extern const char game_html_end[] asm("_binary_game_html_end");
static const char HISTORY_ROWS_MARKER[] = "<!--HISTORY_ROWS-->";

static esp_err_t send_chunkf(httpd_req_t *req, const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (written < 0 || written >= (int)sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return httpd_resp_sendstr_chunk(req, buffer);
}

static esp_err_t send_bytes(httpd_req_t *req, const char *data, size_t len)
{
    if (len == 0U) {
        return ESP_OK;
    }

    return httpd_resp_send_chunk(req, data, len);
}

static const char *find_marker(const char *start, size_t len, const char *marker)
{
    size_t marker_len = strlen(marker);
    if (marker_len == 0U || len < marker_len) {
        return NULL;
    }

    for (size_t i = 0; i <= len - marker_len; ++i) {
        if (memcmp(start + i, marker, marker_len) == 0) {
            return start + i;
        }
    }

    return NULL;
}

static esp_err_t render_history_row(httpd_req_t *req, uint32_t slot_index, const game_session_result_t *session, bool valid)
{
    const char *status_text = "empty";
    const char *status_class = "status";

    if (valid) {
        if (session->completed) {
            status_text = "completed";
            status_class = "status completed";
        } else if (session->aborted) {
            status_text = "aborted";
            status_class = "status aborted";
        } else {
            status_text = "in progress";
            status_class = "status active";
        }
    }

    esp_err_t err = send_chunkf(req,
                                 "<tr><td>%u</td><td><span class=\"%s\">%s</span></td><td>",
                                 (unsigned)slot_index,
                                 status_class,
                                 status_text);
    if (err != ESP_OK) {
        return err;
    }

    if (!valid) {
        err = httpd_resp_sendstr_chunk(req, "<span class=\"empty\">-</span></td><td class=\"round\"><span class=\"empty\">empty</span></td><td class=\"round\"><span class=\"empty\">empty</span></td><td class=\"round\"><span class=\"empty\">empty</span></td></tr>");
        return err;
    }

    err = send_chunkf(req, "%d:%d</td>", session->total_p1_score, session->total_p2_score);
    if (err != ESP_OK) {
        return err;
    }

    for (size_t round_index = 0; round_index < GAME_ROUNDS_PER_MATCH; ++round_index) {
        const game_round_result_t *round = &session->rounds[round_index];
        if (!round->valid) {
            err = httpd_resp_sendstr_chunk(req, "<td class=\"round\"><span class=\"empty\">empty</span></td>");
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }

        err = send_chunkf(req,
                          "<td class=\"round\">T %d, P1 %d (%d), P2 %d (%d), S %d:%d</td>",
                          round->target_number,
                          round->p1_measurement,
                          round->p1_distance,
                          round->p2_measurement,
                          round->p2_distance,
                          round->p1_score,
                          round->p2_score);
        if (err != ESP_OK) {
            return err;
        }
    }

    return httpd_resp_sendstr_chunk(req, "</tr>");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html_start = index_html_start;
    const char *html_end = index_html_end;
    size_t html_len = (size_t)(html_end - html_start);

    httpd_resp_set_type(req, "text/html");

    esp_err_t err = send_bytes(req, html_start, html_len);
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t game_handler(httpd_req_t *req)
{
    if (httpd_req_get_url_query_len(req) > 0) {
        char query[64] = {0};
        size_t query_len = sizeof(query) - 1;
        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            if (strstr(query, "continue=1") != NULL) {
                continue_game_session_from_http();
            }
        }
    } else {
        start_game_session_from_http();
    }

    const char *html_start = game_html_start;
    const char *html_end = game_html_end;
    size_t html_len = (size_t)(html_end - html_start);

    httpd_resp_set_type(req, "text/html");
    esp_err_t err = send_bytes(req, html_start, html_len);
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t game_status_handler(httpd_req_t *req)
{
    http_game_status_t status = {0};
    get_game_status_for_http(&status);

    char json[512];
    int written = snprintf(json, sizeof(json),
                           "{\"active\":%s,\"waiting_for_round\":%s,\"round_result_ready\":%s,\"final_result_ready\":%s,\"completed\":%s,\"aborted\":%s,\"current_round\":%lu,\"total_rounds\":%lu,\"target_number\":%d,\"p1_measurement\":%d,\"p2_measurement\":%d,\"p1_distance\":%d,\"p2_distance\":%d,\"p1_score\":%d,\"p2_score\":%d,\"total_p1_score\":%d,\"total_p2_score\":%d}",
                           status.active ? "true" : "false",
                           status.waiting_for_round ? "true" : "false",
                           status.round_result_ready ? "true" : "false",
                           status.final_result_ready ? "true" : "false",
                           status.completed ? "true" : "false",
                           status.aborted ? "true" : "false",
                           (unsigned long)status.current_round,
                           (unsigned long)status.total_rounds,
                           status.target_number,
                           status.p1_measurement,
                           status.p2_measurement,
                           status.p1_distance,
                           status.p2_distance,
                           status.p1_score,
                           status.p2_score,
                           status.total_p1_score,
                           status.total_p2_score);
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t history_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "starting history_handler");
    const char *html_start = history_html_start;
    const char *html_end = history_html_end;
    size_t html_len = (size_t)(html_end - html_start);
    const char *marker = find_marker(html_start, html_len, HISTORY_ROWS_MARKER);
    const char *prefix_end = marker ? marker : html_end;
    const char *suffix_start = marker ? marker + strlen(HISTORY_ROWS_MARKER) : html_end;

    httpd_resp_set_type(req, "text/html");

    esp_err_t err = send_bytes(req, html_start, (size_t)(prefix_end - html_start));
    if (err != ESP_OK) {
        return err;
    }

    size_t history_slots = (size_t)GAME_HISTORY_SLOT_COUNT;
    game_session_result_t *buffer = calloc(history_slots, sizeof(*buffer));
    if (buffer == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Out of memory");
    }

    size_t num_games = 0;
    err = read_all_games_log(buffer, history_slots, &num_games);
    if (err != ESP_OK) {
        free(buffer);
        return err;
    }
    ESP_LOGI(TAG, "Read %zu games from log", num_games);

    for (uint32_t slot_index = 0; slot_index < GAME_HISTORY_SLOT_COUNT; ++slot_index) {
        bool valid = false;
        if (slot_index < num_games) {
            const game_session_result_t *session = &buffer[slot_index];
            valid = session->rounds[0].valid || session->rounds[1].valid || session->rounds[2].valid ||
                    session->completed || session->aborted ||
                    session->total_p1_score != 0 || session->total_p2_score != 0;
        }

        err = render_history_row(req, slot_index, &buffer[slot_index], valid);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }
    ESP_LOGI(TAG, "Finished rendering history rows");

    err = send_bytes(req, suffix_start, (size_t)(html_end - suffix_start));
    if (err != ESP_OK) {
        goto cleanup;
    }

cleanup:
    free(buffer);
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t history_erase_handler(httpd_req_t *req)
{
    esp_err_t err = clear_all_games_log();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "");
}

static esp_err_t game_end_handler(httpd_req_t *req)
{
    end_game_session_from_http();

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, "");
}

esp_err_t start_server()
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server!");
        return ESP_FAIL;
    }

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t game_uri = {
        .uri       = "/api/game",
        .method    = HTTP_GET,
        .handler   = game_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t history_uri = {
        .uri       = "/api/history",
        .method    = HTTP_GET,
        .handler   = history_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t game_status_uri = {
        .uri       = "/api/game/status",
        .method    = HTTP_GET,
        .handler   = game_status_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t history_erase_uri = {
        .uri       = "/api/history/erase",
        .method    = HTTP_POST,
        .handler   = history_erase_handler,
        .user_ctx  = NULL
    };
    httpd_uri_t game_end_uri = {
        .uri       = "/api/game/end",
        .method    = HTTP_POST,
        .handler   = game_end_handler,
        .user_ctx  = NULL
    };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &game_uri);
    httpd_register_uri_handler(server, &game_status_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &history_erase_uri);
    httpd_register_uri_handler(server, &game_end_uri);

    return ESP_OK;
}
