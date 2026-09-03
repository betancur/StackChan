/*
 * AppClaudeBuddy — Stack-Chan as a Claude Desktop "Hardware Buddy".
 *
 * Implements the BLE (Nordic UART) protocol from
 * https://github.com/anthropics/claude-desktop-buddy so Claude for macOS /
 * Windows (developer mode → "Open Hardware Buddy…") can drive the robot.
 *
 * Buddy states → Stack-Chan avatar:
 *   sleep      bridge not connected      → sleepy face, "Zzz", head down, LEDs off
 *   idle       connected, nothing urgent → idle motion + idle expressions
 *   busy       sessions running          → sweat drop, slow motion, blue breathing LEDs
 *   attention  approval pending          → doubt face, looks up, orange blinking LEDs,
 *                                          on-screen Approve / Deny buttons (head-pet = approve)
 *   celebrate  level up (50K tokens)     → happy face, head wiggle, rainbow LEDs
 *   dizzy      device shaken             → ImuEventModifier (dizzy eyes)
 *   heart      approved within 5 s       → hearts + pink LEDs
 *
 * Screen tap toggles an info overlay (sessions, tokens, transcript entries).
 */
#pragma once
#include "buddy_link.h"
#include "buddy_store.h"
#include "buddy_sfx.h"
#include "buddy_look.h"
#include <mooncake.h>
#include <smooth_lvgl.hpp>
#include <ArduinoJson.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class AppClaudeBuddy : public mooncake::AppAbility {
public:
    AppClaudeBuddy();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Mood { Sleep, Idle, Busy, Attention, Celebrate, Heart, Grumpy };
    enum class Risk { None, Caution, Danger };

    struct Prompt {
        bool present = false;
        std::string id;
        std::string tool;
        std::string hint;
    };

    struct Snapshot {
        int total   = 0;
        int running = 0;
        int waiting = 0;
        std::string msg;
        std::vector<std::string> entries;
        uint32_t tokens       = 0;
        uint32_t tokens_today = 0;
        Prompt prompt;
    };

    // ── Protocol ──────────────────────────────────────────────────────────────
    void handleLine(const std::string& line);
    void handleSnapshot(ArduinoJson::JsonObjectConst doc);
    void handleTurn(ArduinoJson::JsonObjectConst doc);
    void handleTimeSync(ArduinoJson::JsonArrayConst arr);
    void handleCommand(ArduinoJson::JsonObjectConst doc);
    void sendAck(const char* cmd, bool ok, int n = 0, const char* error = nullptr);
    void sendStatus();
    void sendPermission(const std::string& id, bool approve);
    void decide(bool approve);

    // ── Mood / avatar ─────────────────────────────────────────────────────────
    Mood baseMood() const;
    void setMood(Mood mood, bool force = false);
    void startTransient(Mood mood, uint32_t durationMs);
    void refreshSpeech();
    void updateLeds();
    void updateAttention();
    void setGaze(int x, int y);
    static Risk assessRisk(const std::string& tool, const std::string& hint);
    void updateCelebrateWiggle();
    void clearTransientModifiers();
    static const char* moodName(Mood m);
    static std::string fit(const std::string& s, size_t max);

    // ── UI ────────────────────────────────────────────────────────────────────
    void createUi();
    void destroyUi();
    void showDecisionButtons(bool show);
    void showOverlay(bool show);
    void refreshOverlay();
    void showPasskey(uint32_t passkey);
    void refreshChart();
    void showClock(bool show);
    void updateClock();
    static std::string compactNumber(uint32_t v);
    static uint32_t localDate();  // yyyymmdd, 0 if the clock is not set

    // ── Sleep / backlight ─────────────────────────────────────────────────────
    void noteActivity(const char* reason = "?");
    void checkDim();

    // ── State ─────────────────────────────────────────────────────────────────
    buddy::BuddyStore _store;
    Snapshot _snap;
    bool _has_snapshot        = false;
    uint32_t _last_snapshot_ms = 0;
    bool _was_linked          = false;  // last computed "bridge alive" value
    int  _last_link_status    = -1;
    int  _last_pair_failures  = 0;
    uint32_t _linked_since_ms = 0;

    Mood _mood            = Mood::Sleep;
    Mood _transient       = Mood::Idle;
    bool _transient_active = false;
    uint32_t _transient_until = 0;

    int _level            = -1;  // -1 = not yet baselined
    uint32_t _prompt_seen_ms = 0;
    std::string _prompt_seen_id;
    std::string _prompt_decided_id;  // already answered; ignore until the desktop drops it
    Risk _prompt_risk     = Risk::None;
    bool _prompt_is_question = false;  // AskUserQuestion: answered on the Mac, not approve/deny

    // Attention escalation / look-at
    uint32_t _last_reminder_ms  = 0;
    uint32_t _last_look_step_ms = 0;
    uint32_t _last_sweep_ms     = 0;
    bool     _sweep_dir         = false;

    std::string _turn_text;
    uint32_t _turn_until = 0;

    // Modifier / decorator IDs
    int _idle_motion_id = -1;
    int _idle_expr_id   = -1;
    int _sweat_id       = -1;
    int _heart_id       = -1;
    int _angry_id       = -1;
    uint32_t _wink_until = 0;  // Heart: one eye closed until this time

    // Signal connections
    int _head_touch_conn = -1;

    // Flags set from signal/LVGL context, consumed in onRunning()
    std::atomic<bool> _screen_clicked{false};
    std::atomic<bool> _head_pressed{false};
    std::atomic<bool> _head_tap{false};        // press + release within HEAD_TAP_MS
    std::atomic<uint32_t> _head_press_ms{0};
    std::atomic<bool> _approve_clicked{false};
    std::atomic<bool> _deny_clicked{false};
    std::atomic<bool> _overlay_clicked{false};

    // Timers
    uint32_t _led_tick        = 0;
    uint32_t _wiggle_tick     = 0;
    bool _wiggle_phase        = false;
    uint16_t _hue             = 0;
    uint32_t _overlay_until   = 0;
    uint32_t _last_activity_ms = 0;
    bool _dimmed              = false;

    // LVGL widgets
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_approve;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> _btn_deny;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _overlay;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _overlay_label;
    lv_obj_t* _chart                 = nullptr;
    lv_chart_series_t* _series_past  = nullptr;
    lv_chart_series_t* _series_today = nullptr;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _day_labels[7];
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _bar_values[7];
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _chart_title;
    bool     _days_dirty       = false;
    uint32_t _last_days_save_ms = 0;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _clock_panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _clock_time;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _clock_date;
    bool     _clock_shown   = false;
    uint32_t _clock_tick    = 0;
    uint32_t _idle_log_tick = 0;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> _pair_panel;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _pair_title;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> _pair_code;
    uint32_t _shown_passkey = 0;

    static constexpr uint32_t LINK_TIMEOUT_MS     = 30 * 1000;   // spec: dead if no snapshot in ~30 s
    static constexpr uint32_t TRANSIENT_MS        = 3200;
    static constexpr uint32_t GRUMPY_MS           = 2600;
    static constexpr uint32_t WINK_MS             = 750;
    static constexpr uint32_t FAST_APPROVE_MS     = 5000;
    static constexpr uint32_t HEAD_APPROVE_GRACE_MS = 1500;
    static constexpr uint32_t HEAD_TAP_MS           = 1500;  // a real tap releases quickly; drift never does
    static constexpr uint32_t TURN_SHOW_MS        = 7000;
    static constexpr uint32_t OVERLAY_MS          = 10 * 1000;
    static constexpr uint32_t DIM_AFTER_MS        = 5 * 60 * 1000;
    static constexpr uint32_t DAYS_SAVE_EVERY_MS  = 60 * 1000;
    static constexpr uint32_t CLOCK_AFTER_MS      = 3 * 60 * 1000;   // idle/sleep → clock face
    static constexpr uint32_t TOKENS_PER_LEVEL    = 50000;
    static constexpr uint32_t ESCALATE_AFTER_MS   = 60 * 1000;  // prompt unanswered → escalate
    static constexpr uint32_t REMINDER_EVERY_MS   = 30 * 1000;
    static constexpr uint32_t LOOK_STEP_MS        = 900;
    static constexpr uint32_t LOOK_SETTLE_MS      = 700;  // ignore camera while the head moves
    static constexpr uint32_t LOOK_TARGET_AGE_MS  = 2000;
    static constexpr uint32_t SWEEP_EVERY_MS      = 2500;
    static constexpr int      LOOK_YAW_SIGN       = 1;  // flip if the head turns away from the person
    static constexpr uint32_t THEME_PRIMARY       = 0xD97757;  // Claude orange
    static constexpr uint32_t THEME_DARK          = 0x3A2418;
    static constexpr uint32_t CLOCK_COLOR         = 0x5AB0FF;  // light blue clock digits
};
