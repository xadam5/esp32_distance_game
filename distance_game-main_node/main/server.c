#include "server.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "game_history_nvs.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
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

    for (size_t round_index = 0; round_index < GAME_HISTORY_ROUNDS_PER_MATCH; ++round_index) {
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
    const char *marker = find_marker(html_start, html_len, HISTORY_ROWS_MARKER);
    const char *prefix_end = marker ? marker : html_end;
    const char *suffix_start = marker ? marker + strlen(HISTORY_ROWS_MARKER) : html_end;

    httpd_resp_set_type(req, "text/html");

    esp_err_t err = send_bytes(req, html_start, (size_t)(prefix_end - html_start));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t last_index = GAME_HISTORY_INVALID_INDEX;
    err = game_history_nvs_get_last_index(&last_index);
    if (err != ESP_OK) {
        return err;
    }

    for (uint32_t order = 0; order < GAME_HISTORY_SLOT_COUNT; ++order) {
        uint32_t slot_index = (last_index == GAME_HISTORY_INVALID_INDEX)
            ? order
            : (last_index + GAME_HISTORY_SLOT_COUNT - order) % GAME_HISTORY_SLOT_COUNT;

        game_session_result_t session;
        bool valid = false;

        err = game_history_nvs_load_slot(slot_index, &session);
        if (err != ESP_OK) {
            return err;
        }

        valid = session.rounds[0].valid || session.rounds[1].valid || session.rounds[2].valid ||
                session.completed || session.aborted ||
                session.total_p1_score != 0 || session.total_p2_score != 0;

        err = render_history_row(req, slot_index, &session, valid);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = send_bytes(req, suffix_start, (size_t)(html_end - suffix_start));
    if (err != ESP_OK) {
        return err;
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t history_erase_handler(httpd_req_t *req)
{
    esp_err_t err = game_history_nvs_erase_all();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

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
    httpd_uri_t history_erase_uri = {
        .uri       = "/api/history/erase",
        .method    = HTTP_POST,
        .handler   = history_erase_handler,
        .user_ctx  = NULL
    };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &history_erase_uri);

    return ESP_OK;
}
