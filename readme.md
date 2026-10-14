# SiWG917 Wi-Fi A/B Gamepad Sender

> **Example only:** This code is an example and is not intended for production devices. Review and address security considerations before adapting it for production use.

Firmware for BRD4338A on BRD4002A. BTN0 sends gamepad A (HID Button usage 1), and BTN1 sends B (HID Button usage 2). The BRD4002A joystick is not available with this radio board. See the [BRD4338A user guide](https://www.silabs.com/documents/public/user-guides/ug562-brd4338a-user-guide.pdf) and [USB HID usage tables](https://www.usb.org/sites/default/files/hut1_21_0.pdf).

The firmware advertises as `HID-WIFI-917` over BLE for initial Wi-Fi setup. It stores the SSID and WPA2 password in NVM, rejoins after reset, and sends six-byte UDP button state packets to an automatically discovered host (port 5004 by default). It scans the buttons every 5 ms, sends immediately on a change, and refreshes every 50 ms. The host times out after 250 ms if packets stop.

## Configure

Wi-Fi credentials are entered through the host's `provision.py` tool, never compiled into the firmware. The controller discovers the Python receiver automatically; no host IP address or local configuration header is required.

Get the Python provisioning tool and receiver from the [HID over Wi-Fi Host repository](https://github.com/silabs-japresto/HID-over-WiFi---Host). Install its requirements, flash this firmware, then run `python provision.py --ssid YOUR_WIFI_NAME` and enter the password when prompted. Run `python app.py` to see the buttons. The board and host must be on the same reachable LAN. See the host repository's README for Windows, Linux, and macOS setup instructions.

Provisioning uses a custom BLE GATT service (`0xAABB`) with an encrypted write characteristic (`0x1AA1`) and a one-byte status characteristic (`0x1BB1`). Write packets are at most 20 bytes: `0x01, offset, SSID bytes` or `0x02, offset, password bytes`, followed by the one-byte commit command `0x03`. Offsets start at zero and increase by the byte count sent. Status is 0 waiting, 1 joining, 2 connected, 3 join failed, or 4 invalid command. The Python tool handles chunking and pairing prompts.

## Build

Open `hid_over_wifi.slcp` in Simplicity Studio 6 or use the generated CMake project in `cmake_gcc`:

```sh
cmake --preset project
cmake --build --preset default_config
```

Run those commands from `cmake_gcc`. The output is in `cmake_gcc/build/base`. This project uses Simplicity SDK 2026.6.1 and WiSeConnect 4.1.1. Regenerate project files with those packages after changing `.slcp` components. Program the matching WiSeConnect 4.1.1 connectivity firmware (`SiWG917-B.2.16.5.1.0.8.rps`) as well as the application when switching from another SDK or the Zephyr build.

## Automatic host discovery

Start `app.py` or `headless.py` on the receiving computer. The host advertises its UDP listening port once per second on UDP port 5005 on each active IPv4 network interface. The controller learns the host's IP address from the advertisement's source address and sends its button packets directly to the advertised port (5004 by default). No receiver IP address is compiled into the firmware, and no receiver configuration file is required.

The controller keeps the first discovered host while its advertisements continue. After four seconds without an advertisement or probe reply, it stops sending to that endpoint and searches again. A host restart, changed IP address, or changed `--port` is handled automatically. Controllers also send a small broadcast probe while searching. If advertisements from a selected host are missed, the controller sends unicast renewal probes to that host. Run one receiver per computer; if multiple computers advertise, the controller selects the first one it finds.

Discovery requires the board and receiver to be on the same IPv4 broadcast network. Allow inbound UDP 5005 and the receiver's data port in the host firewall. Wi-Fi client isolation or VLAN boundaries can prevent discovery. Wi-Fi credentials are still configured separately through BLE.

Discovery advertisements are eight bytes: ASCII `HWH`, version `1`, the receiver port as a two-byte unsigned integer in network byte order, and two zero bytes. The sender's IPv4 address supplies the endpoint address. Discovery probes are four bytes: ASCII `HW?` followed by version `1`. Both use UDP 5005. Advertisements and button packets are unauthenticated; use a trusted LAN.

## Packet

Six bytes: ASCII `HW`, version `1`, wrapping sequence number, button bits (`bit 0 = A`, `bit 1 = B`), reserved zero. Each datagram is a complete state snapshot. A sequence number lets the receiver reject duplicates and stale packets. No host acknowledgment is required.

This is a trusted LAN prototype. UDP packets are unauthenticated; avoid using it on an untrusted network.
