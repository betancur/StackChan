/*
 * BuddyStore — NVS-backed settings and stats for the Claude Buddy app.
 *
 * Mirrors stats.h of anthropics/claude-desktop-buddy:
 *   appr / deny  — permission decisions taken from the device
 *   vel          — "velocity": approvals given within 5 s of the prompt
 *   nap          — times the buddy went to sleep (desktop disconnected)
 *   lvl          — derived from cumulative tokens (1 level per 50K), not stored
 */
#pragma once
#include <settings.h>
#include <nvs.h>
#include <string>
#include <cstring>
#include <cstdint>

namespace buddy {

struct BuddyStore {
    std::string name  = "Clawd";  // device display name ({"cmd":"name"})
    std::string owner = "";       // owner name ({"cmd":"owner"})
    int appr          = 0;
    int deny          = 0;
    int vel           = 0;
    int nap           = 0;

    void load()
    {
        Settings s("buddy", false);
        name  = s.GetString("name", "Clawd");
        owner = s.GetString("owner", "");
        appr  = s.GetInt("appr", 0);
        deny  = s.GetInt("deny", 0);
        vel   = s.GetInt("vel", 0);
        nap   = s.GetInt("nap", 0);
    }

    void saveNames()
    {
        Settings s("buddy", true);
        s.SetString("name", name);
        s.SetString("owner", owner);
    }

    void saveStats()
    {
        Settings s("buddy", true);
        s.SetInt("appr", appr);
        s.SetInt("deny", deny);
        s.SetInt("vel", vel);
        s.SetInt("nap", nap);
    }

    // ── Per-day history (last 7 days, days[0] = most recent) ─────────────────
    struct DayStat {
        uint32_t date   = 0;  // yyyymmdd local, 0 = empty
        uint32_t tokens = 0;  // desktop's tokens_today high-water mark
        uint16_t appr   = 0;
        uint16_t deny   = 0;
    };
    static constexpr int DAYS = 7;
    DayStat days[DAYS];

    void loadDays()
    {
        nvs_handle_t h;
        if (nvs_open("buddy", NVS_READONLY, &h) != ESP_OK) return;
        size_t len = sizeof(days);
        if (nvs_get_blob(h, "days", days, &len) != ESP_OK || len != sizeof(days)) {
            memset(days, 0, sizeof(days));
        }
        nvs_close(h);
    }

    void saveDays()
    {
        nvs_handle_t h;
        if (nvs_open("buddy", NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_blob(h, "days", days, sizeof(days));
        nvs_commit(h);
        nvs_close(h);
    }

    /// Record for `date`, rotating a fresh one in when the day changes.
    DayStat& today(uint32_t date)
    {
        if (days[0].date != date) {
            for (int i = DAYS - 1; i > 0; i--) days[i] = days[i - 1];
            days[0]      = DayStat{};
            days[0].date = date;
        }
        return days[0];
    }
};

}  // namespace buddy
