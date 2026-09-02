/*
 * BuddySfx — one-shot sound effects for the Buddy app.
 *
 * Plays the Ogg Opus clips embedded by the firmware build (xiaozhi common
 * sounds) straight to the audio codec. The XiaoZhi AudioService is not running
 * in Mooncake mode, so we demux + decode ourselves, same approach as Roz.
 * Playback runs on a short-lived task; overlapping requests are dropped.
 */
#pragma once

namespace buddy {

enum class Sfx {
    Connect,    // desktop linked up
    Attention,  // approval prompt appeared
    Reminder,   // prompt still unanswered (escalation)
    Approve,
    Deny,
    LevelUp,
};

void play_sfx(Sfx sfx);
bool sfx_is_playing();

}  // namespace buddy
