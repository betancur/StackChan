/*
 * LookTracker implementation
 */
#include "buddy_look.h"

#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <hal/board/stackchan_camera.h>
#include <mooncake_log.h>
#include <esp_heap_caps.h>
#include <linux/videodev2.h>
#include <cstdlib>
#include <cstring>

namespace buddy {

static const std::string_view TAG = "BuddyLook";

LookTracker& LookTracker::instance()
{
    static LookTracker t;
    return t;
}

void LookTracker::start()
{
    if (_task) {
        return;
    }
    if (!_prev) {
        _prev = static_cast<uint8_t*>(heap_caps_malloc(MAX_W * MAX_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        _cur  = static_cast<uint8_t*>(heap_caps_malloc(MAX_W * MAX_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!_prev || !_cur) {
        mclog::tagError(TAG, "alloc failed");
        return;
    }
    _have_prev = false;
    _seen_ms   = 0;
    _stop_req  = false;
    if (xTaskCreatePinnedToCore(task_entry, "buddy_look", 8 * 1024, this, 2, &_task, 1) != pdPASS) {
        _task = nullptr;
        mclog::tagError(TAG, "task create failed");
    }
}

void LookTracker::stop()
{
    if (!_task) {
        return;
    }
    _stop_req = true;
    while (_task) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void LookTracker::suppress(uint32_t ms)
{
    _suppress_until = GetHAL().millis() + ms;
}

bool LookTracker::target(int& offset, uint32_t maxAgeMs) const
{
    uint32_t seen = _seen_ms.load();
    if (seen == 0 || GetHAL().millis() - seen > maxAgeMs) {
        return false;
    }
    offset = _offset.load();
    return true;
}

bool LookTracker::takeTarget(int& offset, uint32_t maxAgeMs)
{
    if (!target(offset, maxAgeMs)) {
        return false;
    }
    _seen_ms = 0;
    return true;
}

void LookTracker::task_entry(void* arg)
{
    auto* self = static_cast<LookTracker*>(arg);
    self->run();
    self->_task = nullptr;
    vTaskDelete(nullptr);
}

void LookTracker::run()
{
    auto* camera = hal_bridge::board_get_camera();
    if (!camera) {
        mclog::tagWarn(TAG, "no camera");
        return;
    }
    mclog::tagInfo(TAG, "tracking started");

    uint32_t frames = 0, hits = 0, fails = 0, last_log = GetHAL().millis();
    while (!_stop_req) {
        uint32_t t0 = GetHAL().millis();
        if (_suppress_until != 0 && static_cast<int32_t>(t0 - _suppress_until) < 0) {
            _have_prev = false;  // re-baseline on the first frame after the move
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        if (camera->StreamCaptures()) {
            frames++;
            if (processFrame(camera->GetFrameData(), camera->GetFrameSize(), camera->GetFrameWidth(),
                             camera->GetFrameHeight(), camera->GetFrameFormat())) {
                hits++;
            }
        } else {
            fails++;
        }
        if (GetHAL().millis() - last_log >= 3000) {
            last_log = GetHAL().millis();
            mclog::tagInfo(TAG, "frames={} motion_hits={} capture_fails={} fmt=0x{:x} {}x{} last_total={} offset={}",
                           frames, hits, fails, (unsigned)camera->GetFrameFormat(), camera->GetFrameWidth(),
                           camera->GetFrameHeight(), _last_total, _offset.load());
        }
        uint32_t spent = GetHAL().millis() - t0;
        vTaskDelay(pdMS_TO_TICKS(spent < PERIOD_MS ? PERIOD_MS - spent : 10));
    }
    mclog::tagInfo(TAG, "tracking stopped");
}

bool LookTracker::processFrame(const uint8_t* data, size_t len, int width, int height, int format)
{
    if (!data || width <= 0 || height <= 0) {
        return false;
    }
    int w = width / DS, h = height / DS;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;

    // Extract downsampled luminance
    for (int y = 0; y < h; y++) {
        int sy = y * DS;
        for (int x = 0; x < w; x++) {
            int sx = x * DS;
            uint8_t lum = 0;
            switch (format) {
                case V4L2_PIX_FMT_YUYV: {
                    size_t i = (static_cast<size_t>(sy) * width + sx) * 2;
                    if (i < len) lum = data[i];
                    break;
                }
                case V4L2_PIX_FMT_GREY: {
                    size_t i = static_cast<size_t>(sy) * width + sx;
                    if (i < len) lum = data[i];
                    break;
                }
                case V4L2_PIX_FMT_RGB565: {
                    size_t i = (static_cast<size_t>(sy) * width + sx) * 2;
                    if (i + 1 < len) {
                        uint16_t v = data[i] | (data[i + 1] << 8);
                        lum        = ((v >> 5) & 0x3f) << 2;  // green channel ≈ luma
                    }
                    break;
                }
                case V4L2_PIX_FMT_RGB24: {
                    size_t i = (static_cast<size_t>(sy) * width + sx) * 3;
                    if (i + 2 < len) lum = (data[i] + 2 * data[i + 1] + data[i + 2]) >> 2;
                    break;
                }
                default:
                    return false;
            }
            _cur[y * MAX_W + x] = lum;
        }
    }

    if (!_have_prev) {
        memcpy(_prev, _cur, MAX_W * MAX_H);
        _have_prev = true;
        return false;
    }

    // Motion energy per vertical band
    uint32_t band_energy[BANDS] = {0};
    uint32_t total              = 0;
    int band_w                  = (w + BANDS - 1) / BANDS;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int d = std::abs(int(_cur[y * MAX_W + x]) - int(_prev[y * MAX_W + x]));
            if (d > 18) {  // per-pixel noise gate
                band_energy[x / band_w] += d;
                total += d;
            }
        }
    }
    memcpy(_prev, _cur, MAX_W * MAX_H);

    _last_total = total;

    // Require a meaningful amount of motion (≈ a person moving, not sensor noise)
    const uint32_t threshold = static_cast<uint32_t>(w) * h * 2;
    if (total < threshold) {
        return false;
    }

    // Weighted centroid → -100..100
    uint64_t acc = 0;
    for (int b = 0; b < BANDS; b++) {
        acc += static_cast<uint64_t>(band_energy[b]) * (b * 200 / (BANDS - 1));
    }
    int offset = static_cast<int>(acc / total) - 100;

    _offset  = offset;
    _seen_ms = GetHAL().millis();
    return true;
}

}  // namespace buddy
