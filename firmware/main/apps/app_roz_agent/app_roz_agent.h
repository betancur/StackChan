/*
 * AppRozAgent — Mooncake app for Roz Voice Server conversation.
 *
 * Triggers:
 *   • Head sensor Press   → start recording (push-to-talk)
 *   • Head sensor Release → stop recording, send to server, play response
 *   • Screen tap          → same toggle behaviour
 *
 * Avatar states mirror RozWorker::State via modifiers:
 *   Idle      → IdleMotionModifier + IdleExpressionModifier
 *   Recording → avatar shows "listening" (green LED, quiet)
 *   Sending   → IdleMotionModifier slow (waiting)
 *   Playing   → SpeakingModifier (mouth + micro-movements)
 */
#pragma once
#include <mooncake.h>
#include "roz_worker.h"
#include <memory>
#include <atomic>

class AppRozAgent : public mooncake::AppAbility {
public:
    AppRozAgent();

    void onCreate()  override;
    void onOpen()    override;
    void onRunning() override;
    void onClose()   override;

private:
    std::unique_ptr<roz::RozWorker> _worker;

    // ── Network state ──────────────────────────────────────────────────────────
    enum class NetState { Connecting, Connected, Failed };
    std::atomic<NetState> _net_state {NetState::Connecting};
    TaskHandle_t          _net_task  = nullptr;
    std::string           _net_msg;    // message to show while connecting

    // ── Modifier IDs ───────────────────────────────────────────────────────────
    int _idle_motion_id    = -1;
    int _idle_expr_id      = -1;
    int _speaking_id       = -1;

    // Signal connections (to disconnect on close)
    int _head_touch_conn   = -1;

    // Flags set from ISR/signal context, consumed in onRunning()
    std::atomic<bool> _screen_clicked   {false};
    std::atomic<bool> _head_pressed     {false};
    std::atomic<bool> _head_released    {false};
    std::atomic<bool> _net_just_connected {false};

    roz::State _last_state      = roz::State::Idle;
    bool       _in_conversation = false;   // auto-listen after each response

    // ── Stuck-state watchdog ───────────────────────────────────────────────────
    static constexpr uint32_t THINKING_TIMEOUT_MS = 30 * 1000;  // 30 s max in Thinking/Recording
    uint32_t _active_state_since_ms = 0;

    // ── Hibernation ────────────────────────────────────────────────────────────
    static constexpr uint32_t HIBERNATE_MS       = 5 * 60 * 1000;  // idle → sleep
    static constexpr uint32_t CLOSE_AFTER_SLEEP_MS = 60 * 1000;    // sleep → close app
    uint32_t _last_activity_ms  = 0;
    uint32_t _hibernate_since_ms = 0;
    bool     _is_hibernating    = false;

    void resetActivityTimer();
    void checkHibernation();
    void enterHibernation();
    void wakeFromHibernation();

    void applyStateToAvatar(roz::State state);
    void applyEmotion(const std::string& emotion);
    std::string executeTool(const roz::ToolCall& tc);
    void enterIdle();
    void enterRecording();
    void enterSending();
    void enterPlaying();
};
