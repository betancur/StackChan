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
#include <string>

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
};

}  // namespace buddy
