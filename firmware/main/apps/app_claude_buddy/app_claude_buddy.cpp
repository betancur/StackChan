/*
 * AppClaudeBuddy — Mooncake app implementation
 */
#include "app_claude_buddy.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <assets/assets.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <esp_heap_caps.h>
#include <sys/time.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>

using namespace mooncake;
using namespace stackchan;
using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace ArduinoJson;

static const std::string_view TAG = "AppClaudeBuddy";

// ── Constructor ───────────────────────────────────────────────────────────────

AppClaudeBuddy::AppClaudeBuddy()
{
    setAppInfo().name = "Buddy";

    // Reuse the AI Agent icon (custom icon needs the assets pipeline)
    static auto icon  = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;

    static uint32_t theme_color = THEME_PRIMARY;
    setAppInfo().userData       = (void*)&theme_color;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AppClaudeBuddy::onCreate()
{
    mclog::tagInfo(TAG, "onCreate");
}

void AppClaudeBuddy::onOpen()
{
    mclog::tagInfo(TAG, "onOpen");

    _store.load();

    {
        LvglLockGuard lock;

        auto avatar = std::make_unique<avatar::DefaultAvatar>();
        avatar->init(lv_screen_active());
        avatar->getPanel()->onClick().connect([this]() { _screen_clicked = true; });
        GetStackChan().attachAvatar(std::move(avatar));

        GetStackChan().addModifier(std::make_unique<BreathModifier>());
        GetStackChan().addModifier(std::make_unique<BlinkModifier>());
        GetStackChan().addModifier(std::make_unique<HeadPetModifier>());
        GetStackChan().addModifier(std::make_unique<ImuEventModifier>());  // "dizzy" on shake

        GetStackChan().avatar().setSpeech("Starting BLE...");

        createUi();
        view::create_home_indicator([this]() { close(); }, THEME_PRIMARY, THEME_DARK);
        view::create_status_bar(THEME_PRIMARY, THEME_DARK);
    }

    // Head touch: approve while a prompt is pending, otherwise just activity
    _head_touch_conn = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) {
        if (g == HeadPetGesture::Press) _head_pressed = true;
    });

    // Advertise as "Claude StackChan-XXXX" (desktop filters on the "Claude" prefix)
    std::string mac  = GetHAL().getFactoryMacString();
    std::string name = "Claude StackChan-" + (mac.size() >= 4 ? mac.substr(mac.size() - 4) : mac);
    for (auto& c : name) c = (c >= 'a' && c <= 'f') ? (c - 'a' + 'A') : c;
    buddy::BuddyLink::instance().start(name);

    _last_activity_ms = GetHAL().millis();
    setMood(Mood::Sleep, true);
}

void AppClaudeBuddy::onRunning()
{
    auto& link   = buddy::BuddyLink::instance();
    uint32_t now = GetHAL().millis();

    // ── Drain protocol lines ─────────────────────────────────────────────────
    {
        std::string line;
        int budget = 8;  // bound work per loop
        while (budget-- > 0 && link.takeLine(line)) {
            handleLine(line);
        }
    }

    // ── Link liveness → base mood ────────────────────────────────────────────
    bool linked = link.isConnected() && _has_snapshot && (now - _last_snapshot_ms) < LINK_TIMEOUT_MS;
    if (linked != _was_linked) {
        _was_linked = linked;
        if (!linked) {
            mclog::tagInfo(TAG, "bridge lost → sleep");
            _has_snapshot = false;
            _level        = -1;
            _store.nap++;
            _store.saveStats();
            _transient_active = false;
        } else {
            mclog::tagInfo(TAG, "bridge alive");
            noteActivity();
        }
    }

    // ── Input flags ──────────────────────────────────────────────────────────
    bool screen_click  = _screen_clicked.exchange(false);
    bool head_pressed  = _head_pressed.exchange(false);
    bool approve_click = _approve_clicked.exchange(false);
    bool deny_click    = _deny_clicked.exchange(false);
    bool overlay_click = _overlay_clicked.exchange(false);

    if (screen_click || head_pressed || approve_click || deny_click || overlay_click) {
        noteActivity();
    }

    if (_snap.prompt.present) {
        if (approve_click || head_pressed) decide(true);
        else if (deny_click) decide(false);
    }

    if (overlay_click) {
        LvglLockGuard lock;
        showOverlay(false);
    } else if (screen_click && !_snap.prompt.present) {
        LvglLockGuard lock;
        showOverlay(!_overlay || _overlay->hasFlag(LV_OBJ_FLAG_HIDDEN));
    }

    if (_overlay_until && now >= _overlay_until) {
        LvglLockGuard lock;
        showOverlay(false);
    }

    // ── Transient mood expiry ────────────────────────────────────────────────
    if (_transient_active && now >= _transient_until) {
        _transient_active = false;
    }

    // ── Turn text expiry ─────────────────────────────────────────────────────
    if (_turn_until && now >= _turn_until) {
        _turn_until = 0;
        _turn_text.clear();
        LvglLockGuard lock;
        refreshSpeech();
    }

    // ── Resolve effective mood ───────────────────────────────────────────────
    setMood(_transient_active ? _transient : baseMood());

    // ── Animations ───────────────────────────────────────────────────────────
    updateLeds();
    updateCelebrateWiggle();
    checkDim();

    // ── LVGL update ──────────────────────────────────────────────────────────
    {
        LvglLockGuard lock;
        GetStackChan().update();
        view::update_home_indicator();
        view::update_status_bar();
    }
}

void AppClaudeBuddy::onClose()
{
    mclog::tagInfo(TAG, "onClose");

    GetHAL().onHeadPetGesture.disconnect(_head_touch_conn);
    _head_touch_conn = -1;

    GetHAL().showRgbColor(0, 0, 0);
    if (_dimmed) {
        GetHAL().setBackLightBrightness(75);
    }

    {
        LvglLockGuard lock;
        clearTransientModifiers();
        destroyUi();
        GetStackChan().resetAvatar();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    // NimBLE cannot be re-initialised in-process: warm-reboot back to the launcher
    GetHAL().requestWarmReboot(0);
}

// ── Protocol ──────────────────────────────────────────────────────────────────

void AppClaudeBuddy::handleLine(const std::string& line)
{
    JsonDocument doc;
    auto err = deserializeJson(doc, line);
    if (err) {
        mclog::tagWarn(TAG, "bad json ({}): {}", err.c_str(), fit(line, 80));
        return;
    }
    JsonObjectConst obj = doc.as<JsonObjectConst>();
    if (obj.isNull()) {
        return;
    }

    if (obj["cmd"].is<const char*>()) {
        handleCommand(obj);
        return;
    }
    if (obj["time"].is<JsonArrayConst>()) {
        handleTimeSync(obj["time"].as<JsonArrayConst>());
        return;
    }
    if (obj["evt"].is<const char*>()) {
        handleTurn(obj);
        return;
    }
    if (obj["total"].is<int>() || obj["msg"].is<const char*>() || obj["tokens"].is<unsigned long>()) {
        handleSnapshot(obj);
        return;
    }

    mclog::tagWarn(TAG, "unhandled message: {}", fit(line, 80));
}

void AppClaudeBuddy::handleSnapshot(JsonObjectConst doc)
{
    Snapshot s;
    s.total        = doc["total"] | 0;
    s.running      = doc["running"] | 0;
    s.waiting      = doc["waiting"] | 0;
    s.msg          = doc["msg"] | "";
    s.tokens       = doc["tokens"] | 0UL;
    s.tokens_today = doc["tokens_today"] | 0UL;

    JsonArrayConst entries = doc["entries"].as<JsonArrayConst>();
    if (!entries.isNull()) {
        for (JsonVariantConst e : entries) {
            if (e.is<const char*>()) {
                s.entries.emplace_back(e.as<const char*>());
                if (s.entries.size() >= 6) break;
            }
        }
    }

    JsonObjectConst p = doc["prompt"].as<JsonObjectConst>();
    if (!p.isNull() && p["id"].is<const char*>()) {
        s.prompt.present = true;
        s.prompt.id      = p["id"].as<const char*>();
        s.prompt.tool    = p["tool"] | "";
        s.prompt.hint    = p["hint"] | "";
    }

    uint32_t now = GetHAL().millis();

    // Track when a given prompt was first shown (for the 5 s "heart" reward)
    if (s.prompt.present) {
        if (s.prompt.id != _prompt_seen_id) {
            _prompt_seen_id = s.prompt.id;
            _prompt_seen_ms = now;
            mclog::tagInfo(TAG, "approval pending: {} — {}", s.prompt.tool, s.prompt.hint);
        }
    } else {
        _prompt_seen_id.clear();
    }

    // Level up every 50K cumulative tokens (baseline on first snapshot)
    int lvl = static_cast<int>(s.tokens / TOKENS_PER_LEVEL);
    if (_level < 0) {
        _level = lvl;
    } else if (lvl > _level) {
        _level = lvl;
        mclog::tagInfo(TAG, "level up → {}", lvl);
        startTransient(Mood::Celebrate, TRANSIENT_MS);
    }

    _snap             = std::move(s);
    _has_snapshot     = true;
    _last_snapshot_ms = now;

    LvglLockGuard lock;
    refreshSpeech();
    refreshOverlay();
}

void AppClaudeBuddy::handleTurn(JsonObjectConst doc)
{
    const char* evt = doc["evt"] | "";
    if (std::string_view(evt) != "turn") {
        return;
    }
    const char* role = doc["role"] | "";
    if (std::string_view(role) != "assistant") {
        return;
    }

    JsonArrayConst content = doc["content"].as<JsonArrayConst>();
    if (content.isNull()) {
        return;
    }
    for (JsonVariantConst block : content) {
        const char* type = block["type"] | "";
        if (std::string_view(type) == "text" && block["text"].is<const char*>()) {
            _turn_text  = fit(block["text"].as<const char*>(), 56);
            _turn_until = GetHAL().millis() + TURN_SHOW_MS;
            LvglLockGuard lock;
            refreshSpeech();
            break;
        }
    }
}

void AppClaudeBuddy::handleTimeSync(JsonArrayConst arr)
{
    if (arr.size() < 1) {
        return;
    }
    long long epoch = arr[0] | 0LL;
    long offset     = arr.size() > 1 ? (arr[1] | 0L) : 0L;
    if (epoch <= 0) {
        return;
    }

    // POSIX TZ strings are "west-positive": UTC-7 → "BUD+7:00"
    long west   = -offset;
    char sign   = west < 0 ? '-' : '+';
    long absw   = std::labs(west);
    char tz[24] = {0};
    snprintf(tz, sizeof(tz), "BUD%c%ld:%02ld", sign, absw / 3600, (absw % 3600) / 60);
    setenv("TZ", tz, 1);
    tzset();

    struct timeval tv = {.tv_sec = static_cast<time_t>(epoch), .tv_usec = 0};
    settimeofday(&tv, nullptr);
    GetHAL().syncSystemTimeToRtc();

    mclog::tagInfo(TAG, "time synced: {} tz={}", epoch, tz);
}

void AppClaudeBuddy::handleCommand(JsonObjectConst doc)
{
    std::string cmd = doc["cmd"].as<const char*>();

    if (cmd == "status") {
        sendStatus();
    } else if (cmd == "name") {
        _store.name = doc["name"] | _store.name.c_str();
        _store.saveNames();
        sendAck("name", true);
    } else if (cmd == "owner") {
        _store.owner = doc["name"] | "";
        _store.saveNames();
        mclog::tagInfo(TAG, "owner: {}", _store.owner);
        sendAck("owner", true);
        if (!_store.owner.empty()) {
            _turn_text  = "Hi " + fit(_store.owner, 20) + "!";
            _turn_until = GetHAL().millis() + 4000;
            LvglLockGuard lock;
            refreshSpeech();
        }
    } else if (cmd == "unpair") {
        buddy::BuddyLink::instance().dropBonds();
        sendAck("unpair", true);
    } else if (cmd == "char_begin" || cmd == "file" || cmd == "chunk" || cmd == "file_end" || cmd == "char_end") {
        // GIF character packs target the StickC screen; Stack-Chan renders its own avatar.
        sendAck(cmd.c_str(), false, 0, "character packs not supported on Stack-Chan");
    } else {
        mclog::tagWarn(TAG, "unknown cmd: {}", cmd);
        sendAck(cmd.c_str(), false, 0, "unknown command");
    }
}

void AppClaudeBuddy::sendAck(const char* cmd, bool ok, int n, const char* error)
{
    JsonDocument doc;
    doc["ack"] = cmd;
    doc["ok"]  = ok;
    doc["n"]   = n;
    if (error) {
        doc["error"] = error;
    }
    std::string out;
    serializeJson(doc, out);
    buddy::BuddyLink::instance().sendLine(out);
}

void AppClaudeBuddy::sendStatus()
{
    JsonDocument doc;
    doc["ack"] = "status";
    doc["ok"]  = true;

    JsonObject d = doc["data"].to<JsonObject>();
    d["name"]    = _store.name;
    d["sec"]     = false;  // no bonding/encryption enforced (see sdkconfig EXAMPLE_BONDING)

    JsonObject bat = d["bat"].to<JsonObject>();
    bat["pct"]     = GetHAL().getBatteryLevel();
    bat["usb"]     = GetHAL().isBatteryCharging();

    JsonObject sys = d["sys"].to<JsonObject>();
    sys["up"]      = GetHAL().millis() / 1000;
    sys["heap"]    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    JsonObject st = d["stats"].to<JsonObject>();
    st["appr"]    = _store.appr;
    st["deny"]    = _store.deny;
    st["vel"]     = _store.vel;
    st["nap"]     = _store.nap;
    st["lvl"]     = _level < 0 ? 0 : _level;

    std::string out;
    serializeJson(doc, out);
    buddy::BuddyLink::instance().sendLine(out);
}

void AppClaudeBuddy::sendPermission(const std::string& id, bool approve)
{
    JsonDocument doc;
    doc["cmd"]      = "permission";
    doc["id"]       = id;
    doc["decision"] = approve ? "once" : "deny";
    std::string out;
    serializeJson(doc, out);
    buddy::BuddyLink::instance().sendLine(out);
}

void AppClaudeBuddy::decide(bool approve)
{
    if (!_snap.prompt.present) {
        return;
    }
    uint32_t now      = GetHAL().millis();
    uint32_t reaction = now - _prompt_seen_ms;
    mclog::tagInfo(TAG, "{} {} after {} ms", approve ? "APPROVE" : "DENY", _snap.prompt.id, reaction);

    sendPermission(_snap.prompt.id, approve);

    if (approve) {
        _store.appr++;
        if (reaction < FAST_APPROVE_MS) {
            _store.vel++;
            startTransient(Mood::Heart, TRANSIENT_MS);
        }
    } else {
        _store.deny++;
    }
    _store.saveStats();

    // Clear locally; the next snapshot is authoritative
    _snap.prompt = Prompt{};
    _prompt_seen_id.clear();
    LvglLockGuard lock;
    refreshSpeech();
}

// ── Mood / avatar ─────────────────────────────────────────────────────────────

AppClaudeBuddy::Mood AppClaudeBuddy::baseMood() const
{
    if (!_was_linked) return Mood::Sleep;
    if (_snap.prompt.present) return Mood::Attention;
    if (_snap.running > 0) return Mood::Busy;
    return Mood::Idle;
}

void AppClaudeBuddy::startTransient(Mood mood, uint32_t durationMs)
{
    _transient        = mood;
    _transient_active = true;
    _transient_until  = GetHAL().millis() + durationMs;
}

void AppClaudeBuddy::clearTransientModifiers()
{
    auto& sc = GetStackChan();
    if (_idle_motion_id >= 0) { sc.removeModifier(_idle_motion_id); _idle_motion_id = -1; }
    if (_idle_expr_id   >= 0) { sc.removeModifier(_idle_expr_id);   _idle_expr_id   = -1; }
    if (sc.hasAvatar()) {
        if (_sweat_id >= 0) { sc.avatar().removeDecorator(_sweat_id); _sweat_id = -1; }
        if (_heart_id >= 0) { sc.avatar().removeDecorator(_heart_id); _heart_id = -1; }
    }
}

void AppClaudeBuddy::setMood(Mood mood, bool force)
{
    if (!force && mood == _mood) {
        return;
    }
    mclog::tagInfo(TAG, "mood {} → {}", moodName(_mood), moodName(mood));
    _mood = mood;

    LvglLockGuard lock;
    auto& sc = GetStackChan();
    clearTransientModifiers();
    showDecisionButtons(mood == Mood::Attention);

    auto& av = sc.avatar();
    av.mouth().setWeight(0);

    switch (mood) {
        case Mood::Sleep:
            av.setEmotion(avatar::Emotion::Sleepy);
            av.mouth().setWeight(10);
            sc.motion().moveWithSpeed(0, 0, 80);
            GetHAL().showRgbColor(0, 0, 0);
            break;

        case Mood::Idle:
            av.setEmotion(avatar::Emotion::Neutral);
            _idle_motion_id = sc.addModifier(std::make_unique<IdleMotionModifier>());
            _idle_expr_id   = sc.addModifier(std::make_unique<IdleExpressionModifier>());
            GetHAL().showRgbColor(0, 0, 0);
            break;

        case Mood::Busy:
            av.setEmotion(avatar::Emotion::Neutral);
            _sweat_id       = av.addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 0, 700));
            _idle_motion_id = sc.addModifier(std::make_unique<IdleMotionModifier>(6000, 10000));
            break;  // LEDs: breathing blue in updateLeds()

        case Mood::Attention:
            av.setEmotion(avatar::Emotion::Doubt);
            sc.motion().moveWithSpeed(0, 320, 300);  // look up at the user
            break;  // LEDs: orange blink in updateLeds()

        case Mood::Celebrate:
            av.setEmotion(avatar::Emotion::Happy);
            av.mouth().setWeight(70);
            _wiggle_tick = 0;
            break;  // LEDs: rainbow, head wiggle in updateCelebrateWiggle()

        case Mood::Heart:
            av.setEmotion(avatar::Emotion::Happy);
            av.mouth().setWeight(40);
            _heart_id = av.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 0, 400));
            GetHAL().showRgbColor(70, 0, 30);
            break;
    }

    refreshSpeech();
}

void AppClaudeBuddy::refreshSpeech()
{
    // Must be called with LvglLockGuard held.
    auto& sc = GetStackChan();
    if (!sc.hasAvatar()) return;
    auto& av = sc.avatar();

    switch (_mood) {
        case Mood::Sleep:
            av.setSpeech(buddy::BuddyLink::instance().isConnected() ? "Zzz..." : "Zzz  (waiting for Claude)");
            return;
        case Mood::Attention:
            if (_snap.prompt.present) {
                std::string t = _snap.prompt.tool.empty() ? "Approve?" : ("Approve " + _snap.prompt.tool + "?");
                if (!_snap.prompt.hint.empty()) t += "\n" + fit(_snap.prompt.hint, 36);
                av.setSpeech(t);
            } else {
                av.setSpeech("Approve?");
            }
            return;
        case Mood::Celebrate: {
            char buf[32];
            snprintf(buf, sizeof(buf), "Level %d!", _level < 0 ? 0 : _level);
            av.setSpeech(buf);
            return;
        }
        case Mood::Heart:
            av.setSpeech("Thanks!");
            return;
        case Mood::Busy:
        case Mood::Idle:
            break;
    }

    if (_turn_until && !_turn_text.empty()) {
        av.setSpeech(_turn_text);
    } else if (_mood == Mood::Busy && !_snap.msg.empty()) {
        av.setSpeech(fit(_snap.msg, 40));
    } else {
        av.setSpeech("");
    }
}

static void hue_to_rgb(uint16_t h, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    h %= 360;
    uint8_t region = h / 60;
    uint8_t rem    = (h % 60) * 255 / 60;
    uint8_t q      = v - (uint32_t)v * rem / 255;
    uint8_t t      = (uint32_t)v * rem / 255;
    switch (region) {
        case 0:  r = v; g = t; b = 0; break;
        case 1:  r = q; g = v; b = 0; break;
        case 2:  r = 0; g = v; b = t; break;
        case 3:  r = 0; g = q; b = v; break;
        case 4:  r = t; g = 0; b = v; break;
        default: r = v; g = 0; b = q; break;
    }
}

void AppClaudeBuddy::updateLeds()
{
    uint32_t now = GetHAL().millis();
    if (now - _led_tick < 100) {
        return;
    }
    _led_tick = now;

    switch (_mood) {
        case Mood::Busy: {
            // slow blue breathing
            float phase = (now % 3000) / 3000.0f * 2.0f * 3.14159f;
            uint8_t v   = static_cast<uint8_t>(8 + 32 * (0.5f + 0.5f * sinf(phase)));
            GetHAL().showRgbColor(0, v / 3, v);
            break;
        }
        case Mood::Attention: {
            bool on = ((now / 400) % 2) == 0;
            if (on) GetHAL().showRgbColor(90, 40, 0);
            else    GetHAL().showRgbColor(0, 0, 0);
            break;
        }
        case Mood::Celebrate: {
            uint8_t r, g, b;
            _hue = (_hue + 36) % 360;
            hue_to_rgb(_hue, 70, r, g, b);
            GetHAL().showRgbColor(r, g, b);
            break;
        }
        default:
            break;  // static colours set in setMood()
    }
}

void AppClaudeBuddy::updateCelebrateWiggle()
{
    if (_mood != Mood::Celebrate) {
        return;
    }
    uint32_t now = GetHAL().millis();
    if (now - _wiggle_tick < 350) {
        return;
    }
    _wiggle_tick  = now;
    _wiggle_phase = !_wiggle_phase;
    LvglLockGuard lock;
    GetStackChan().motion().moveWithSpeed(_wiggle_phase ? 260 : -260, 220, 450);
}

const char* AppClaudeBuddy::moodName(Mood m)
{
    switch (m) {
        case Mood::Sleep:     return "sleep";
        case Mood::Idle:      return "idle";
        case Mood::Busy:      return "busy";
        case Mood::Attention: return "attention";
        case Mood::Celebrate: return "celebrate";
        case Mood::Heart:     return "heart";
    }
    return "?";
}

std::string AppClaudeBuddy::fit(const std::string& s, size_t max)
{
    if (s.size() <= max) return s;
    // Avoid cutting a UTF-8 sequence in half
    size_t cut = max;
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) cut--;
    return s.substr(0, cut) + "…";
}

// ── UI ────────────────────────────────────────────────────────────────────────

void AppClaudeBuddy::createUi()
{
    // Must be called with LvglLockGuard held.
    _btn_approve = std::make_unique<Button>(lv_screen_active());
    _btn_approve->setSize(120, 44);
    _btn_approve->align(LV_ALIGN_BOTTOM_LEFT, 14, -14);
    _btn_approve->setBgColor(lv_color_hex(0x2E9E5B));
    _btn_approve->setRadius(12);
    _btn_approve->label().setText("Approve");
    _btn_approve->label().setTextFont(&lv_font_montserrat_16);
    _btn_approve->label().setTextColor(lv_color_hex(0xFFFFFF));
    _btn_approve->onClick().connect([this]() { _approve_clicked = true; });
    _btn_approve->setHidden(true);

    _btn_deny = std::make_unique<Button>(lv_screen_active());
    _btn_deny->setSize(120, 44);
    _btn_deny->align(LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    _btn_deny->setBgColor(lv_color_hex(0xC0392B));
    _btn_deny->setRadius(12);
    _btn_deny->label().setText("Deny");
    _btn_deny->label().setTextFont(&lv_font_montserrat_16);
    _btn_deny->label().setTextColor(lv_color_hex(0xFFFFFF));
    _btn_deny->onClick().connect([this]() { _deny_clicked = true; });
    _btn_deny->setHidden(true);

    _overlay = std::make_unique<Container>(lv_screen_active());
    _overlay->setSize(320, 240);
    _overlay->setAlign(LV_ALIGN_CENTER);
    _overlay->setBgColor(lv_color_hex(0x101010));
    _overlay->setBgOpa(LV_OPA_90);
    _overlay->setBorderWidth(0);
    _overlay->setRadius(0);
    _overlay->setPaddingAll(10);
    _overlay->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
    _overlay->onClick().connect([this]() { _overlay_clicked = true; });

    _overlay_label = std::make_unique<Label>(_overlay->get());
    _overlay_label->setTextFont(&lv_font_montserrat_14);
    _overlay_label->setTextColor(lv_color_hex(0xEDEDED));
    _overlay_label->setWidth(300);
    _overlay_label->setLongMode(LV_LABEL_LONG_WRAP);
    _overlay_label->align(LV_ALIGN_TOP_LEFT, 0, 0);
    _overlay_label->setText("");
    _overlay->setHidden(true);
}

void AppClaudeBuddy::destroyUi()
{
    _overlay_label.reset();
    _overlay.reset();
    _btn_approve.reset();
    _btn_deny.reset();
}

void AppClaudeBuddy::showDecisionButtons(bool show)
{
    if (!_btn_approve || !_btn_deny) return;
    _btn_approve->setHidden(!show);
    _btn_deny->setHidden(!show);
    if (show) {
        _btn_approve->moveForeground();
        _btn_deny->moveForeground();
        showOverlay(false);
    }
}

void AppClaudeBuddy::showOverlay(bool show)
{
    if (!_overlay) return;
    if (show) {
        refreshOverlay();
        _overlay->setHidden(false);
        _overlay->moveForeground();
        _overlay_until = GetHAL().millis() + OVERLAY_MS;
    } else {
        _overlay->setHidden(true);
        _overlay_until = 0;
    }
}

void AppClaudeBuddy::refreshOverlay()
{
    if (!_overlay || !_overlay_label || _overlay->hasFlag(LV_OBJ_FLAG_HIDDEN)) return;

    auto& link = buddy::BuddyLink::instance();
    std::string t;
    t.reserve(512);

    t += _store.name;
    if (!_store.owner.empty()) t += "  ·  " + _store.owner;
    t += link.isReady() ? "  ·  linked\n" : (link.isConnected() ? "  ·  connecting\n" : "  ·  offline\n");

    char buf[96];
    snprintf(buf, sizeof(buf), "Sessions %d  (run %d / wait %d)\n", _snap.total, _snap.running, _snap.waiting);
    t += buf;
    snprintf(buf, sizeof(buf), "Tokens %lu  today %lu  Lv %d\n", (unsigned long)_snap.tokens,
             (unsigned long)_snap.tokens_today, _level < 0 ? 0 : _level);
    t += buf;
    snprintf(buf, sizeof(buf), "OK %d  Deny %d  Fast %d  Naps %d\n", _store.appr, _store.deny, _store.vel,
             _store.nap);
    t += buf;

    if (!_snap.msg.empty()) t += "> " + fit(_snap.msg, 44) + "\n";
    for (const auto& e : _snap.entries) {
        t += "  " + fit(e, 44) + "\n";
    }

    _overlay_label->setText(t);
}

// ── Sleep / backlight ─────────────────────────────────────────────────────────

void AppClaudeBuddy::noteActivity()
{
    _last_activity_ms = GetHAL().millis();
    if (_dimmed) {
        _dimmed = false;
        GetHAL().setBackLightBrightness(75);
    }
}

void AppClaudeBuddy::checkDim()
{
    // Dim only while asleep; an active bridge keeps the screen on
    if (_mood != Mood::Sleep) {
        if (_dimmed) noteActivity();
        return;
    }
    if (!_dimmed && GetHAL().millis() - _last_activity_ms >= DIM_AFTER_MS) {
        _dimmed = true;
        GetHAL().setBackLightBrightness(20);
    }
}
