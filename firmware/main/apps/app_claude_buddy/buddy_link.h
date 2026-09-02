/*
 * BuddyLink — transport layer for the Claude Desktop Buddy BLE protocol.
 *
 * Wire format: UTF-8 JSON, one object per line, '\n' terminated, over the
 * Nordic UART Service. This class owns line framing (RX) and chunked
 * notifications (TX). JSON parsing/dispatch lives in AppClaudeBuddy.
 *
 * RX bytes arrive on the NimBLE host task; complete lines are queued and
 * drained from the Mooncake loop via takeLine().
 */
#pragma once
#include <string>
#include <string_view>
#include <deque>
#include <mutex>
#include <cstdint>

namespace buddy {

class BuddyLink {
public:
    static BuddyLink& instance();

    /// Start the NUS BLE peripheral. Can only be called once per boot.
    void start(std::string_view deviceName);

    /// Link-layer connected (GATT connection exists).
    bool isConnected() const;

    /// Desktop subscribed to TX notifications (ready to receive).
    bool isReady() const;

    /// Pop the next complete JSON line, if any.
    bool takeLine(std::string& out);

    /// Send one JSON object (newline appended). Returns false if not ready.
    bool sendLine(std::string_view json);

    /// Erase stored BLE bonds ({"cmd":"unpair"}).
    void dropBonds();

    /// Bytes dropped because a line exceeded the size cap (diagnostics).
    uint32_t droppedLines() const
    {
        return _dropped;
    }

private:
    BuddyLink() = default;
    static void rx_trampoline(const uint8_t* data, uint16_t len);
    void onRx(const uint8_t* data, uint16_t len);

    static constexpr size_t MAX_LINE_BYTES = 8 * 1024;  // spec: events > 4 KB are dropped upstream
    static constexpr size_t MAX_QUEUE      = 24;

    std::mutex _mutex;
    std::string _partial;
    bool _discarding = false;  // current line exceeded cap; skip until '\n'
    std::deque<std::string> _lines;
    uint32_t _dropped = 0;
    bool _started     = false;
};

}  // namespace buddy
