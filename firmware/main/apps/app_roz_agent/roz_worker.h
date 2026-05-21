/*
 * AppRozAgent — RozWorker (WebSocket streaming version)
 *
 * Architecture:
 *   - Persistent WebSocket to Roz WS server (opened after network connects)
 *   - Record task:  PCM → Opus encode → send 0x01 frames in real-time
 *   - OnData cb:    receives 0x01 audio frames → pushes to playback queue
 *   - Playback task: dequeues Opus frames → decodes → AudioCodec::OutputData
 *   - VAD:          firmware-side energy detection, sends 0x02 to server
 */
#pragma once
#include <atomic>
#include <vector>
#include <cstdint>
#include <memory>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
class WebSocket;

namespace roz {

enum class State {
    Idle,
    Recording,
    Thinking,   // server processing: STT → LLM
    Playing,    // receiving + playing TTS audio
    Error,
};

class RozWorker {
public:
    RozWorker();
    ~RozWorker();

    // Called once network is ready — opens WebSocket
    void connect();

    // Trigger recording (touch/press)
    void startRecording();

    // Stop recording early (optional — VAD stops automatically)
    void stopRecording();

    // Abort everything
    void cancel();

    State       getState()     const { return _state.load(); }
    bool        isConnected()  const { return _ws_connected; }
    std::string getLastError() const { return _last_error; }

    // Returns true (and fills out_*) when a new per-sentence text/emotion is available.
    bool takeSpeechUpdate(std::string& out_text, std::string& out_emotion);

    // Returns true (once) when the last idle was due to empty STT — caller should
    // skip auto-listen so the noise loop doesn't repeat.
    bool takeNoSpeech() { return _no_speech.exchange(false); }

private:
    // ── WebSocket ─────────────────────────────────────────────────────────────
    std::unique_ptr<WebSocket> _ws;
    std::atomic<bool>          _ws_connected {false};

    void reconnect();
    void sendFrame(uint8_t type, const void* data, size_t len);
    void sendJson(uint8_t type, const char* json);
    void onWsData(const char* data, size_t len, bool binary);

    // ── State ─────────────────────────────────────────────────────────────────
    std::atomic<State>  _state        {State::Idle};
    std::string         _last_error;
    std::atomic<bool>   _stop_rec     {false};
    std::atomic<bool>   _cancel_req   {false};

    void setState(State s) { _state.store(s); }

    // ── Record task ───────────────────────────────────────────────────────────
    TaskHandle_t _rec_task = nullptr;
    static void  recTaskEntry(void* arg);
    void         runRecTask();

    // ── Playback task + queue ─────────────────────────────────────────────────
    QueueHandle_t _play_queue = nullptr;
    TaskHandle_t  _play_task  = nullptr;
    static void   playTaskEntry(void* arg);
    void          runPlayTask();

    void startPlayTask();
    void stopPlayTask();

    std::atomic<bool> _server_done {false};  // server sent "idle" → play task can drain & exit
    std::atomic<bool> _no_speech   {false};  // last idle had reason:"no_speech"

    // ── Per-sentence speech bubble + emotion (written from WS thread, read from app thread) ──
    std::string       _speech_text;
    std::string       _speech_emotion;
    std::atomic<bool> _speech_dirty {false};

};

}  // namespace roz
