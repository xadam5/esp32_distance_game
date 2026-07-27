#ifndef BLE_DRIVER_H
#define BLE_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*ble_notify_cb_t)(uint16_t uuid16, const uint8_t *data, uint16_t len);

void ble_init(void);
esp_err_t ble_write_cmd_start(uint8_t value);
esp_err_t ble_write_cmd_end(uint8_t value);
bool ble_char_available(uint16_t uuid16);
void ble_register_notify_cb(ble_notify_cb_t cb);
bool ble_is_connected(void);

/**
 * Blocking receive for the next notification. If `timeout_ms` is negative, wait indefinitely.
 * Returns true if a notification was received and populated into `out_uuid`, `out_buf` and `out_len`.
 */
bool ble_wait_for_notification(int32_t timeout_ms, uint16_t *out_uuid, uint8_t *out_buf, uint16_t *out_len);


#endif // BLE_DRIVER_H