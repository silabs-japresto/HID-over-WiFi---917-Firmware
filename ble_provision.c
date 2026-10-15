#include "ble_provision.h"

#include <string.h>
#include "cmsis_os2.h"
#include "rsi_ble_apis.h"
#include "rsi_bt_common_apis.h"
#include "rsi_common_apis.h"
#include "nvm3.h"

// Custom service: write 20-byte chunks to 1AA1; read status from 1BB1.
// Wire format is documented in readme.md and host provision.py.
#define SERVICE_UUID 0xAABB
#define COMMAND_UUID 0x1AA1
#define STATUS_UUID  0x1BB1
#define DEVICE_NAME "HID-WIFI-917"
#define ATT_READ  0x02
#define ATT_WRITE 0x08
#define ATT_HOST  0x01
#define ATT_ENCRYPTED 0x04
#define BLE_BOND_KEY 0x4000

typedef struct
{
    uint8_t kind;
    uint8_t length;
    uint8_t peer[6];
    uint8_t data[sizeof(rsi_bt_event_encryption_enabled_t)];
} ble_message_t;

static osMessageQueueId_t event_queue;
static osMessageQueueId_t credential_queue;
static uint16_t command_handle;
static uint16_t status_handle;
static volatile uint8_t current_status;
volatile int32_t hid_ble_pair_request_status = -1;
volatile uint32_t hid_ble_smp_response_count;
volatile uint32_t hid_ble_stage;
volatile uint32_t hid_ble_connection_count;
volatile uint32_t hid_ble_ltk_request_count;
volatile int32_t hid_ble_ltk_reply_status = -1;
volatile int32_t hid_ble_bond_save_status = -1;
volatile int32_t hid_ble_pair_response_status = -1;
volatile int32_t hid_ble_smp_failed_status = -1;
volatile int32_t hid_ble_pairing_config_status = -1;

static void queue_connection(const uint8_t *peer)
{
    hid_ble_connection_count++;
    ble_message_t message = { .kind = 3 };

    memcpy(message.peer, peer, sizeof(message.peer));
    osMessageQueuePut(event_queue, &message, 0, 0);
}

static void on_connect(rsi_ble_event_conn_status_t *event)
{
    if (event->status == RSI_SUCCESS)
    {
        queue_connection(event->dev_addr);
    }
}

static void on_enhanced_connect(rsi_ble_event_enhance_conn_status_t *event)
{
    if (event->status == RSI_SUCCESS)
    {
        queue_connection(event->dev_addr);
    }
}

static void on_ltk_request(rsi_bt_event_le_ltk_request_t *event)
{
    hid_ble_ltk_request_count++;
    ble_message_t message = { .kind = 5 };

    memcpy(message.peer, event->dev_addr, sizeof(message.peer));
    osMessageQueuePut(event_queue, &message, 0, 0);
}

static void on_encryption(uint16_t status, rsi_bt_event_encryption_enabled_t *event)
{
    if (status != RSI_SUCCESS || event == NULL || !(event->enabled & 1))
    {
        return;
    }

    ble_message_t message = { .kind = 6 };

    memcpy(message.data, event, sizeof(*event));
    osMessageQueuePut(event_queue, &message, 0, 0);
}

static void on_smp_response(rsi_bt_event_smp_resp_t *event)
{
    hid_ble_smp_response_count++;
    ble_message_t message = { .kind = 4 };

    memcpy(message.peer, event->dev_addr, sizeof(message.peer));
    osMessageQueuePut(event_queue, &message, 0, 0);
}

static void on_smp_failed(uint16_t status, rsi_bt_event_smp_failed_t *event)
{
    (void)event;
    hid_ble_smp_failed_status = status;
}

static void on_disconnect(rsi_ble_event_disconnect_t *event, uint16_t reason)
{
    (void)event;
    (void)reason;
    ble_message_t message = { .kind = 2 };

    osMessageQueuePut(event_queue, &message, 0, 0);
}

static void on_write(uint16_t event_id, rsi_ble_event_write_t *event)
{
    (void)event_id;
    const uint16_t handle = (uint16_t)event->handle[0] | ((uint16_t)event->handle[1] << 8);

    if (handle != command_handle || event->length > 20)
    {
        return;
    }

    ble_message_t message = { .kind = 1, .length = event->length };

    memcpy(message.peer, event->dev_addr, sizeof(message.peer));
    memcpy(message.data, event->att_value, event->length);
    osMessageQueuePut(event_queue, &message, 0, 0);
}

static int32_t add_characteristic(void *service, uint16_t declaration_handle,
                                  uint16_t uuid, uint8_t properties, uint8_t security)
{
    rsi_ble_req_add_att_t attribute = { 0 };

    attribute.serv_handler = service;
    attribute.handle = declaration_handle;
    attribute.att_uuid.size = 2;
    attribute.att_uuid.val.val16 = 0x2803;
    attribute.property = ATT_READ;
    // WiSeConnect's add-attribute API uses the same six-byte declaration layout
    // as the SDK provisioning example (property, reserved, handle, UUID).
    attribute.data_len = 6;
    attribute.data[0] = properties;
    rsi_uint16_to_2bytes(&attribute.data[2], declaration_handle + 1);
    rsi_uint16_to_2bytes(&attribute.data[4], uuid);

    if (rsi_ble_add_attribute(&attribute) != RSI_SUCCESS)
    {
        return -1;
    }

    memset(&attribute, 0, sizeof(attribute));
    attribute.serv_handler = service;
    attribute.handle = declaration_handle + 1;
    attribute.att_uuid.size = 2;
    attribute.att_uuid.val.val16 = uuid;
    attribute.property = properties;
    attribute.config_bitmap = security;
    attribute.data_len = properties == ATT_READ ? 1 : 20;

    return rsi_ble_add_attribute(&attribute);
}

static bool create_service(void)
{
    uuid_t uuid = { 0 };
    uuid.size = 2;
    uuid.val.val16 = SERVICE_UUID;

    rsi_ble_resp_add_serv_t response = { 0 };

    if (rsi_ble_add_service(uuid, &response) != RSI_SUCCESS)
    {
        return false;
    }
    command_handle = response.start_handle + 2;
    status_handle = response.start_handle + 4;

    return add_characteristic(response.serv_handler, response.start_handle + 1,
                              COMMAND_UUID, ATT_WRITE, ATT_HOST | ATT_ENCRYPTED) == RSI_SUCCESS
            && add_characteristic(response.serv_handler, response.start_handle + 3,
                                  STATUS_UUID, ATT_READ, 0) == RSI_SUCCESS;
}

static bool append_chunk(char *target, size_t capacity, size_t *used,
                         const ble_message_t *message)
{
    if (message->length < 2)
    {
        return false;
    }

    const size_t offset = message->data[1];
    const size_t length = message->length - 2;

    if (offset == 0)
    {
        memset(target, 0, capacity);
        *used = 0;
    }

    if (offset != *used || offset + length >= capacity)
    {
        return false;
    }

    memcpy(target + offset, message->data + 2, length);
    *used += length;
    target[*used] = '\0';

    return true;
}

static void ble_task(void *argument)
{
    (void)argument;
    hid_ble_stage = 4;
    rsi_ble_gap_register_callbacks(NULL, on_connect, on_disconnect, NULL, NULL,
                                   NULL, on_enhanced_connect, NULL, NULL, NULL);
    rsi_ble_smp_register_callbacks(NULL, on_smp_response, NULL, on_smp_failed, on_encryption,
                                   NULL, NULL, on_ltk_request, NULL, NULL, NULL);
    rsi_ble_gatt_register_callbacks(NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                    on_write, NULL, NULL, NULL, NULL, NULL, NULL,
                                    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                    NULL);

    if (!create_service())
    {
        hid_ble_stage = 5;
        return;
    }
    hid_ble_stage = 6;

    // The board has no display or keyboard. Advertise Just Works bonding so
    // BlueZ does not request numeric comparison from an inputless peripheral.
    rsi_ble_set_smp_pairing_capabilty_data_t pairing = { 0 };

    pairing.io_capability = 3;
    pairing.auth_req = 0x09; // Bonding + LE Secure Connections, no MITM.
    pairing.enc_key_size = 16;
    pairing.ini_key_distribution = 0x03; // Encryption and identity keys.
    pairing.rsp_key_distribution = 0x03;
    hid_ble_pairing_config_status = rsi_ble_set_smp_pairing_cap_data(&pairing);

    rsi_bt_set_local_name((uint8_t *)DEVICE_NAME);
    uint8_t advertisement[5 + sizeof(DEVICE_NAME) - 1] = { 2, 1, 6,
        sizeof(DEVICE_NAME), 9 };
    memcpy(advertisement + 5, DEVICE_NAME, sizeof(DEVICE_NAME) - 1);
    rsi_ble_set_advertise_data(advertisement, sizeof(advertisement));
    rsi_ble_start_advertising();
    hid_ble_stage = 7;

    hid_wifi_credentials_t pending = { 0 };
    size_t ssid_used = 0;
    size_t password_used = 0;
    uint8_t published_status = 0xff;

    for (;;)
    {
        ble_message_t message;

        if (osMessageQueueGet(event_queue, &message, NULL, 100) == osOK)
        {
            if (message.kind == 2)
            {
                memset(&pending, 0, sizeof(pending));
                ssid_used = password_used = 0;
                rsi_ble_start_advertising();
            }
            else if (message.kind == 3)
            {
                // No display/input: Just Works pairing encrypts the link before the
                // host sends the password. The macOS client may show a pairing dialog.
                hid_ble_pair_request_status = rsi_ble_smp_pair_request(message.peer, 3, 0);
            }
            else if (message.kind == 4)
            {
                hid_ble_pair_response_status = rsi_ble_smp_pair_response(message.peer, 3, 0);
            }
            else if (message.kind == 5)
            {
                rsi_bt_event_encryption_enabled_t bond = { 0 };

                if (nvm3_readData(nvm3_defaultHandle, BLE_BOND_KEY,
                                  &bond, sizeof(bond)) == ECODE_NVM3_OK)
                {
                    hid_ble_ltk_reply_status = rsi_ble_ltk_req_reply(message.peer,
                        1 | bond.enabled | (bond.sc_enable << 7), bond.localltk);
                }
                else
                {
                    hid_ble_ltk_reply_status = rsi_ble_ltk_req_reply(message.peer, 0, NULL);
                }
            }
            else if (message.kind == 6)
            {
                hid_ble_bond_save_status = nvm3_writeData(nvm3_defaultHandle, BLE_BOND_KEY,
                                                         message.data,
                                                         sizeof(rsi_bt_event_encryption_enabled_t));
            }
            else if (message.kind == 1)
            {
                bool valid = message.length != 0;

                if (valid && message.data[0] == 1)
                {
                    valid = append_chunk(pending.ssid, sizeof(pending.ssid), &ssid_used, &message);
                }
                else if (valid && message.data[0] == 2)
                {
                    valid = append_chunk(pending.password, sizeof(pending.password), &password_used, &message);
                }
                else if (valid && message.data[0] == 3)
                {
                    valid = message.length == 1 && ssid_used > 0 && password_used >= 8
                              && osMessageQueuePut(credential_queue, &pending, 0, 0) == osOK;

                    if (valid)
                    {
                        current_status = 1;
                        memset(&pending, 0, sizeof(pending));
                        ssid_used = password_used = 0;
                    }
                }
                else
                {
                    valid = false;
                }

                if (!valid)
                {
                    current_status = 4;
                }

                rsi_ble_gatt_write_response(message.peer, valid ? 0 : 1);
            }
        }

        if (published_status != current_status)
        {
            published_status = current_status;
            rsi_ble_set_local_att_value(status_handle, 1, &published_status);
        }
    }
}

bool ble_provision_start(void)
{
    hid_ble_stage = 1;
    event_queue = osMessageQueueNew(8, sizeof(ble_message_t), NULL);
    hid_ble_stage = 2;
    credential_queue = osMessageQueueNew(1, sizeof(hid_wifi_credentials_t), NULL);
    hid_ble_stage = 3;

    if (!event_queue || !credential_queue)
    {
        hid_ble_stage = 8;
        return false;
    }

    static const osThreadAttr_t attributes = {
        .name = "ble_setup", .stack_size = 4096, .priority = osPriorityNormal
    };
    const bool started = osThreadNew(ble_task, NULL, &attributes) != NULL;

    if (!started)
    {
        hid_ble_stage = 9;
    }

    return started;
}

bool ble_provision_take(hid_wifi_credentials_t *credentials)
{
    return osMessageQueueGet(credential_queue, credentials, NULL, 0) == osOK;
}

void ble_provision_set_status(uint8_t status)
{
    current_status = status;
}
