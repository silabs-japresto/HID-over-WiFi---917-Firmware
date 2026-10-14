/***************************************************************************/ /**
 * @file
 * @brief Wi-Fi gamepad sender and application lifecycle.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#include "app.h"
#include "ble_provision.h"

#include <cstring>
#include "hid_wifi_local.h"
extern "C" {
#include "cmsis_os2.h"
#include "sl_net.h"
#include "sl_net_default_values.h"
#include "sl_si91x_button.h"
#include "sl_si91x_button_instances.h"
#include "sl_si91x_socket.h"
#include "sl_wifi.h"
}

#ifndef HID_HOST_PORT
#define HID_HOST_PORT 5004
#endif

extern "C" {
volatile uint32_t hid_radio_init_status = 0xffffffff;
}

namespace {
constexpr uint8_t kVersion = 1;
constexpr uint32_t kPollMs = 5;
constexpr uint32_t kRefreshMs = 50;
constexpr uint32_t kRetryMs = 1000;
uint8_t packet[6] = {'H', 'W', kVersion, 0, 0, 0};

uint8_t button_state()
{
  // The driver's shadow state is only updated by application callbacks. Read
  // the active-low physical inputs so held buttons and releases are reliable.
  return static_cast<uint8_t>((sl_si91x_button_state(button_btn0.pin, button_btn0.port) == 0 ? 1 : 0)
                              | (sl_si91x_button_state(button_btn1.pin, button_btn1.port) == 0 ? 2 : 0));
}

bool initialize_radio()
{
  sl_wifi_device_configuration_t config = sl_wifi_default_client_configuration;
  config.boot_config.coex_mode = SL_SI91X_WLAN_BLE_MODE;
  config.boot_config.ext_custom_feature_bit_map |= SL_SI91X_EXT_FEAT_BT_CUSTOM_FEAT_ENABLE;
  config.boot_config.bt_feature_bit_map = SL_SI91X_BT_RF_TYPE | SL_SI91X_ENABLE_BLE_PROTOCOL;
  config.boot_config.ble_feature_bit_map =
      SL_SI91X_BLE_MAX_NBR_PERIPHERALS(1)
    | SL_SI91X_BLE_MAX_NBR_CENTRALS(1)
    | SL_SI91X_BLE_MAX_NBR_ATT_SERV(4)
    | SL_SI91X_BLE_MAX_NBR_ATT_REC(20)
    | SL_SI91X_FEAT_BLE_CUSTOM_FEAT_EXTENSION_VALID
    | SL_SI91X_BLE_PWR_INX(30)
    | SL_SI91X_916_BLE_COMPATIBLE_FEAT_ENABLE;
  config.boot_config.ble_ext_feature_bit_map =
      SL_SI91X_BLE_NUM_CONN_EVENTS(2) | SL_SI91X_BLE_NUM_REC_BYTES(0x40);
  hid_radio_init_status = sl_net_init(SL_NET_WIFI_CLIENT_INTERFACE, &config, nullptr, nullptr);
  return hid_radio_init_status == SL_STATUS_OK;
}

bool save_profile(const hid_wifi_credentials_t &credentials)
{
  sl_net_wifi_client_profile_t profile = {};
  const size_t ssid_length = std::strlen(credentials.ssid);
  if (ssid_length == 0 || ssid_length > 32) {
    return false;
  }
  std::memcpy(profile.config.ssid.value, credentials.ssid, ssid_length);
  profile.config.ssid.length = static_cast<uint8_t>(ssid_length);
  profile.config.channel.channel = SL_WIFI_AUTO_CHANNEL;
  profile.config.channel.band = SL_WIFI_AUTO_BAND;
  profile.config.channel.bandwidth = SL_WIFI_AUTO_BANDWIDTH;
  profile.config.channel_bitmap.channel_bitmap_2_4 = SL_WIFI_DEFAULT_CHANNEL_BITMAP;
  profile.config.bss_type = SL_WIFI_BSS_TYPE_INFRASTRUCTURE;
  profile.config.security = SL_WIFI_WPA2;
  profile.config.encryption = SL_WIFI_DEFAULT_ENCRYPTION;
  profile.config.credential_id = SL_NET_DEFAULT_WIFI_CLIENT_CREDENTIAL_ID;
  profile.ip.mode = SL_IP_MANAGEMENT_DHCP;
  profile.ip.type = SL_IPV4;
  if (sl_net_set_credential(SL_NET_DEFAULT_WIFI_CLIENT_CREDENTIAL_ID,
                            SL_NET_WIFI_PSK, credentials.password,
                            std::strlen(credentials.password)) != SL_STATUS_OK) {
    return false;
  }
  if (sl_net_set_profile(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &profile) != SL_STATUS_OK) {
    return false;
  }
  return true;
}

bool connect_wifi()
{
  return sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID) == SL_STATUS_OK;
}

void run(void *)
{
  if (!initialize_radio()) {
    for (;;) {
      osDelay(kRetryMs);
    }
  }
  ble_provision_start();

  sl_net_wifi_client_profile_t saved = {};
  bool connected = false;
  if (sl_net_get_profile(SL_NET_WIFI_CLIENT_INTERFACE,
                         SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &saved) == SL_STATUS_OK
      && saved.config.ssid.length > 0) {
    ble_provision_set_status(1);
    connected = connect_wifi();
    ble_provision_set_status(connected ? 2 : 3);
  }

  sockaddr_in host = {};
  host.sin_family = AF_INET;
  host.sin_port = HID_HOST_PORT;
  if (sl_net_inet_addr(HID_HOST_IP, &host.sin_addr.s_addr) != SL_STATUS_OK) {
    return;
  }

  int socket = -1;
  uint8_t last_state = 0xff;
  uint32_t last_sent = 0;
  for (;;) {
    hid_wifi_credentials_t credentials = {};
    if (ble_provision_take(&credentials)) {
      if (connected) {
        sl_net_down(SL_NET_WIFI_CLIENT_INTERFACE);
        connected = false;
      }
      if (socket >= 0) {
        sl_si91x_shutdown(socket, 0);
        socket = -1;
      }
      ble_provision_set_status(1);
      connected = save_profile(credentials) && connect_wifi();
      std::memset(&credentials, 0, sizeof(credentials));
      ble_provision_set_status(connected ? 2 : 3);
      last_state = 0xff;
    }
    if (!connected) {
      osDelay(kPollMs);
      continue;
    }
    if (socket < 0) {
      socket = sl_si91x_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (socket < 0) {
        osDelay(kRetryMs);
        continue;
      }
    }
    const uint32_t now = osKernelGetTickCount();
    const uint8_t state = button_state();
    if (state != last_state || now - last_sent >= kRefreshMs) {
      packet[3]++;
      packet[4] = state;
      const int sent = sl_si91x_sendto(socket, packet, sizeof(packet), 0,
                                      reinterpret_cast<sockaddr *>(&host), sizeof(host));
      if (sent == static_cast<int>(sizeof(packet))) {
        last_state = state;
        last_sent = now;
      } else {
        sl_si91x_shutdown(socket, 0);
        socket = -1;
        osDelay(kRetryMs);
      }
    }
    osDelay(kPollMs);
  }
}
} // namespace

extern "C" void app_init(void)
{
  static osThreadAttr_t attributes = {};
  attributes.name = "hid_wifi";
  attributes.stack_size = 4096;
  attributes.priority = osPriorityNormal;
  osThreadNew(run, nullptr, &attributes);
}

extern "C" void app_process_action(void) {}
