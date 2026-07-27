#include <stdint.h>
#ifndef BLE_DRIVER_H
#define BLE_DRIVER_H

bool ble_driver_is_connected(void);

void ble_setup(void);

void ble_driver_send_measurements(uint16_t player1_measurement, uint16_t player2_measurement);

void ble_driver_send_game_ending(bool is_ending);

bool ble_driver_is_round_Start_requested(void);

bool ble_driver_is_game_end_requested(void);

void ble_driver_clear_round_Start_requested(void);

void ble_driver_clear_game_end_requested(void);

#endif // BLE_DRIVER_H