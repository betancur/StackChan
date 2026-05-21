/*
 * AppRozAgent — Roz Voice Server configuration
 */
#pragma once
#include <cstdint>
#include <hal/board/config.h>

namespace roz {

// ── WebSocket server ──────────────────────────────────────────────────────────
inline constexpr const char* WS_URL        = "wss://openclaw.tail842722.ts.net/voice/ws";
inline constexpr const char* API_TOKEN     = "8ACdEYNV8RPeE/0U3nP/2Edd7lnF4C4Odp7iMPVKM1c=";
inline constexpr const char* SESSION_ID    = "stackchan-001";
inline constexpr const char* ASST_NAME     = "Roz";

// ── Recording / VAD ───────────────────────────────────────────────────────────
inline constexpr uint32_t SAMPLE_RATE      = AUDIO_INPUT_SAMPLE_RATE;  // 24 000 Hz
inline constexpr float    VOICE_THRESHOLD  = 800.0f;
inline constexpr int      MIN_SPEECH_CHUNKS = 10;   // ~200 ms before we care
inline constexpr int      SILENCE_CHUNKS   = 75;    // 1.5 s silence → stop
inline constexpr int      MAX_WAIT_CHUNKS  = 300;   // 6 s max wait for speech

// ── Opus encoder (recording) ──────────────────────────────────────────────────
inline constexpr int      ENC_BITRATE      = 32000;
inline constexpr int      ENC_COMPLEXITY   = 5;

// ── Opus decoder (playback) ───────────────────────────────────────────────────
inline constexpr int      OPUS_OUT_RATE    = AUDIO_OUTPUT_SAMPLE_RATE;  // 24 000 Hz
inline constexpr int      OPUS_CHANNELS    = 1;
inline constexpr int      OPUS_MAX_FRAME   = OPUS_OUT_RATE * 20 / 1000;  // 480 samples @ 20ms

// ── WebSocket frame types ─────────────────────────────────────────────────────
inline constexpr uint8_t  WS_TYPE_AUDIO   = 0x01;
inline constexpr uint8_t  WS_TYPE_VAD     = 0x02;
inline constexpr uint8_t  WS_TYPE_STATE   = 0x04;
inline constexpr uint8_t  WS_TYPE_ERROR   = 0x05;
inline constexpr uint8_t  WS_TYPE_CONFIG  = 0x10;

// ── Playback queue ────────────────────────────────────────────────────────────
inline constexpr size_t   PLAY_QUEUE_DEPTH = 256;  // Opus packets (~5s) buffered for playback

}  // namespace roz
