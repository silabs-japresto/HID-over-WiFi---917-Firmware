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
#include <cstdio>
#include <cerrno>
extern "C" {
#include "cmsis_os2.h"
#include "sl_net.h"
#include "sl_net_default_values.h"
#include "sl_si91x_button.h"
#include "sl_si91x_button_instances.h"
#include "sl_si91x_socket.h"
#include "sl_wifi.h"
}

extern "C" {
volatile uint32_t hid_radio_init_status = 0xffffffff;
}

namespace {
constexpr uint8_t kVersion = 1;
constexpr uint32_t kPollMs = 5;
constexpr uint32_t kRefreshMs = 50;
constexpr uint32_t kRetryMs = 1000;
constexpr uint16_t kDiscoveryPort = 5005;
constexpr uint32_t kHostLeaseMs = 4000;
constexpr uint8_t kHostMagic[] = {'H', 'W', 'H', 1};
constexpr uint8_t kDiscoveryQuery[] = {'H', 'W', '?', 1};
uint8_t packet[6] = {'H', 'W', kVersion, 0, 0, 0};
osMessageQueueId_t discovery_queue;
struct DiscoveryMessage {
  sockaddr_in source;
  uint8_t data[8];
};
bool discovery_network_ready = false;
int discovery_socket = -1;
uint32_t last_discovery_open = 0;
uint32_t last_query = 0;
sockaddr_in discovered_host = {};
uint32_t host_seen = 0;
bool host_selected = false;

uint32_t milliseconds()
{
  return static_cast<uint32_t>(static_cast<uint64_t>(osKernelGetTickCount()) * 1000
                               / osKernelGetTickFreq());
}

void discovery_network(bool ready)
{
  if (discovery_socket >= 0) {
    sl_si91x_shutdown(discovery_socket, 0);
    discovery_socket = -1;
  }
  osMessageQueueReset(discovery_queue);
  discovery_network_ready = ready;
  host_selected = false;
  last_discovery_open = milliseconds() - kRetryMs;
}

bool current_host(sockaddr_in &host)
{
  if (!host_selected || milliseconds() - host_seen >= kHostLeaseMs) {
    return false;
  }
  host = discovered_host;
  return true;
}

void discovery_receive(uint32_t, uint8_t *data, uint32_t size,
                       const sl_si91x_socket_metadata_t *metadata)
{
  if (size != 8 || metadata == nullptr || metadata->ip_version != 4
      || std::memcmp(data, kHostMagic, sizeof(kHostMagic)) != 0) {
    return;
  }
  DiscoveryMessage message = {};
  message.source.sin_family = AF_INET;
  message.source.sin_port = metadata->dest_port;
  std::memcpy(&message.source.sin_addr.s_addr, metadata->dest_ip_addr.ipv4_address, 4);
  std::memcpy(message.data, data, sizeof(message.data));
  // The callback runs on the driver's receive task: never block it.
  osMessageQueuePut(discovery_queue, &message, 0, 0);
}

void discover_hosts()
{
  if (!discovery_network_ready) {
    return;
  }
  const uint32_t now = milliseconds();
  if (host_selected && now - host_seen >= kHostLeaseMs) {
    host_selected = false;
    std::printf("HID host expired; searching again\n");
  }
  if (discovery_socket < 0 && now - last_discovery_open >= kRetryMs) {
    last_discovery_open = now;
    discovery_socket = sl_si91x_socket_async(AF_INET, SOCK_DGRAM, IPPROTO_UDP, discovery_receive);
    if (discovery_socket >= 0) {
      sockaddr_in local = {};
      local.sin_family = AF_INET;
      // Si91x asynchronous socket ports use host byte order.
      local.sin_port = kDiscoveryPort;
      if (sl_si91x_bind(discovery_socket, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        std::printf("HID discovery bind failed\n");
        sl_si91x_shutdown(discovery_socket, 0);
        discovery_socket = -1;
      } else {
        std::printf("HID host discovery listening on UDP %u\n", kDiscoveryPort);
        last_query = now - kRetryMs;
      }
    }
  }
  if (discovery_socket < 0) {
    return;
  }
  if ((!host_selected || now - host_seen >= 1500) && now - last_query >= kRetryMs) {
    sockaddr_in probe = discovered_host;
    probe.sin_family = AF_INET;
    if (!host_selected) {
      sl_net_inet_addr("255.255.255.255", &probe.sin_addr.s_addr);
    }
    probe.sin_port = kDiscoveryPort;
    if (sl_si91x_sendto(discovery_socket, kDiscoveryQuery, sizeof(kDiscoveryQuery), 0,
                        reinterpret_cast<sockaddr *>(&probe), sizeof(probe)) < 0) {
      std::printf("HID discovery probe failed: %d\n", errno);
    }
    last_query = now;
  }
  // Bound queue work and never wait on network reads in the button loop.
  for (unsigned int i = 0; i < 4; ++i) {
    DiscoveryMessage message;
    if (osMessageQueueGet(discovery_queue, &message, nullptr, 0) != osOK) {
      break;
    }
    const uint8_t *buffer = message.data;
    const sockaddr_in &source = message.source;
    if (buffer[6] != 0 || buffer[7] != 0 || source.sin_port != kDiscoveryPort
        || source.sin_addr.s_addr == 0) {
      continue;
    }
    const uint16_t port = static_cast<uint16_t>((buffer[4] << 8) | buffer[5]);
    if (port == 0) {
      continue;
    }
    if (!host_selected || (source.sin_addr.s_addr == discovered_host.sin_addr.s_addr
                           && port == discovered_host.sin_port)) {
      if (!host_selected) {
        std::printf("HID host discovered on UDP port %u\n", port);
      }
      discovered_host = source;
      discovered_host.sin_port = port;
      host_seen = milliseconds();
      host_selected = true;
    }
  }
}

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
  const sl_status_t status = sl_net_up(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID);
  std::printf("Wi-Fi connection status: 0x%08lx\n", static_cast<unsigned long>(status));
  sl_net_wifi_client_profile_t profile = {};
  if (status == SL_STATUS_OK
      && sl_net_get_profile(SL_NET_WIFI_CLIENT_INTERFACE, SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &profile) == SL_STATUS_OK) {
    const uint8_t *ip = profile.ip.ip.v4.ip_address.bytes;
    std::printf("Wi-Fi IPv4 address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    const sl_wifi_groupcast_filter_config_t groupcast = {};
    const sl_status_t filter_status = sl_wifi_set_groupcast_filter_config(&groupcast);
    std::printf("Wi-Fi groupcast filter status: 0x%08lx\n", static_cast<unsigned long>(filter_status));
  }
  return status == SL_STATUS_OK;
}

void run(void *)
{
  if (!initialize_radio()) {
    std::printf("Radio initialization failed: 0x%08lx\n", static_cast<unsigned long>(hid_radio_init_status));
    for (;;) {
      osDelay(kRetryMs);
    }
  }
  std::printf("BLE provisioning startup: %s\n", ble_provision_start() ? "ready" : "failed");

  sl_net_wifi_client_profile_t saved = {};
  bool connected = false;
  if (sl_net_get_profile(SL_NET_WIFI_CLIENT_INTERFACE,
                         SL_NET_DEFAULT_WIFI_CLIENT_PROFILE_ID, &saved) == SL_STATUS_OK
      && saved.config.ssid.length > 0) {
    ble_provision_set_status(1);
    connected = connect_wifi();
    std::printf("Saved Wi-Fi profile connection: %s\n", connected ? "connected" : "failed");
    ble_provision_set_status(connected ? 2 : 3);
  }

  discovery_network(connected);

  int socket = -1;
  sockaddr_in host = {};
  bool had_host = false;
  uint8_t last_state = 0xff;
  uint32_t last_sent = 0;
  for (;;) {
    hid_wifi_credentials_t credentials = {};
    if (ble_provision_take(&credentials)) {
      discovery_network(false);
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
      std::printf("Provisioned Wi-Fi profile connection: %s\n", connected ? "connected" : "failed");
      std::memset(&credentials, 0, sizeof(credentials));
      ble_provision_set_status(connected ? 2 : 3);
      discovery_network(connected);
      last_state = 0xff;
    }
    if (!connected) {
      osDelay(kPollMs);
      continue;
    }
    discover_hosts();
    sockaddr_in selected = {};
    if (!current_host(selected)) {
      had_host = false;
      osDelay(kPollMs);
      continue;
    }
    if (!had_host || host.sin_addr.s_addr != selected.sin_addr.s_addr
        || host.sin_port != selected.sin_port) {
      host = selected;
      last_state = 0xff;
    }
    had_host = true;
    if (socket < 0) {
      socket = sl_si91x_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (socket < 0) {
        osDelay(kRetryMs);
        continue;
      }
    }
    const uint32_t now = milliseconds();
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
  discovery_queue = osMessageQueueNew(4, sizeof(DiscoveryMessage), nullptr);
  if (discovery_queue == nullptr) {
    return;
  }
  static osThreadAttr_t attributes = {};
  attributes.name = "hid_wifi";
  attributes.stack_size = 4096;
  attributes.priority = osPriorityNormal;
  osThreadNew(run, nullptr, &attributes);
}

extern "C" void app_process_action(void) {}
