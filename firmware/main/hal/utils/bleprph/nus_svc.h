/*
 * Nordic UART Service (NUS) for the Claude Desktop Buddy protocol.
 *
 * Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX (desktop -> device, write):  6e400002-...
 *   TX (device -> desktop, notify): 6e400003-...
 *
 * Bytes written to RX are handed to the registered callback as-is (the caller
 * is responsible for line framing). nus_svc_send() splits payloads into
 * MTU-sized notifications.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "host/ble_uuid.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const ble_uuid128_t nus_svc_uuid;

typedef void (*nus_rx_callback_t)(const uint8_t *data, uint16_t len);

/* Register the NUS GATT service. Must be called before the NimBLE host starts. */
int nus_svc_register(void);

void nus_svc_set_rx_callback(nus_rx_callback_t cb);

/* Send raw bytes over TX notifications. Returns 0 on success, NimBLE error otherwise. */
int nus_svc_send(const uint8_t *data, uint16_t len);

bool nus_svc_is_subscribed(void);

/* Hooks called from the GAP event handler */
void nus_svc_on_subscribe(uint16_t attr_handle, bool notify_enabled);
void nus_svc_on_disconnect(void);

#ifdef __cplusplus
}
#endif
