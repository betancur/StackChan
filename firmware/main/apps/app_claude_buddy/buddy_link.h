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
#include <atomic>
#include <cstdint>

namespace buddy {

enum class LinkStatus {
    Advertising,      // nobody connected
    Connected,        // GATT connection, not yet encrypted
    Encrypted,        // bonded & encrypted
    PairFailed,       // encryption failed (see pairFailures())
    IdentityRotated,  // we changed address: the desktop must connect/pair again
};

class BuddyLink {
public:
    static BuddyLink& instance();

    LinkStatus linkStatus() const
    {
        return static_cast<LinkStatus>(_status.load());
    }
    int pairFailures() const
    {
        return _fail_count.load();
    }

    /// Start the NUS BLE peripheral. Can only be called once per boot.
    void start(std::string_view deviceName);

    /// Link-layer connected (GATT connection exists).
    bool isConnected() const;

    /// Desktop subscribed to TX notifications (ready to receive).
    bool isReady() const;

    /// Link is encrypted (bonded with passkey).
    bool isEncrypted() const;

    /// 6-digit passkey the desktop must type right now, or 0 if none pending.
    uint32_t pendingPasskey() const
    {
        return _passkey.load();
    }

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
    static void passkey_trampoline(uint32_t passkey, bool show);
    static void link_trampoline(int evt, int arg);
    void onRx(const uint8_t* data, uint16_t len);

    static constexpr size_t MAX_LINE_BYTES = 8 * 1024;  // spec: events > 4 KB are dropped upstream
    static constexpr size_t MAX_QUEUE      = 24;

    std::mutex _mutex;
    std::string _partial;
    bool _discarding = false;  // current line exceeded cap; skip until '\n'
    std::deque<std::string> _lines;
    uint32_t _dropped = 0;
    bool _started     = false;
    std::atomic<uint32_t> _passkey{0};
    std::atomic<int> _status{0};
    std::atomic<int> _fail_count{0};
};

}  // namespace buddy
