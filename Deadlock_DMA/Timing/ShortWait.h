#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <thread>

// A reusable, process-local timer for short worker waits. Sleep(1) can otherwise
// consume an entire scheduler tick (~16 ms), even on a lightly loaded machine.
// No timer-resolution setting or busy wait is needed.
class ShortWait final {
public:
    using Clock = std::chrono::steady_clock;

    ShortWait() noexcept {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    ~ShortWait() {
        if (timer_) {
            CancelWaitableTimer(timer_);
            CloseHandle(timer_);
        }
    }
    ShortWait(const ShortWait&) = delete;
    ShortWait& operator=(const ShortWait&) = delete;

    void WaitFor(Clock::duration duration) noexcept {
        // This helper only services short waits. Bound all paths so a failed
        // native timer cannot turn a worker pause into an infinite wait.
        const auto wait = std::clamp(duration, Clock::duration::zero(),
            std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(50)));
        if (wait == Clock::duration::zero()) return;
        const auto deadline = Clock::now() + wait;
        if (timer_) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count();
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, (ns + 99) / 100);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                // Allow the timer to signal at its deadline before the coarse
                // timeout. The timeout is a fail-safe, not the desired cadence.
                const auto timeout = static_cast<DWORD>((ns + 999'999) / 1'000'000 + 2);
                if (WaitForSingleObject(timer_, timeout) == WAIT_OBJECT_0) return;
                CancelWaitableTimer(timer_);
            }
        }
        // Use the original deadline: a timed-out native wait must not be
        // followed by another full wait. Older Windows may have coarser timing.
        if (Clock::now() < deadline) std::this_thread::sleep_until(deadline);
    }

private:
    HANDLE timer_{};
};
