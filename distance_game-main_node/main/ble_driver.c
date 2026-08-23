#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "ble_driver.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define GATTC_TAG "GATTC"
#define REMOTE_SERVICE_UUID        0x00FF
#define REMOTE_NOTIFY_CHAR_UUID    0xFF01
#define PROFILE_NUM      1
#define PROFILE_A_APP_ID 0
#define INVALID_HANDLE   0

static const char remote_device_name[] = "ESP_DISTANCE_GAME";
static bool connect    = false;
static bool get_server = false;
static esp_gattc_char_elem_t *char_elem_result   = NULL;
static esp_gattc_descr_elem_t *descr_elem_result = NULL;

/* User-provided notify callback (set via ble_register_notify_cb) */
static ble_notify_cb_t user_notify_cb = NULL;

/* Notification queue and task */
#define NOTIFY_QUEUE_LEN 8
#define NOTIFY_MAX_LEN 512
typedef struct {
    uint16_t uuid;
    uint16_t len;
    uint8_t data[NOTIFY_MAX_LEN];
} notify_msg_t;
static QueueHandle_t notify_queue = NULL;

bool ble_is_connected(void)
{
    return connect && get_server;
}

bool ble_wait_for_notification(int32_t timeout_ms, uint16_t *out_uuid, uint8_t *out_buf, uint16_t *out_len)
{
    if (!notify_queue) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    notify_msg_t msg;
    if (xQueueReceive(notify_queue, &msg, ticks) != pdTRUE) return false;
    if (out_uuid) *out_uuid = msg.uuid;
    if (out_len) *out_len = msg.len;
    if (out_buf && msg.len > 0) memcpy(out_buf, msg.data, msg.len);
    return true;
}

/* Declare static functions */
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);


static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = REMOTE_SERVICE_UUID,},
};

static esp_bt_uuid_t remote_char_p1_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = REMOTE_NOTIFY_CHAR_UUID,},
};

/* Additional characteristic UUIDs used by the distance game */
static esp_bt_uuid_t remote_char_p2_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = 0xFF02,},
};

static esp_bt_uuid_t remote_char_game_end_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = 0xFF03,},
};

static esp_bt_uuid_t remote_char_cmd_start_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = 0xFF04,},
};

static esp_bt_uuid_t remote_char_cmd_end_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = 0xFF05,},
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,},
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_ENABLE
};

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    /* Handles for specific characteristics */
    uint16_t char_meas_p1_handle;
    uint16_t char_meas_p2_handle;
    uint16_t char_game_end_handle;
    uint16_t char_cmd_start_handle;
    uint16_t char_cmd_end_handle;
    /* Properties cached for each char */
    uint8_t  char_meas_p1_props;
    uint8_t  char_meas_p2_props;
    uint8_t  char_game_end_props;
    uint8_t  char_cmd_start_props;
    uint8_t  char_cmd_end_props;
    esp_bd_addr_t remote_bda;
};

static void restart_scanning(void)
{
    esp_err_t scan_ret = esp_ble_gap_start_scanning(30);
    if (scan_ret != ESP_OK) {
        ESP_LOGE(GATTC_TAG, "failed to restart scanning: %s", esp_err_to_name(scan_ret));
    }
}

/* One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT */
static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

static void reset_connection_cache(void)
{
    connect = false;
    get_server = false;

    struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
    p->conn_id = 0;
    p->service_start_handle = 0;
    p->service_end_handle = 0;
    p->char_meas_p1_handle = 0;
    p->char_meas_p2_handle = 0;
    p->char_game_end_handle = 0;
    p->char_cmd_start_handle = 0;
    p->char_cmd_end_handle = 0;
    p->char_meas_p1_props = 0;
    p->char_meas_p2_props = 0;
    p->char_game_end_props = 0;
    p->char_cmd_start_props = 0;
    p->char_cmd_end_props = 0;
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(GATTC_TAG, "REG_EVT");
        esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
        if (scan_ret){
            ESP_LOGE(GATTC_TAG, "set scan params error, error code = %x", scan_ret);
        }
        break;
    case ESP_GATTC_CONNECT_EVT:{
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_CONNECT_EVT conn_id %d, if %d", p_data->connect.conn_id, gattc_if);
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = p_data->connect.conn_id;
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda, p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(GATTC_TAG, "REMOTE BDA:");
        ESP_LOG_BUFFER_HEX(GATTC_TAG, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, sizeof(esp_bd_addr_t));
        esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req (gattc_if, p_data->connect.conn_id);
        if (mtu_ret){
            ESP_LOGE(GATTC_TAG, "config MTU error, error code = %x", mtu_ret);
        }
        break;
    }
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "open failed, status %d", p_data->open.status);
            reset_connection_cache();
            restart_scanning();
            break;
        }
        ESP_LOGI(GATTC_TAG, "open success");
        break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (param->dis_srvc_cmpl.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "discover service failed, status %d", param->dis_srvc_cmpl.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "discover service complete conn_id %d", param->dis_srvc_cmpl.conn_id);
        esp_ble_gattc_search_service(gattc_if, param->dis_srvc_cmpl.conn_id, &remote_filter_service_uuid);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG,"config mtu failed, error status = %x", param->cfg_mtu.status);
        }
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d", param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        ESP_LOGI(GATTC_TAG, "SEARCH RES: conn_id = %x is primary service %d", p_data->search_res.conn_id, p_data->search_res.is_primary);
        ESP_LOGI(GATTC_TAG, "start handle %d end handle %d current handle value %d", p_data->search_res.start_handle, p_data->search_res.end_handle, p_data->search_res.srvc_id.inst_id);
        if (p_data->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 && p_data->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID) {
            ESP_LOGI(GATTC_TAG, "service found");
            get_server = true;
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle = p_data->search_res.start_handle;
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle = p_data->search_res.end_handle;
            ESP_LOGI(GATTC_TAG, "UUID16: %x", p_data->search_res.srvc_id.uuid.uuid.uuid16);
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (p_data->search_cmpl.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "search service failed, error status = %x", p_data->search_cmpl.status);
            break;
        }
        if(p_data->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_REMOTE_DEVICE) {
            ESP_LOGI(GATTC_TAG, "Get service information from remote device");
        } else if (p_data->search_cmpl.searched_service_source == ESP_GATT_SERVICE_FROM_NVS_FLASH) {
            ESP_LOGI(GATTC_TAG, "Get service information from flash");
        } else {
            ESP_LOGI(GATTC_TAG, "unknown service source");
        }
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_SEARCH_CMPL_EVT");
        if (get_server){
            uint16_t char_capacity = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                     p_data->search_cmpl.conn_id,
                                                                     ESP_GATT_DB_CHARACTERISTIC,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                     INVALID_HANDLE,
                                                                     &char_capacity);
            if (status != ESP_GATT_OK){
                ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_attr_count error");
                break;
            }

            if (char_capacity > 0){
                char_elem_result = (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * char_capacity);
                if (!char_elem_result){
                    ESP_LOGE(GATTC_TAG, "gattc no mem");
                    break;
                }else{
                    uint16_t match_count = char_capacity;
                    /* Try to find each characteristic by UUID and cache handle/props */
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_char_p1_uuid,
                                                             char_elem_result,
                                                             &match_count);
                    if (status == ESP_GATT_OK && match_count > 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].char_meas_p1_handle = char_elem_result[0].char_handle;
                        gl_profile_tab[PROFILE_A_APP_ID].char_meas_p1_props = char_elem_result[0].properties;
                        if (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                            esp_ble_gattc_register_for_notify (gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, char_elem_result[0].char_handle);
                        }
                    }
                    match_count = char_capacity;
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_char_p2_uuid,
                                                             char_elem_result,
                                                             &match_count);
                    if (status == ESP_GATT_OK && match_count > 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].char_meas_p2_handle = char_elem_result[0].char_handle;
                        gl_profile_tab[PROFILE_A_APP_ID].char_meas_p2_props = char_elem_result[0].properties;
                        if (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                            esp_ble_gattc_register_for_notify (gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, char_elem_result[0].char_handle);
                        }
                    }
                    match_count = char_capacity;
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_char_game_end_uuid,
                                                             char_elem_result,
                                                             &match_count);
                    if (status == ESP_GATT_OK && match_count > 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].char_game_end_handle = char_elem_result[0].char_handle;
                        gl_profile_tab[PROFILE_A_APP_ID].char_game_end_props = char_elem_result[0].properties;
                        if (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                            esp_ble_gattc_register_for_notify (gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, char_elem_result[0].char_handle);
                        }
                    }
                    match_count = char_capacity;
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_char_cmd_start_uuid,
                                                             char_elem_result,
                                                             &match_count);
                    if (status == ESP_GATT_OK && match_count > 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].char_cmd_start_handle = char_elem_result[0].char_handle;
                        gl_profile_tab[PROFILE_A_APP_ID].char_cmd_start_props = char_elem_result[0].properties;
                    }
                    match_count = char_capacity;
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_char_cmd_end_uuid,
                                                             char_elem_result,
                                                             &match_count);
                    if (status == ESP_GATT_OK && match_count > 0) {
                        gl_profile_tab[PROFILE_A_APP_ID].char_cmd_end_handle = char_elem_result[0].char_handle;
                        gl_profile_tab[PROFILE_A_APP_ID].char_cmd_end_props = char_elem_result[0].properties;
                    }
                }
                /* free char_elem_result */
                free(char_elem_result);

                /* Verify that all required characteristics were discovered and log errors if any are missing */
                struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
                if (p->char_meas_p1_handle == 0) {
                    ESP_LOGE(GATTC_TAG, "Characteristic 0xFF01 (meas p1) not found");
                }
                if (p->char_meas_p2_handle == 0) {
                    ESP_LOGE(GATTC_TAG, "Characteristic 0xFF02 (meas p2) not found");
                }
                if (p->char_game_end_handle == 0) {
                    ESP_LOGE(GATTC_TAG, "Characteristic 0xFF03 (game end) not found");
                }
                if (p->char_cmd_start_handle == 0) {
                    ESP_LOGE(GATTC_TAG, "Characteristic 0xFF04 (cmd start) not found");
                }
                if (p->char_cmd_end_handle == 0) {
                    ESP_LOGE(GATTC_TAG, "Characteristic 0xFF05 (cmd end) not found");
                }

                /* Check write permissions for command characteristics */
                if (p->char_cmd_start_handle != 0) {
                    if (!(p->char_cmd_start_props & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
                        ESP_LOGE(GATTC_TAG, "Characteristic 0xFF04 (cmd start) found but not writable (props=0x%02x)", p->char_cmd_start_props);
                    }
                }
                if (p->char_cmd_end_handle != 0) {
                    if (!(p->char_cmd_end_props & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
                        ESP_LOGE(GATTC_TAG, "Characteristic 0xFF05 (cmd end) found but not writable (props=0x%02x)", p->char_cmd_end_props);
                    }
                }
            }else{
                ESP_LOGE(GATTC_TAG, "no char found");
            }
        }
         break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_REG_FOR_NOTIFY_EVT");
        if (p_data->reg_for_notify.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "REG FOR NOTIFY failed: error status = %d", p_data->reg_for_notify.status);
        }else{
            uint16_t count = 0;
            uint16_t notify_en = 1;
            esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         ESP_GATT_DB_DESCRIPTOR,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                         p_data->reg_for_notify.handle,
                                                                         &count);
            if (ret_status != ESP_GATT_OK){
                ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_attr_count error");
                break;
            }
            if (count > 0){
                descr_elem_result = malloc(sizeof(esp_gattc_descr_elem_t) * count);
                if (!descr_elem_result){
                    ESP_LOGE(GATTC_TAG, "malloc error, gattc no mem");
                    break;
                }else{
                    ret_status = esp_ble_gattc_get_descr_by_char_handle( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         p_data->reg_for_notify.handle,
                                                                         notify_descr_uuid,
                                                                         descr_elem_result,
                                                                         &count);
                    if (ret_status != ESP_GATT_OK){
                        ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_descr_by_char_handle error");
                        free(descr_elem_result);
                        descr_elem_result = NULL;
                        break;
                    }
                    /* Every char has only one descriptor in our 'ESP_GATTS_DEMO' demo, so we used first 'descr_elem_result' */
                    if (count > 0 && descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG){
                        ret_status = esp_ble_gattc_write_char_descr( gattc_if,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                     descr_elem_result[0].handle,
                                                                     sizeof(notify_en),
                                                                     (uint8_t *)&notify_en,
                                                                     ESP_GATT_WRITE_TYPE_RSP,
                                                                     ESP_GATT_AUTH_REQ_NONE);
                    }

                    if (ret_status != ESP_GATT_OK){
                        ESP_LOGE(GATTC_TAG, "esp_ble_gattc_write_char_descr error");
                    }

                    /* free descr_elem_result */
                    free(descr_elem_result);
                }
            }
            else{
                ESP_LOGE(GATTC_TAG, "decsr not found");
            }

        }
        break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
        uint16_t handle = p_data->notify.handle;
        uint16_t uuid16 = 0;
        struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
        if (handle == p->char_meas_p1_handle) uuid16 = 0xFF01;
        else if (handle == p->char_meas_p2_handle) uuid16 = 0xFF02;
        else if (handle == p->char_game_end_handle) uuid16 = 0xFF03;

        /* Package and enqueue the notification for the dispatch task to handle */
        if (notify_queue && uuid16) {
            notify_msg_t msg;
            msg.uuid = uuid16;
            msg.len = (p_data->notify.value_len > NOTIFY_MAX_LEN) ? NOTIFY_MAX_LEN : p_data->notify.value_len;
            if (msg.len > 0) memcpy(msg.data, p_data->notify.value, msg.len);
            if (xQueueSend(notify_queue, &msg, portMAX_DELAY) != pdTRUE) {
                ESP_LOGW(GATTC_TAG, "notify queue full, dropping notification for uuid=0x%04x", uuid16);
                ESP_LOG_BUFFER_HEX(GATTC_TAG, p_data->notify.value, p_data->notify.value_len);
            }
        } else {
            /* Queue-only: if queue missing or UUID unknown, log and drop the notification */
            if (!uuid16) {
                ESP_LOGW(GATTC_TAG, "Unknown notify handle %d — ignoring", handle);
            } else if (!notify_queue) {
                ESP_LOGE(GATTC_TAG, "notify_queue not initialized — dropping notification for uuid=0x%04x", uuid16);
                ESP_LOG_BUFFER_HEX(GATTC_TAG, p_data->notify.value, p_data->notify.value_len);
            }
        }

        if (user_notify_cb) {
            user_notify_cb(uuid16, p_data->notify.value, p_data->notify.value_len);
        }
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (p_data->write.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "write descr failed, error status = %x", p_data->write.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "write descr success ");
        /* Descriptor written (notifications enabled). No automatic writes performed here. */
        break;
    case ESP_GATTC_SRVC_CHG_EVT: {
        esp_bd_addr_t bda;
        memcpy(bda, p_data->srvc_chg.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_SRVC_CHG_EVT, bd_addr:");
        ESP_LOG_BUFFER_HEX(GATTC_TAG, bda, sizeof(esp_bd_addr_t));
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (p_data->write.status != ESP_GATT_OK){
            ESP_LOGE(GATTC_TAG, "write char failed, error status = %x", p_data->write.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "write char success ");
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        reset_connection_cache();
        ESP_LOGI(GATTC_TAG, "ESP_GATTC_DISCONNECT_EVT, reason = %d", p_data->disconnect.reason);
        restart_scanning();
        break;
    default:
        break;
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        //the unit of the duration is second
        uint32_t duration = 30;
        esp_ble_gap_start_scanning(duration);
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        //scan start complete event to indicate scan start successfully or failed
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(GATTC_TAG, "scan start failed, error status = %x", param->scan_start_cmpl.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "scan start success");

        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            // ESP_LOG_BUFFER_HEX(GATTC_TAG, scan_result->scan_rst.bda, 6);
            // ESP_LOGI(GATTC_TAG, "searched Adv Data Len %d, Scan Response Len %d", scan_result->scan_rst.adv_data_len, scan_result->scan_rst.scan_rsp_len);
            adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                                ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
            // ESP_LOGI(GATTC_TAG, "searched Device Name Len %d", adv_name_len);
            // ESP_LOG_BUFFER_CHAR(GATTC_TAG, adv_name, adv_name_len);

#if CONFIG_EXAMPLE_DUMP_ADV_DATA_AND_SCAN_RESP
            if (scan_result->scan_rst.adv_data_len > 0) {
                ESP_LOGI(GATTC_TAG, "adv data:");
                ESP_LOG_BUFFER_HEX(GATTC_TAG, &scan_result->scan_rst.ble_adv[0], scan_result->scan_rst.adv_data_len);
            }
            if (scan_result->scan_rst.scan_rsp_len > 0) {
                ESP_LOGI(GATTC_TAG, "scan resp:");
                ESP_LOG_BUFFER_HEX(GATTC_TAG, &scan_result->scan_rst.ble_adv[scan_result->scan_rst.adv_data_len], scan_result->scan_rst.scan_rsp_len);
            }
#endif

            if (adv_name != NULL) {
                if (strlen(remote_device_name) == adv_name_len && strncmp((char *)adv_name, remote_device_name, adv_name_len) == 0) {
                    ESP_LOGI(GATTC_TAG, "searched device %s", remote_device_name);
                    if (connect == false) {
                        connect = true;
                        ESP_LOGI(GATTC_TAG, "connect to the remote device.");
                        esp_ble_gap_stop_scanning();
                        esp_ble_gattc_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if, scan_result->scan_rst.bda, scan_result->scan_rst.ble_addr_type, true);
                    }
                }
            }
            break;
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            break;
        default:
            break;
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(GATTC_TAG, "scan stop failed, error status = %x", param->scan_stop_cmpl.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "stop scan successfully");
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(GATTC_TAG, "adv stop failed, error status = %x", param->adv_stop_cmpl.status);
            break;
        }
        ESP_LOGI(GATTC_TAG, "stop adv successfully");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(GATTC_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    case ESP_GAP_BLE_SET_PKT_LENGTH_COMPLETE_EVT:
        ESP_LOGI(GATTC_TAG, "packet length updated: rx = %d, tx = %d, status = %d",
                  param->pkt_data_length_cmpl.params.rx_len,
                  param->pkt_data_length_cmpl.params.tx_len,
                  param->pkt_data_length_cmpl.status);
        break;
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        } else {
            ESP_LOGI(GATTC_TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    /* If the gattc_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gattc_if == gl_profile_tab[idx].gattc_if) {
                if (gl_profile_tab[idx].gattc_cb) {
                    gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
                }
            }
        }
    } while (0);
}

void ble_init(void)
{
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(GATTC_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    //register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret){
        ESP_LOGE(GATTC_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return;
    }

    //register the callback function to the gattc module
    ret = esp_ble_gattc_register_callback(esp_gattc_cb);
    if(ret){
        ESP_LOGE(GATTC_TAG, "%s gattc register failed, error code = %x", __func__, ret);
        return;
    }

    ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    if (ret){
        ESP_LOGE(GATTC_TAG, "%s gattc app register failed, error code = %x", __func__, ret);
    }
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret){
        ESP_LOGE(GATTC_TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }

    /* Create notification queue (task created when a callback is registered) */
    if (!notify_queue) {
        notify_queue = xQueueCreate(NOTIFY_QUEUE_LEN, sizeof(notify_msg_t));
        if (!notify_queue) {
            ESP_LOGE(GATTC_TAG, "Failed to create notify queue");
        }
    }
}

/* Helper: write single-byte to start command characteristic */
esp_err_t ble_write_cmd_start(uint8_t value)
{
    struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
    if (p->char_cmd_start_handle == 0) return ESP_ERR_INVALID_ARG;
    esp_gatt_write_type_t write_type = ESP_GATT_WRITE_TYPE_RSP;
    if (p->char_cmd_start_props & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) write_type = ESP_GATT_WRITE_TYPE_NO_RSP;
    return esp_ble_gattc_write_char(p->gattc_if,
                                    p->conn_id,
                                    p->char_cmd_start_handle,
                                    1,
                                    &value,
                                    write_type,
                                    ESP_GATT_AUTH_REQ_NONE);
}

/* Helper: write single-byte to end command characteristic */
esp_err_t ble_write_cmd_end(uint8_t value)
{
    struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
    if (p->char_cmd_end_handle == 0) return ESP_ERR_INVALID_ARG;
    esp_gatt_write_type_t write_type = ESP_GATT_WRITE_TYPE_RSP;
    if (p->char_cmd_end_props & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) write_type = ESP_GATT_WRITE_TYPE_NO_RSP;
    return esp_ble_gattc_write_char(p->gattc_if,
                                    p->conn_id,
                                    p->char_cmd_end_handle,
                                    1,
                                    &value,
                                    write_type,
                                    ESP_GATT_AUTH_REQ_NONE);
}

/* Helper: check if a given 16-bit characteristic UUID is available (has a handle) */
bool ble_char_available(uint16_t uuid)
{
    struct gattc_profile_inst *p = &gl_profile_tab[PROFILE_A_APP_ID];
    switch (uuid) {
        case 0xFF01: return p->char_meas_p1_handle != 0;
        case 0xFF02: return p->char_meas_p2_handle != 0;
        case 0xFF03: return p->char_game_end_handle != 0;
        case 0xFF04: return p->char_cmd_start_handle != 0;
        case 0xFF05: return p->char_cmd_end_handle != 0;
        default: return false;
    }
}

void ble_register_notify_cb(ble_notify_cb_t cb)
{
    user_notify_cb = cb;
}