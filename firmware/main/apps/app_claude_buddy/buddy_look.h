/*
 * LookTracker — finds where the person is using the camera.
 *
 * No face-detection model is available in this build, so we use motion:
 * frames are downsampled to luminance, differenced against the previous one,
 * and the horizontal centroid of the motion energy gives a target offset
 * (-100 = far left of the image, +100 = far right). The app nudges the yaw
 * servo toward it; the loop closes because a centred person produces motion
 * in the middle of the frame.
 *
 * Runs on its own task while active; the app polls target().
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace buddy {

class LookTracker {
public:
    static LookTracker& instance();

    void start();
    void stop();
    bool isRunning() const
    {
        return _task != nullptr;
    }

    /// Latest target: returns true and fills offset (-100..100) if a target was
    /// seen within maxAgeMs.
    bool target(int& offset, uint32_t maxAgeMs) const;

    /// Same as target() but consumes it: each detection drives at most one step.
    bool takeTarget(int& offset, uint32_t maxAgeMs);

    /// The head is moving: ignore frames until `ms` from now and re-baseline
    /// afterwards, so the camera's own motion is not mistaken for a person.
    void suppress(uint32_t ms);

private:
    LookTracker() = default;
    static void task_entry(void* arg);
    void run();
    bool processFrame(const uint8_t* data, size_t len, int width, int height, int format);

    static constexpr int DS         = 4;              // downsample factor
    static constexpr int MAX_W      = 320 / DS;       // 80
    static constexpr int MAX_H      = 240 / DS;       // 60
    static constexpr int BANDS      = 8;
    static constexpr uint32_t PERIOD_MS = 350;

    uint32_t _last_total        = 0;        // diagnostics
    uint8_t* _prev              = nullptr;  // MAX_W * MAX_H luminance
    uint8_t* _cur               = nullptr;
    bool     _have_prev         = false;

    TaskHandle_t _task          = nullptr;
    std::atomic<bool> _stop_req{false};
    std::atomic<int>  _offset{0};
    std::atomic<uint32_t> _seen_ms{0};
    std::atomic<uint32_t> _suppress_until{0};
};

}  // namespace buddy
