/*
 * AppRozAgent — Mooncake app implementation
 */
#include "app_roz_agent.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <cctype>
#include <ctime>
#include <cstdio>

using namespace mooncake;
using namespace stackchan;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string_view TAG = "AppRozAgent";

// ── Tool param helpers ────────────────────────────────────────────────────────

static std::string param_str(const std::string& j, const char* key, const char* def = "")
{
    std::string needle = std::string("\"") + key + "\":";
    auto p = j.find(needle);
    if (p == std::string::npos) return def;
    p += needle.size();
    while (p < j.size() && j[p] == ' ') p++;
    if (p >= j.size() || j[p] != '"') return def;
    p++;
    auto q = j.find('"', p);
    return (q == std::string::npos) ? std::string{def} : j.substr(p, q - p);
}

static int param_int(const std::string& j, const char* key, int def = 0)
{
    std::string needle = std::string("\"") + key + "\":";
    auto p = j.find(needle);
    if (p == std::string::npos) return def;
    p += needle.size();
    while (p < j.size() && j[p] == ' ') p++;
    if (p >= j.size()) return def;
    bool neg = (j[p] == '-');
    if (neg) p++;
    if (p >= j.size() || !isdigit((unsigned char)j[p])) return def;
    int val = 0;
    while (p < j.size() && isdigit((unsigned char)j[p])) val = val * 10 + (j[p++] - '0');
    return neg ? -val : val;
}

static bool param_bool(const std::string& j, const char* key, bool def = false)
{
    std::string needle = std::string("\"") + key + "\":";
    auto p = j.find(needle);
    if (p == std::string::npos) return def;
    p += needle.size();
    while (p < j.size() && j[p] == ' ') p++;
    if (j.substr(p, 4) == "true")  return true;
    if (j.substr(p, 5) == "false") return false;
    return def;
}

// ── Constructor ───────────────────────────────────────────────────────────────

AppRozAgent::AppRozAgent()
{
    setAppInfo().name = "ROZ";

    // Reuse the AI Agent icon; replace with a custom one if desired
    static auto icon  = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;

    static uint32_t theme_color = 0x4A90D9;   // blue
    setAppInfo().userData       = (void*)&theme_color;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AppRozAgent::onCreate()
{
    mclog::tagInfo(TAG, "onCreate");
}

void AppRozAgent::onOpen()
{
    mclog::tagInfo(TAG, "onOpen");

    // ── Avatar + UI — create immediately so home indicator is active ──────────
    {
        LvglLockGuard lock;

        auto avatar = std::make_unique<avatar::DefaultAvatar>();
        avatar->init(lv_screen_active());
        avatar->getPanel()->onClick().connect([this]() { _screen_clicked = true; });
        GetStackChan().attachAvatar(std::move(avatar));

        GetStackChan().addModifier(std::make_unique<BreathModifier>());
        GetStackChan().addModifier(std::make_unique<BlinkModifier>());
        GetStackChan().addModifier(std::make_unique<HeadPetModifier>());
        GetStackChan().addModifier(std::make_unique<ImuEventModifier>());

        // Show "connecting" in the avatar speech bubble
        GetStackChan().avatar().setSpeech("Connecting...");

        view::create_home_indicator([this]() { close(); }, 0x4A90D9, 0x1A3A5C);
        view::create_status_bar(0x4A90D9, 0x1A3A5C);
    }

    // ── Head touch ────────────────────────────────────────────────────────────
    _head_touch_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press)   _head_pressed  = true;
        if (g == HeadPetGesture::Release) _head_released = true;
    });

    // ── Connect to WiFi in background task ────────────────────────────────────
    // onOpen() returns immediately → onRunning() starts → home indicator works.
    _net_state = NetState::Connecting;
    xTaskCreate([](void* arg) {
        auto* self = static_cast<AppRozAgent*>(arg);
        GetHAL().startNetwork([self](std::string_view msg) {
            self->_net_msg = std::string(msg);
            LvglLockGuard lock;
            if (GetStackChan().hasAvatar()) {
                GetStackChan().avatar().setSpeech(self->_net_msg.c_str());
            }
        });
        self->_net_just_connected = true;
        self->_net_state = NetState::Connected;
        vTaskDelete(nullptr);
    }, "roz_net", 8192, this, 3, &_net_task);
}

void AppRozAgent::onRunning()
{
    // ── Network connect event ─────────────────────────────────────────────────
    if (_net_just_connected.exchange(false)) {
        _worker = std::make_unique<roz::RozWorker>();
        _worker->connect();   // open persistent WebSocket
        LvglLockGuard lock;
        GetStackChan().avatar().setSpeech("");
        resetActivityTimer();
        enterIdle();
    }

    // ── Any touch wakes from hibernation ─────────────────────────────────────
    bool head_pressed  = _head_pressed.exchange(false);
    bool head_released = _head_released.exchange(false);
    bool screen_click  = _screen_clicked.exchange(false);

    if ((head_pressed || screen_click) && _is_hibernating) {
        wakeFromHibernation();
        head_pressed = false;
        screen_click = false;
    }

    // ── Conversation triggers (only when worker is ready and awake) ───────────
    if (_worker && !_is_hibernating) {
        roz::State current = _worker->getState();

        // Touch while Idle or Error: start/restart conversation
        if ((head_pressed || screen_click) &&
            (current == roz::State::Idle || current == roz::State::Error)) {
            if (current == roz::State::Error) {
                // Recover from error — reset conversation state
                _in_conversation = false;
            }
            if (_in_conversation) {
                _in_conversation = false;
                mclog::tagInfo(TAG, "Conversation ended by user");
            } else {
                _in_conversation = true;
                _worker->startRecording();
            }
            head_pressed = false;
            screen_click = false;
        }

        // Head release stops current recording turn
        if (head_released && current == roz::State::Recording) _worker->stopRecording();

        // Touch during active turn → cancel + end conversation
        if (screen_click && (current == roz::State::Recording ||
                              current == roz::State::Thinking  ||
                              current == roz::State::Playing)) {
            _in_conversation = false;
            _worker->cancel();
        }

        // Auto-listen: only after the robot actually spoke (Playing→Idle).
        // If the server returned nothing (Thinking→Idle, empty STT/response),
        // don't auto-listen — the echo/noise loop would keep firing endlessly.
        bool just_went_idle = (current == roz::State::Idle &&
                               _last_state != roz::State::Idle);
        bool robot_just_spoke = (current == roz::State::Idle &&
                                 _last_state == roz::State::Playing);
        if (robot_just_spoke && _in_conversation && _worker->isConnected()) {
            if (_worker->takeNoSpeech()) {
                // Server didn't catch speech — stay in conversation but wait for
                // an explicit touch so the noise→loop cycle doesn't repeat.
                mclog::tagInfo(TAG, "No speech — waiting for touch to continue");
            } else {
                // Normal turn end: robot spoke, auto-listen for next turn
                GetHAL().delay(1200);
                _worker->startRecording();
            }
        }

        // Reset activity timer on any interaction or turn completion
        if (head_pressed || screen_click || just_went_idle) {
            resetActivityTimer();
        }

        if (current != _last_state) {
            mclog::tagInfo(TAG, "state → {}", static_cast<int>(current));
            applyStateToAvatar(current);
            _last_state = current;
        }

        // Per-sentence speech bubble + emotion update (fires every sentence, same state)
        {
            std::string sp_text, sp_emotion;
            if (_worker->takeSpeechUpdate(sp_text, sp_emotion)) {
                LvglLockGuard lock;
                if (!sp_text.empty())
                    GetStackChan().avatar().setSpeech(sp_text.c_str());
                applyEmotion(sp_emotion);
            }
        }
        // ── Tool call dispatch ────────────────────────────────────────────────
        {
            roz::ToolCall tc;
            while (_worker->takeToolCall(tc)) {
                std::string result = executeTool(tc);
                _worker->sendToolResult(tc.id, result);
            }
        }

    } else if (!_worker) {
        _head_pressed.store(false);
        _head_released.store(false);
        _screen_clicked.store(false);
    }

    // ── Stuck-state watchdog ─────────────────────────────────────────────────
    // If the worker stays in Thinking or Recording for more than 30 s (server
    // timeout, dropped WS frame, etc.) force-cancel so the user can interact again.
    if (_worker && !_is_hibernating) {
        roz::State ws = _worker->getState();
        if (ws == roz::State::Thinking || ws == roz::State::Recording) {
            if (_active_state_since_ms == 0)
                _active_state_since_ms = GetHAL().millis();
            if (GetHAL().millis() - _active_state_since_ms > THINKING_TIMEOUT_MS) {
                mclog::tagInfo(TAG, "Watchdog: stuck in active state — cancelling");
                _in_conversation = false;
                _worker->cancel();
                _active_state_since_ms = 0;
            }
        } else {
            _active_state_since_ms = 0;
        }
    }

    // ── Hibernation check (only when idle and connected) ─────────────────────
    if (_worker && !_is_hibernating) {
        checkHibernation();
    }

    // ── Close app after 1 minute in hibernation without interaction ───────────
    if (_is_hibernating &&
        GetHAL().millis() - _hibernate_since_ms >= CLOSE_AFTER_SLEEP_MS) {
        mclog::tagInfo(TAG, "sleep timeout — closing app");
        close();
        return;
    }

    // ── LVGL update ───────────────────────────────────────────────────────────
    {
        LvglLockGuard lock;
        GetStackChan().update();
        view::update_home_indicator();
        view::update_status_bar();
    }
}

void AppRozAgent::onClose()
{
    mclog::tagInfo(TAG, "onClose");

    if (_worker) {
        _worker->cancel();
        _worker.reset();
    }

    GetHAL().onHeadPetGesture.disconnect(_head_touch_conn);
    _head_touch_conn = -1;

    // Kill the network background task if it's still running
    if (_net_task && _net_state.load() == NetState::Connecting) {
        vTaskDelete(_net_task);
        _net_task = nullptr;
    }

    {
        LvglLockGuard lock;
        GetStackChan().resetAvatar();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    // Warm-reboot back to launcher, landing on this app's icon (index 1: Buddy is 0)
    GetHAL().requestWarmReboot(1);
}

// ── Avatar state transitions ──────────────────────────────────────────────────

void AppRozAgent::applyStateToAvatar(roz::State state)
{
    LvglLockGuard lock;

    switch (state) {
        case roz::State::Idle:      enterIdle();      break;
        case roz::State::Recording: enterRecording(); break;
        case roz::State::Thinking:  enterSending();   break;  // same UI as "Sending"
        case roz::State::Playing:   enterPlaying();   break;
        case roz::State::Error: {
            // Flash sad expression briefly, then return to idle
            auto& sc = GetStackChan();
            sc.addModifier(std::make_unique<TimedEmotionModifier>(avatar::Emotion::Sad, 3000));
            sc.addModifier(std::make_unique<SpeakingModifier>(2000, 180, false));
            GetHAL().showRgbColor(50, 0, 0);   // red LEDs
            break;
        }
    }
}

void AppRozAgent::enterIdle()
{
    auto& sc = GetStackChan();

    // Remove transient modifiers
    if (_speaking_id >= 0)    { sc.removeModifier(_speaking_id);    _speaking_id    = -1; }
    if (_idle_motion_id >= 0) { sc.removeModifier(_idle_motion_id); _idle_motion_id = -1; }
    if (_idle_expr_id   >= 0) { sc.removeModifier(_idle_expr_id);   _idle_expr_id   = -1; }

    sc.avatar().setSpeech("");
    sc.avatar().setEmotion(avatar::Emotion::Neutral);
    sc.avatar().mouth().setWeight(0);

    // Natural idle behaviour
    _idle_motion_id = sc.addModifier(std::make_unique<IdleMotionModifier>());    // 4-8 s interval
    _idle_expr_id   = sc.addModifier(std::make_unique<IdleExpressionModifier>());

    GetHAL().showRgbColor(0, 0, 0);
}

void AppRozAgent::enterRecording()
{
    auto& sc = GetStackChan();

    // Stop idle movement so the robot stays "attentive"
    if (_idle_motion_id >= 0) { sc.removeModifier(_idle_motion_id); _idle_motion_id = -1; }
    if (_idle_expr_id   >= 0) { sc.removeModifier(_idle_expr_id);   _idle_expr_id   = -1; }
    if (_speaking_id    >= 0) { sc.removeModifier(_speaking_id);    _speaking_id    = -1; }

    sc.avatar().setEmotion(avatar::Emotion::Neutral);
    sc.avatar().mouth().setWeight(0);
    sc.avatar().setSpeech("...");

    // Center-forward "listening" pose
    sc.motion().moveWithSpeed(0, 200, 250);

    GetHAL().showRgbColor(0, 50, 0);   // green LEDs
}

void AppRozAgent::enterSending()
{
    auto& sc = GetStackChan();

    sc.avatar().setSpeech("Thinking...");

    // Slow idle movement while waiting for server
    if (_idle_motion_id < 0) {
        _idle_motion_id = sc.addModifier(std::make_unique<IdleMotionModifier>(6000, 10000));
    }

    GetHAL().showRgbColor(0, 0, 50);   // blue LEDs
}

void AppRozAgent::enterPlaying()
{
    auto& sc = GetStackChan();

    // Stop slow idle, start speaking animation
    if (_idle_motion_id >= 0) { sc.removeModifier(_idle_motion_id); _idle_motion_id = -1; }

    sc.avatar().setSpeech("");
    // Emotion is controlled per-sentence by the server via takeSpeechUpdate().

    if (_speaking_id < 0) {
        // SpeakingModifier: mouth animation + subtle head movement
        _speaking_id = sc.addModifier(std::make_unique<SpeakingModifier>(0, 180, true));
    }

    GetHAL().showRgbColor(0, 20, 50);  // cyan-ish LEDs
}

// ── Emotion ───────────────────────────────────────────────────────────────────

void AppRozAgent::applyEmotion(const std::string& e)
{
    // Must be called with LvglLockGuard already held.
    using E = avatar::Emotion;
    E em = E::Neutral;
    if      (e == "happy")  em = E::Happy;
    else if (e == "sad")    em = E::Sad;
    else if (e == "angry")  em = E::Angry;
    else if (e == "doubt")  em = E::Doubt;
    else if (e == "sleepy") em = E::Sleepy;
    GetStackChan().avatar().setEmotion(em);
}

// ── Hibernation ───────────────────────────────────────────────────────────────

void AppRozAgent::resetActivityTimer()
{
    _last_activity_ms = GetHAL().millis();
}

void AppRozAgent::checkHibernation()
{
    if (_worker && _worker->getState() != roz::State::Idle) {
        resetActivityTimer();   // any active state resets the clock
        return;
    }
    if (GetHAL().millis() - _last_activity_ms >= HIBERNATE_MS) {
        enterHibernation();
    }
}

void AppRozAgent::enterHibernation()
{
    mclog::tagInfo(TAG, "entering hibernation");
    _is_hibernating      = true;
    _hibernate_since_ms  = GetHAL().millis();

    LvglLockGuard lock;
    auto& sc = GetStackChan();

    // Remove all motion/expression modifiers
    if (_speaking_id    >= 0) { sc.removeModifier(_speaking_id);    _speaking_id    = -1; }
    if (_idle_motion_id >= 0) { sc.removeModifier(_idle_motion_id); _idle_motion_id = -1; }
    if (_idle_expr_id   >= 0) { sc.removeModifier(_idle_expr_id);   _idle_expr_id   = -1; }

    // Sleepy face + Zzz text (same as XiaoZhi)
    sc.avatar().setEmotion(avatar::Emotion::Sleepy);
    sc.avatar().setSpeech("Zzz...");
    sc.avatar().mouth().setWeight(10);

    // Center-down pose — relax head
    sc.motion().moveWithSpeed(0, 0, 80);

    // LEDs off
    GetHAL().showRgbColor(0, 0, 0);

    // Dim backlight
    GetHAL().setBackLightBrightness(20);
}

void AppRozAgent::wakeFromHibernation()
{
    mclog::tagInfo(TAG, "waking from hibernation");
    _is_hibernating = false;
    resetActivityTimer();

    LvglLockGuard lock;
    GetStackChan().avatar().setSpeech("");
    GetHAL().setBackLightBrightness(GetHAL().getBackLightBrightness() == 20
                                     ? 75 : GetHAL().getBackLightBrightness());
    enterIdle();
}

// ── Tool execution ────────────────────────────────────────────────────────────

std::string AppRozAgent::executeTool(const roz::ToolCall& tc)
{
    const auto& tool   = tc.tool;
    const auto& params = tc.params;
    mclog::tagInfo(TAG, "executeTool: {} {}", tool, params);

    if (tool == "set_emotion") {
        std::string emotion = param_str(params, "emotion", "neutral");
        {
            LvglLockGuard lock;
            applyEmotion(emotion);
        }
        return "{\"ok\":true}";
    }

    if (tool == "move_head") {
        int pan   = param_int(params, "pan",  0);
        int tilt  = param_int(params, "tilt", 0);
        pan  = std::max(-1000, std::min(1000, pan));
        tilt = std::max(-1000, std::min(1000, tilt));
        {
            LvglLockGuard lock;
            GetStackChan().motion().moveWithSpeed(pan, tilt, 300);
        }
        return "{\"ok\":true}";
    }

    if (tool == "set_led") {
        uint8_t r = (uint8_t)std::max(0, std::min(255, param_int(params, "r", 0)));
        uint8_t g = (uint8_t)std::max(0, std::min(255, param_int(params, "g", 0)));
        uint8_t b = (uint8_t)std::max(0, std::min(255, param_int(params, "b", 0)));
        GetHAL().showRgbColor(r, g, b);
        GetHAL().refreshRgb();
        return "{\"ok\":true}";
    }

    if (tool == "set_volume") {
        int level = std::max(0, std::min(100, param_int(params, "level", 50)));
        GetHAL().setSpeakerVolume((uint8_t)level);
        return "{\"ok\":true}";
    }

    if (tool == "get_volume") {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"level\":%d}", (int)GetHAL().getSpeakerVolume());
        return buf;
    }

    if (tool == "get_battery") {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"level\":%d,\"charging\":%s}",
                 (int)GetHAL().getBatteryLevel(),
                 GetHAL().isBatteryCharging() ? "true" : "false");
        return buf;
    }

    if (tool == "get_time") {
        time_t now = time(nullptr);
        struct tm* t = localtime(&now);
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"unix\":%lld,\"iso\":\"%04d-%02d-%02dT%02d:%02d:%02d\"}",
                 (long long)now,
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        return buf;
    }

    if (tool == "set_laser") {
        bool on = param_bool(params, "on", false);
        GetHAL().setLaserEnabled(on);
        return "{\"ok\":true}";
    }

    if (tool == "set_brightness") {
        int level = std::max(0, std::min(100, param_int(params, "level", 75)));
        GetHAL().setBackLightBrightness((uint8_t)level);
        return "{\"ok\":true}";
    }

    mclog::tagWarn(TAG, "Unknown tool: {}", tool);
    return "{\"error\":\"unknown_tool\"}";
}
