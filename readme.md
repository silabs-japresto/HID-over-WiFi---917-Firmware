# SiWG917 Wi-Fi A/B Gamepad Sender

> **Example only:** This code is an example and is not intended for production devices. Review and address security considerations before adapting it for production use.

Firmware for BRD4338A on BRD4002A. BTN0 sends gamepad A (HID Button usage 1), and BTN1 sends B (HID Button usage 2). The BRD4002A joystick is not available with this radio board. See the [BRD4338A user guide](https://www.silabs.com/documents/public/user-guides/ug562-brd4338a-user-guide.pdf) and [USB HID usage tables](https://www.usb.org/sites/default/files/hut1_21_0.pdf).

The firmware advertises as `HID-WIFI-917` over BLE for initial Wi-Fi setup. It stores the SSID and WPA2 password in NVM, rejoins after reset, and sends six-byte UDP button state packets to a host at port 5004. It scans the buttons every 5 ms, sends immediately on a change, and refreshes every 50 ms. The host times out after 250 ms if packets stop.

## Configure

Copy `hid_wifi_local.h.example` to `hid_wifi_local.h` and set the host computer's LAN IPv4 address. The local header is ignored by Git. Replace the placeholder address before building, and rebuild if the host address changes. Wi-Fi credentials are entered through the host's `provision.py` tool, never compiled into the firmware.

Get the Python provisioning tool and receiver from the [HID over Wi-Fi Host repository](https://github.com/silabs-japresto/HID-over-WiFi---Host). Install its requirements, flash this firmware, then run `python provision.py --ssid YOUR_WIFI_NAME` and enter the password when prompted. Run `python app.py` to see the buttons. The board and host must be on the same reachable LAN. See the host repository's README for Windows, Linux, and macOS setup instructions.

Provisioning uses a custom BLE GATT service (`0xAABB`) with an encrypted write characteristic (`0x1AA1`) and a one-byte status characteristic (`0x1BB1`). Write packets are at most 20 bytes: `0x01, offset, SSID bytes` or `0x02, offset, password bytes`, followed by the one-byte commit command `0x03`. Offsets start at zero and increase by the byte count sent. Status is 0 waiting, 1 joining, 2 connected, 3 join failed, or 4 invalid command. The Python tool handles chunking and pairing prompts.

## Build

Open `hid_over_wifi.slcp` in Simplicity Studio 6 or use the generated CMake project in `cmake_gcc`:

```sh
cmake --preset project
cmake --build --preset default_config
```

Run those commands from `cmake_gcc`. The output is in `cmake_gcc/build/base`. This project uses Simplicity SDK 2026.6.1 and WiSeConnect 4.1.1. Regenerate project files with those packages after changing `.slcp` components.

## Packet

Six bytes: ASCII `HW`, version `1`, wrapping sequence number, button bits (`bit 0 = A`, `bit 1 = B`), reserved zero. Each datagram is a complete state snapshot. A sequence number lets the receiver reject duplicates and stale packets. No host acknowledgment is required.

This is a trusted LAN prototype. UDP packets are unauthenticated; avoid using it on an untrusted network.
