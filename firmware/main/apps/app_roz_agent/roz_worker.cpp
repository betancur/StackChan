/*
 * AppRozAgent — RozWorker WebSocket streaming implementation
 */
#include "roz_worker.h"
#include "roz_config.h"
#include "roz_ogg_writer.h"   // OGG writer — builds valid OGG for STT upload

#include <hal/hal.h>
#include <hal/board/config.h>
#include <board.h>
#include <audio/audio_codec.h>
#include <web_socket.h>        // 78__esp-ml307 WebSocket
#include <esp_heap_caps.h>
#include <esp_log.h>

// Opus encoder (recording → frames sent to server)
#include <esp_opus_enc.h>

// Opus decoder (frames received from server → playback)
#include <esp_audio_dec_default.h>
#include <esp_audio_dec.h>
#include <esp_opus_dec.h>

#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace roz;

static const char* TAG = "RozWorker";

// Extracts a string value from a minimal JSON object without a full parser.
static std::string json_str(const std::string& json, const char* key)
{
    std::string needle = std::string("\"") + key + "\":";
    auto p = json.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    while (p < json.size() && json[p] == ' ') p++;  // skip optional space
    if (p >= json.size() || json[p] != '"') return {};
    p++;  // skip opening quote
    auto q = json.find('"', p);
    return (q == std::string::npos) ? std::string{} : json.substr(p, q - p);
}

bool RozWorker::takeSpeechUpdate(std::string& out_text, std::string& out_emotion)
{
    if (!_speech_dirty.load(std::memory_order_acquire)) return false;
    out_text    = _speech_text;
    out_emotion = _speech_emotion;
    _speech_dirty.store(false, std::memory_order_relaxed);
    ESP_LOGI(TAG, "SpeechUpdate: emotion='%s' text='%.40s'", out_emotion.c_str(), out_text.c_str());
    return true;
}

// ── Frame packing helper ──────────────────────────────────────────────────────
// Protocol: [1 byte type][4 bytes length big-endian][payload]

void RozWorker::sendFrame(uint8_t type, const void* data, size_t len)
{
    if (!_ws_connected || !_ws) return;

    std::vector<uint8_t> frame(5 + len);
    frame[0] = type;
    frame[1] = (len >> 24) & 0xFF;
    frame[2] = (len >> 16) & 0xFF;
    frame[3] = (len >> 8)  & 0xFF;
    frame[4] = (len >> 0)  & 0xFF;
    if (len > 0 && data) memcpy(frame.data() + 5, data, len);

    _ws->Send(frame.data(), frame.size(), /*binary=*/true);
}

void RozWorker::sendJson(uint8_t type, const char* json)
{
    sendFrame(type, json, strlen(json));
}

// ── WebSocket data handler ────────────────────────────────────────────────────

void RozWorker::onWsData(const char* data, size_t len, bool binary)
{
    if (!binary || len < 5) return;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint8_t type     = p[0];
    uint32_t payload_len = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16)
                         | ((uint32_t)p[3] << 8)  | (uint32_t)p[4];

    if (len < 5 + payload_len) {
        ESP_LOGW(TAG, "Incomplete frame: got %u need %u", (unsigned)len, 5 + payload_len);
        return;
    }

    const uint8_t* payload = p + 5;

    if (type == WS_TYPE_AUDIO) {
        // Each 0x01 frame is one raw Opus packet — push directly to play queue
        auto* pkt = static_cast<std::vector<uint8_t>*>(
            heap_caps_malloc(sizeof(std::vector<uint8_t>), MALLOC_CAP_DEFAULT));
        if (pkt) {
            new (pkt) std::vector<uint8_t>(payload, payload + payload_len);
            if (xQueueSend(_play_queue, &pkt, 0) != pdTRUE) {
                pkt->~vector();
                heap_caps_free(pkt);
            }
        }

    } else if (type == WS_TYPE_STATE) {
        std::string json(reinterpret_cast<const char*>(payload), payload_len);
        ESP_LOGI(TAG, "Server state: %s", json.c_str());

        if (json.find("\"thinking\"") != std::string::npos) {
            setState(State::Thinking);
        } else if (json.find("\"speaking\"") != std::string::npos) {
            _speech_text    = json_str(json, "text");
            _speech_emotion = json_str(json, "emotion");
            ESP_LOGI(TAG, "Speaking rcv: emotion='%s' text='%.40s'", _speech_emotion.c_str(), _speech_text.c_str());
            _speech_dirty.store(true, std::memory_order_release);

            setState(State::Playing);
            startPlayTask();
        } else if (json.find("\"idle\"") != std::string::npos) {
            if (json.find("\"no_speech\"") != std::string::npos) {
                _no_speech = true;   // tell app thread: skip auto-listen this cycle
            }
            _server_done = true;
            if (!_play_task) setState(State::Idle);
        }

    } else if (type == WS_TYPE_ERROR) {
        _last_error = std::string(reinterpret_cast<const char*>(payload), payload_len);
        ESP_LOGE(TAG, "Server error: %s", _last_error.c_str());
        setState(State::Error);
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

RozWorker::RozWorker()
{
    _play_queue = xQueueCreate(PLAY_QUEUE_DEPTH, sizeof(std::vector<uint8_t>*));
    esp_audio_dec_register_default();
}

RozWorker::~RozWorker()
{
    cancel();
    stopPlayTask();

    if (_ws) {
        _ws->Close();
        _ws.reset();
    }

    if (_play_queue) {
        std::vector<uint8_t>* pkt = nullptr;
        while (xQueueReceive(_play_queue, &pkt, 0) == pdTRUE) {
            if (pkt) { pkt->~vector(); heap_caps_free(pkt); }
        }
        vQueueDelete(_play_queue);
    }

}

void RozWorker::connect()
{
    if (_ws_connected) return;

    auto& board  = Board::GetInstance();
    auto  net    = board.GetNetwork();
    if (!net) { ESP_LOGE(TAG, "No network"); return; }

    _ws = net->CreateWebSocket(2);  // connect_id=2 (1 is used by avatar WS)
    if (!_ws) { ESP_LOGE(TAG, "CreateWebSocket failed"); return; }

    static std::string auth_val = std::string("Bearer ") + API_TOKEN;
    _ws->SetHeader("Authorization", auth_val.c_str());
    _ws->SetReceiveBufferSize(4096);

    _ws->OnConnected([this]() {
        ESP_LOGI(TAG, "WS connected");
        _ws_connected = true;

        // Send session config
        char cfg[256];
        snprintf(cfg, sizeof(cfg),
                 "{\"session_id\":\"%s\",\"assistant\":\"%s\"}", SESSION_ID, ASST_NAME);
        sendJson(WS_TYPE_CONFIG, cfg);
    });

    _ws->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WS disconnected");
        _ws_connected = false;
        if (_state.load() != State::Idle && _state.load() != State::Error) {
            setState(State::Error);
        }
    });

    _ws->OnData([this](const char* data, size_t len, bool binary) {
        onWsData(data, len, binary);
    });

    ESP_LOGI(TAG, "Connecting to %s", WS_URL);
    if (!_ws->Connect(WS_URL)) {
        ESP_LOGE(TAG, "WS connect failed");
    }
}

void RozWorker::cancel()
{
    _cancel_req = true;
    _stop_rec   = true;
    // Drain queue so the play task (if running) exits on its next check
    if (_play_queue) {
        std::vector<uint8_t>* pkt = nullptr;
        while (xQueueReceive(_play_queue, &pkt, 0) == pdTRUE) {
            if (pkt) { pkt->~vector(); heap_caps_free(pkt); }
        }
    }
    // Force Idle immediately so the UI unblocks regardless of task state
    setState(State::Idle);
}

// ── Recording ─────────────────────────────────────────────────────────────────

void RozWorker::startRecording()
{
    auto s = _state.load();
    if ((s != State::Idle && s != State::Error) || !_ws_connected) return;

    _stop_rec    = false;
    _cancel_req  = false;
    _server_done = false;
    _last_error.clear();
    setState(State::Recording);

    sendJson(WS_TYPE_VAD, "{\"speaking\":true}");

    xTaskCreatePinnedToCoreWithCaps(
        recTaskEntry, "roz_rec",
        32 * 1024, this, 4, &_rec_task, 0,
        MALLOC_CAP_SPIRAM
    );
}

void RozWorker::stopRecording()
{
    if (_state.load() == State::Recording) _stop_rec = true;
}

void RozWorker::recTaskEntry(void* arg) { static_cast<RozWorker*>(arg)->runRecTask(); vTaskDelete(nullptr); }

void RozWorker::runRecTask()
{
    auto& board = Board::GetInstance();
    auto  audio = board.GetAudioCodec();
    if (!audio) { _last_error = "No audio codec"; setState(State::Error); _rec_task = nullptr; return; }

    // Open Opus encoder
    esp_opus_enc_config_t enc_cfg = {};
    enc_cfg.sample_rate     = SAMPLE_RATE;
    enc_cfg.channel         = 1;
    enc_cfg.bits_per_sample = 16;
    enc_cfg.bitrate         = ENC_BITRATE;
    enc_cfg.frame_duration  = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    enc_cfg.complexity      = ENC_COMPLEXITY;

    void* enc = nullptr;
    if (esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &enc) != ESP_AUDIO_ERR_OK || !enc) {
        _last_error = "Opus encoder failed";
        setState(State::Error);
        _rec_task = nullptr;
        return;
    }

    int in_size = 0, out_size = 0;
    esp_opus_enc_get_frame_size(enc, &in_size, &out_size);
    const size_t frame_samples = in_size / sizeof(int16_t);
    const uint32_t clock_scale = 48000 / SAMPLE_RATE;

    const int input_ch = std::max(audio->input_channels(), 1);
    std::vector<int16_t> raw(frame_samples * input_ch);
    std::vector<int16_t> mono(frame_samples);
    std::vector<uint8_t> pkt(out_size);

    audio->EnableInput(true);

    int      speech_chunks  = 0;
    int      silence_chunks = 0;
    int      wait_chunks    = 0;
    bool     speech_started = false;
    uint64_t granule        = 312;  // pre-skip
    const uint64_t gran_per_frame = frame_samples * clock_scale;

    // Build valid OGG/Opus — Whisper needs proper container, not raw frames
    OggWriter ogg;
    ogg.writeOpusHeaders(SAMPLE_RATE, /*channels=*/1, /*pre_skip=*/312);

    while (!_stop_rec && !_cancel_req) {
        if (!audio->InputData(raw)) break;

        for (size_t i = 0; i < frame_samples; i++) mono[i] = raw[i * input_ch];

        float sum_sq = 0;
        for (auto s : mono) { float f = s; sum_sq += f * f; }
        float rms = sqrtf(sum_sq / frame_samples);
        bool  is_voice = (rms > VOICE_THRESHOLD);

        if (is_voice) {
            speech_started = true;
            speech_chunks++;
            silence_chunks = 0;
        } else if (speech_started) {
            silence_chunks++;
        } else {
            if (++wait_chunks > MAX_WAIT_CHUNKS) {
                ESP_LOGW(TAG, "No speech detected");
                _last_error = "No speech detected";
                break;
            }
        }

        bool vad_end = speech_started && speech_chunks >= MIN_SPEECH_CHUNKS
                                      && silence_chunks >= SILENCE_CHUNKS;
        if (vad_end) {
            ESP_LOGD(TAG, "VAD end (speech=%d silence=%d)", speech_chunks, silence_chunks);
        }

        if (speech_started) {
            esp_audio_enc_in_frame_t  in_f  = {};
            esp_audio_enc_out_frame_t out_f = {};
            in_f.buffer  = (uint8_t*)mono.data();
            in_f.len     = (uint32_t)in_size;
            out_f.buffer = pkt.data();
            out_f.len    = (uint32_t)out_size;

            if (esp_opus_enc_process(enc, &in_f, &out_f) == ESP_AUDIO_ERR_OK
                    && out_f.encoded_bytes > 0) {
                granule += gran_per_frame;
                ogg.writeAudioPage(pkt.data(), out_f.encoded_bytes, granule, vad_end);
            }
        }

        if (vad_end) break;
    }

    audio->EnableInput(false);
    esp_opus_enc_close(enc);

    // Send complete OGG as one frame — server writes to .ogg, Whisper reads it
    if (!_cancel_req && speech_chunks >= MIN_SPEECH_CHUNKS) {
        ESP_LOGI(TAG, "Sending OGG: %u bytes (%d speech frames)", (unsigned)ogg.size(), speech_chunks);
        sendFrame(WS_TYPE_AUDIO, ogg.data(), ogg.size());
        sendJson(WS_TYPE_VAD, "{\"speaking\":false}");
        setState(State::Thinking);
    } else {
        setState(State::Idle);
    }

    _rec_task = nullptr;
}

// ── Playback ──────────────────────────────────────────────────────────────────

void RozWorker::startPlayTask()
{
    if (_play_task) return;
    // This runs in WiFi callback context (internal stack) — NVS access is safe here.
    // Pre-set the codec volume so the play task (which might have PSRAM stack) never
    // needs to touch NVS. AudioCodec::SetOutputVolume writes NVS + caches output_volume_;
    // then EnableOutput will use that cached value.
    auto audio = Board::GetInstance().GetAudioCodec();
    if (audio) audio->SetOutputVolume(GetHAL().getSpeakerVolume());

    // Reset before task creation so "idle" arriving after "speaking" but before task
    // start can't leave _server_done stuck at true for this session.
    _server_done = false;

    // Volume pre-set above → play task never touches NVS → PSRAM stack is safe
    xTaskCreatePinnedToCoreWithCaps(
        playTaskEntry, "roz_play",
        16 * 1024, this, 4, &_play_task, 0,
        MALLOC_CAP_SPIRAM
    );
}

void RozWorker::stopPlayTask()
{
    // Task self-terminates when queue is empty and state is not Playing
    // We just drain the queue here to unblock it
    if (_play_queue) {
        std::vector<uint8_t>* pkt = nullptr;
        while (xQueueReceive(_play_queue, &pkt, 0) == pdTRUE) {
            if (pkt) { pkt->~vector(); heap_caps_free(pkt); }
        }
    }
}

void RozWorker::playTaskEntry(void* arg) { static_cast<RozWorker*>(arg)->runPlayTask(); vTaskDelete(nullptr); }

void RozWorker::runPlayTask()
{
    auto& board = Board::GetInstance();
    auto  audio = board.GetAudioCodec();
    if (!audio) { _play_task = nullptr; return; }

    // Open Opus decoder
    esp_opus_dec_cfg_t opus_cfg = {};
    opus_cfg.sample_rate    = (uint32_t)OPUS_OUT_RATE;
    opus_cfg.channel        = (uint8_t)OPUS_CHANNELS;
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;  // edge-tts/ffmpeg uses 20ms frames

    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type   = ESP_AUDIO_TYPE_OPUS;
    dec_cfg.cfg    = &opus_cfg;
    dec_cfg.cfg_sz = sizeof(opus_cfg);

    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "Opus decoder open failed");
        _play_task = nullptr;
        return;
    }

    audio->EnableOutput(true);  // uses output_volume_ pre-set in startPlayTask()
    ESP_LOGD(TAG, "Playback at volume: %d", audio->output_volume());

    const size_t pcm_buf_bytes = OPUS_MAX_FRAME * sizeof(int16_t);
    std::vector<int16_t> pcm(OPUS_MAX_FRAME);

    while (true) {
        std::vector<uint8_t>* pkt = nullptr;
        // Short wait — 80ms balances inter-sentence gap detection vs. CPU usage
        if (xQueueReceive(_play_queue, &pkt, pdMS_TO_TICKS(80)) != pdTRUE) {
            // Exit when server signalled done AND queue is empty
            if (_server_done && uxQueueMessagesWaiting(_play_queue) == 0) break;
            // Also exit if something cancelled us
            if (_cancel_req) break;
            continue;
        }
        if (!pkt) break;

        esp_audio_dec_in_raw_t  raw_in  = {};
        esp_audio_dec_out_frame_t out_f = {};
        raw_in.buffer = pkt->data();
        raw_in.len    = (uint32_t)pkt->size();
        out_f.buffer  = (uint8_t*)pcm.data();
        out_f.len     = (uint32_t)pcm_buf_bytes;

        if (esp_audio_dec_process(dec, &raw_in, &out_f) == ESP_AUDIO_ERR_OK
                && out_f.decoded_size > 0) {
            size_t samples = out_f.decoded_size / sizeof(int16_t);
            std::vector<int16_t> out(pcm.begin(), pcm.begin() + samples);
            audio->OutputData(out);
        }

        pkt->~vector();
        heap_caps_free(pkt);

        // Check if server signalled idle and queue is now empty
        if (_state.load() != State::Playing) {
            if (uxQueueMessagesWaiting(_play_queue) == 0) break;
        }
    }

    audio->EnableOutput(false);
    esp_audio_dec_close(dec);
    setState(State::Idle);
    _play_task = nullptr;
    ESP_LOGI(TAG, "Playback done");
}
