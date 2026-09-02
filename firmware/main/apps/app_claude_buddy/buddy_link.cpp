/*
 * BuddyLink implementation
 */
#include "buddy_link.h"
#include <hal/hal.h>
#include <hal/utils/bleprph/bleprph.h>
#include <hal/utils/bleprph/nus_svc.h>
#include <mooncake_log.h>

extern "C" {
#include "host/ble_store.h"
}

namespace buddy {

static const std::string_view TAG = "BuddyLink";

BuddyLink& BuddyLink::instance()
{
    static BuddyLink link;
    return link;
}

void BuddyLink::start(std::string_view deviceName)
{
    if (_started) {
        mclog::tagWarn(TAG, "already started");
        return;
    }
    _started = true;
    nus_svc_set_rx_callback(&BuddyLink::rx_trampoline);
    ble_prph_set_passkey_callback(&BuddyLink::passkey_trampoline);
    ble_prph_set_link_callback([](ble_prph_link_event_t evt, int arg) { BuddyLink::link_trampoline(evt, arg); });
    GetHAL().startBuddyBleServer(deviceName);
}

bool BuddyLink::isConnected() const
{
    return stackchan_ble_is_connected();
}

bool BuddyLink::isReady() const
{
    return stackchan_ble_is_connected() && nus_svc_is_subscribed();
}

void BuddyLink::rx_trampoline(const uint8_t* data, uint16_t len)
{
    instance().onRx(data, len);
}

void BuddyLink::passkey_trampoline(uint32_t passkey, bool show)
{
    instance()._passkey.store(show ? passkey : 0);
    if (show) {
        mclog::tagInfo(TAG, "pairing passkey: {}", passkey);
    }
}

void BuddyLink::link_trampoline(int evt, int arg)
{
    auto& self = instance();
    switch (static_cast<ble_prph_link_event_t>(evt)) {
        case BLE_PRPH_EVT_CONNECTED:
            self._status = static_cast<int>(LinkStatus::Connected);
            break;
        case BLE_PRPH_EVT_DISCONNECTED:
            // Keep "PairFailed"/"IdentityRotated" visible while advertising again
            if (self._status == static_cast<int>(LinkStatus::Connected) ||
                self._status == static_cast<int>(LinkStatus::Encrypted)) {
                self._status = static_cast<int>(LinkStatus::Advertising);
            }
            break;
        case BLE_PRPH_EVT_ENC_OK:
            self._fail_count = 0;
            self._status     = static_cast<int>(LinkStatus::Encrypted);
            break;
        case BLE_PRPH_EVT_ENC_FAIL:
            self._fail_count = arg;
            self._status     = static_cast<int>(LinkStatus::PairFailed);
            break;
        case BLE_PRPH_EVT_IDENTITY_ROTATED:
            self._status = static_cast<int>(LinkStatus::IdentityRotated);
            break;
    }
}

bool BuddyLink::isEncrypted() const
{
    return ble_prph_is_encrypted();
}

void BuddyLink::onRx(const uint8_t* data, uint16_t len)
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (uint16_t i = 0; i < len; i++) {
        char c = static_cast<char>(data[i]);

        if (c == '\n') {
            if (_discarding) {
                _discarding = false;
                _dropped++;
            } else if (!_partial.empty()) {
                if (_lines.size() >= MAX_QUEUE) {
                    _lines.pop_front();
                    _dropped++;
                }
                _lines.push_back(std::move(_partial));
            }
            _partial.clear();
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (_discarding) {
            continue;
        }

        if (_partial.size() >= MAX_LINE_BYTES) {
            _discarding = true;
            _partial.clear();
            continue;
        }

        _partial.push_back(c);
    }
}

bool BuddyLink::takeLine(std::string& out)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lines.empty()) {
        return false;
    }
    out = std::move(_lines.front());
    _lines.pop_front();
    return true;
}

bool BuddyLink::sendLine(std::string_view json)
{
    if (!isReady()) {
        return false;
    }
    std::string line(json);
    line.push_back('\n');
    int rc = nus_svc_send(reinterpret_cast<const uint8_t*>(line.data()), static_cast<uint16_t>(line.size()));
    if (rc != 0) {
        mclog::tagWarn(TAG, "send failed rc={}", rc);
        return false;
    }
    return true;
}

void BuddyLink::dropBonds()
{
    int rc = ble_store_clear();
    mclog::tagInfo(TAG, "bonds cleared rc={}", rc);
}

}  // namespace buddy
