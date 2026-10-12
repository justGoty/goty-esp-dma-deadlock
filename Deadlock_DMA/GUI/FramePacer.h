#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <thread>

namespace FramePacing {
using Clock = std::chrono::steady_clock;

inline constexpr int ClampTargetFps(int targetFps) noexcept {
    return std::clamp(targetFps, 60, 240);
}

inline constexpr Clock::duration FramePeriod(int targetFps) noexcept {
    return std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds(1'000'000'000 / ClampTargetFps(targetFps)));
}

inline constexpr Clock::duration RemainingFrameTime(
    Clock::time_point frameStart, Clock::time_point now, int targetFps) noexcept {
    const auto period = FramePeriod(targetFps);
    // A completed VSync wait or a slow frame consumes the entire budget. Each
    // frame owns one deadline; missed frames never cause a burst of catch-up work.
    return std::clamp(frameStart + period - now, Clock::duration::zero(), period);
}
}

class FramePacer final {
public:
    FramePacer() noexcept {
        // Windows 10 1803+ supports high-resolution one-shot waitable timers.
        // Keep one handle for the whole GUI loop and fall back on older systems.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    ~FramePacer() {
        if (timer_) {
            CancelWaitableTimer(timer_);
            CloseHandle(timer_);
        }
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    void BeginFrame() noexcept { frameStart_ = FramePacing::Clock::now(); }

    void WaitForNextFrame(int targetFps) noexcept {
        const auto now = FramePacing::Clock::now();
        const auto remaining = FramePacing::RemainingFrameTime(
            frameStart_, now, targetFps);
        if (remaining <= FramePacing::Clock::duration::zero()) return;
        const auto deadline = now + remaining;
        if (timer_) {
            const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -std::max<LONGLONG>(1, (nanoseconds + 99) / 100);
            if (SetWaitableTimer(timer_, &dueTime, 0, nullptr, nullptr, FALSE)) {
                // Even a faulty timer cannot block shutdown/input processing indefinitely.
                // At the 60 FPS lower bound, the maximum requested wait is 17 ms.
                const auto timeout = static_cast<DWORD>(std::clamp<LONGLONG>(
                    (nanoseconds + 999'999) / 1'000'000, 1, 17));
                if (WaitForSingleObject(timer_, timeout) == WAIT_OBJECT_0) return;
                CancelWaitableTimer(timer_);
            }
        }
        std::this_thread::sleep_until(deadline);
    }

private:
    HANDLE timer_{};
    FramePacing::Clock::time_point frameStart_{};
};
