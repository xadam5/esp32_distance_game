#pragma once

#include <esp_event.h>
#include <esp_mac.h>
#include <esp_wifi.h>
#include <esp_log.h>

extern volatile bool wifi_connected;

void wifi_init_sta(char* ssid, char* password);
void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);