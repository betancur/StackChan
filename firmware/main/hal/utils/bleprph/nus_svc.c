/*
 * Nordic UART Service (NUS) implementation on NimBLE.
 */
#include "nus_svc.h"
#include "bleprph.h"
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "os/os_mbuf.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NUS";

/* 6e400001-b5a3-f393-e0a9-e50e24dcca9e (little-endian byte order) */
const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                                                    0x01, 0x00, 0x40, 0x6e);
/* 6e400002-... */
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                                          0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
/* 6e400003-... */
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,
                                                          0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define NUS_RX_MAX 512

static uint16_t s_rx_handle;
static uint16_t s_tx_handle;
static volatile bool s_subscribed  = false;
static nus_rx_callback_t s_rx_cb   = NULL;
static uint8_t s_rx_buf[NUS_RX_MAX];

static int nus_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == s_rx_handle) {
        uint16_t len = 0;
        int rc       = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf, sizeof(s_rx_buf), &len);
        if (rc != 0) {
            ESP_LOGE(TAG, "rx too large (%d bytes)", OS_MBUF_PKTLEN(ctxt->om));
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (s_rx_cb && len > 0) {
            s_rx_cb(s_rx_buf, len);
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_tx_handle) {
        /* TX is notify-only; return empty value for reads */
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_svc_def nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid       = &nus_rx_uuid.u,
                    .access_cb  = nus_access,
                    .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                    .val_handle = &s_rx_handle,
                },
                {
                    .uuid       = &nus_tx_uuid.u,
                    .access_cb  = nus_access,
                    .flags      = BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_tx_handle,
                },
                {
                    0, /* No more characteristics */
                },
            },
    },
    {
        0, /* No more services */
    },
};

int nus_svc_register(void)
{
    int rc = ble_gatts_count_cfg(nus_svcs);
    if (rc != 0) {
        return rc;
    }
    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) {
        return rc;
    }
    ESP_LOGI(TAG, "NUS service registered");
    return 0;
}

void nus_svc_set_rx_callback(nus_rx_callback_t cb)
{
    s_rx_cb = cb;
}

bool nus_svc_is_subscribed(void)
{
    return s_subscribed;
}

void nus_svc_on_subscribe(uint16_t attr_handle, bool notify_enabled)
{
    if (attr_handle == s_tx_handle) {
        s_subscribed = notify_enabled;
        ESP_LOGI(TAG, "TX notifications %s", notify_enabled ? "enabled" : "disabled");
    }
}

void nus_svc_on_disconnect(void)
{
    s_subscribed = false;
}

int nus_svc_send(const uint8_t *data, uint16_t len)
{
    uint16_t conn = stackchan_ble_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return BLE_HS_ENOTCONN;
    }

    uint16_t mtu   = ble_att_mtu(conn);
    uint16_t chunk = (mtu > 3) ? (mtu - 3) : 20;

    while (len > 0) {
        uint16_t n = (len < chunk) ? len : chunk;

        int rc = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
            if (!om) {
                rc = BLE_HS_ENOMEM;
            } else {
                /* ble_gatts_notify_custom consumes the mbuf on every path */
                rc = ble_gatts_notify_custom(conn, s_tx_handle, om);
            }
            if (rc != BLE_HS_ENOMEM) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10)); /* out of mbufs: wait for the controller to drain */
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed rc=%d", rc);
            return rc;
        }

        data += n;
        len -= n;
    }
    return 0;
}
