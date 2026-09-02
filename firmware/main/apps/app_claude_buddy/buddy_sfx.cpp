/*
 * BuddySfx implementation
 */
#include "buddy_sfx.h"
#include "../app_roz_agent/roz_ogg.h"  // minimal Ogg Opus page demuxer

#include <hal/hal.h>
#include <hal/board/config.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <mooncake_log.h>

#include <esp_audio_dec_default.h>
#include <esp_audio_dec.h>
#include <esp_opus_dec.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstdlib>

namespace buddy {

static const std::string_view TAG = "BuddySfx";

// Embedded by main/CMakeLists.txt (COMMON_SOUNDS → xiaozhi-esp32/main/assets/common/*.ogg)
#define BUDDY_OGG(sym, name)                                              \
    extern "C" const char sym##_start[] asm("_binary_" name "_ogg_start"); \
    extern "C" const char sym##_end[] asm("_binary_" name "_ogg_end");

BUDDY_OGG(ogg_exclamation, "exclamation")
BUDDY_OGG(ogg_popup, "popup")
BUDDY_OGG(ogg_success, "success")
BUDDY_OGG(ogg_vibration, "vibration")

static std::string_view clip_for(Sfx sfx)
{
    switch (sfx) {
        case Sfx::Attention: return {ogg_exclamation_start, size_t(ogg_exclamation_end - ogg_exclamation_start)};
        case Sfx::Reminder:  return {ogg_exclamation_start, size_t(ogg_exclamation_end - ogg_exclamation_start)};
        case Sfx::Approve:   return {ogg_success_start, size_t(ogg_success_end - ogg_success_start)};
        case Sfx::Deny:      return {ogg_vibration_start, size_t(ogg_vibration_end - ogg_vibration_start)};
        case Sfx::LevelUp:   return {ogg_popup_start, size_t(ogg_popup_end - ogg_popup_start)};
    }
    return {};
}

static constexpr int32_t SFX_GAIN_NUM = 5;      // ×5 ≈ +14 dB before the limiter
static constexpr int32_t SFX_GAIN_DEN = 1;
static constexpr int32_t SFX_KNEE     = 24000;  // soft-limiter knee

static std::atomic<bool> s_playing{false};
static std::string_view s_clip;

static void play_task(void*)
{
    auto* audio = Board::GetInstance().GetAudioCodec();
    if (!audio) {
        s_playing = false;
        vTaskDelete(nullptr);
        return;
    }

    static bool registered = false;
    if (!registered) {
        esp_audio_dec_register_default();
        registered = true;
    }

    // Common clips are 60 ms Opus frames; decode at the codec's output rate.
    constexpr uint32_t rate       = AUDIO_OUTPUT_SAMPLE_RATE;
    constexpr size_t   max_samples = rate * 60 / 1000;

    esp_opus_dec_cfg_t opus_cfg = {};
    opus_cfg.sample_rate        = rate;
    opus_cfg.channel            = 1;
    opus_cfg.frame_duration     = ESP_OPUS_DEC_FRAME_DURATION_60_MS;

    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type                = ESP_AUDIO_TYPE_OPUS;
    dec_cfg.cfg                 = &opus_cfg;
    dec_cfg.cfg_sz              = sizeof(opus_cfg);

    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        mclog::tagError(TAG, "opus decoder open failed");
        s_playing = false;
        vTaskDelete(nullptr);
        return;
    }

    std::vector<int16_t> pcm(max_samples);

    // CoreS3 quirk: the AW88298 amp only comes up correctly when the shared
    // duplex I2S clock is already running, which the input (ES7210) channel
    // provides. Every working playback path on this board (Roz, mic test)
    // opens input first, so do the same for the short clip.
    bool input_was_enabled = audio->input_enabled();
    if (!input_was_enabled) {
        audio->EnableInput(true);
    }
    // Prime the RX DMA like the mic test does (a few reads), then close the
    // input again before opening the output: playing with the input open
    // works but comes out very quiet (shared duplex clock/slot layout), while
    // Roz's record→close→play sequence plays at full volume.
    {
        std::vector<int16_t> prime(480 * std::max(audio->input_channels(), 1));
        for (int i = 0; i < 3; i++) audio->InputData(prime);
    }
    if (!input_was_enabled) {
        audio->EnableInput(false);
    }
    audio->EnableOutput(true);

    int packets = 0, decoded = 0, errors = 0, last_rc = 0;
    size_t samples_out = 0;
    int32_t peak = 0;
    roz::OggDemux demux([&](const uint8_t* data, size_t len) {
        packets++;
        esp_audio_dec_in_raw_t  in  = {};
        esp_audio_dec_out_frame_t out = {};
        in.buffer  = const_cast<uint8_t*>(data);
        in.len     = static_cast<uint32_t>(len);
        out.buffer = reinterpret_cast<uint8_t*>(pcm.data());
        out.len    = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
        int rc = esp_audio_dec_process(dec, &in, &out);
        if (rc == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
            decoded++;
            size_t n = out.decoded_size / sizeof(int16_t);
            // Software gain: the CoreS3 amp path plays these short clips quietly
            for (size_t i = 0; i < n; i++) {
                int32_t v = static_cast<int32_t>(pcm[i]) * SFX_GAIN_NUM / SFX_GAIN_DEN;
                // Soft knee above SFX_KNEE so the boosted clip stays loud without hard clipping
                int32_t a = std::abs(v);
                if (a > SFX_KNEE) {
                    a = SFX_KNEE + (a - SFX_KNEE) / 4;
                }
                a         = std::min<int32_t>(a, 32767);
                pcm[i]    = static_cast<int16_t>(v < 0 ? -a : a);
                peak      = std::max(peak, a);
            }
            // Write in 20 ms pieces (480 samples), same granularity as Roz / mic test
            for (size_t off = 0; off < n; off += 480) {
                size_t m = std::min<size_t>(480, n - off);
                std::vector<int16_t> chunk(pcm.begin() + off, pcm.begin() + off + m);
                audio->OutputData(chunk);
            }
            samples_out += n;
        } else {
            errors++;
            last_rc = rc;
        }
    });
    demux.feed(reinterpret_cast<const uint8_t*>(s_clip.data()), s_clip.size());
    mclog::tagInfo(TAG, "clip {} B: packets={} decoded={} errors={} last_rc={} samples={} peak={} vol={}",
                   s_clip.size(), packets, decoded, errors, last_rc, samples_out, peak, audio->output_volume());

    // Let the DMA drain the tail before muting
    vTaskDelay(pdMS_TO_TICKS(120));
    audio->EnableOutput(false);
    esp_audio_dec_close(dec);

    s_playing = false;
    vTaskDelete(nullptr);
}

void play_sfx(Sfx sfx)
{
    if (s_playing.exchange(true)) {
        return;  // one at a time; drop overlapping requests
    }
    s_clip = clip_for(sfx);
    if (s_clip.empty()) {
        s_playing = false;
        return;
    }

    // Volume touches NVS: do it here (caller context), never on the PSRAM-stack task
    if (auto* audio = Board::GetInstance().GetAudioCodec()) {
        audio->SetOutputVolume(GetHAL().getSpeakerVolume());
    }

    TaskHandle_t h = nullptr;
    // Internal-RAM stack (like the mic test path); the decoder state lives on the heap
    if (xTaskCreatePinnedToCore(play_task, "buddy_sfx", 10 * 1024, nullptr, 4, &h, 0) != pdPASS) {
        mclog::tagError(TAG, "sfx task create failed");
        s_playing = false;
    }
}

bool sfx_is_playing()
{
    return s_playing;
}

}  // namespace buddy
