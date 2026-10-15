#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    char ssid[33];
    char password[65];
} hid_wifi_credentials_t;

// Call after sl_net_init() has booted the radio in Wi-Fi/BLE coexistence mode.
bool ble_provision_start(void);
bool ble_provision_take(hid_wifi_credentials_t *credentials);
void ble_provision_set_status(uint8_t status);

#ifdef __cplusplus
}
#endif

