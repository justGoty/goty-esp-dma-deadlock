#include "GUI/FramePacer.h"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks{};
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
}

int main() {
    try {
        using namespace std::chrono;
        using FramePacing::Clock;
        Check(FramePacing::ClampTargetFps(-1) == 60, "Invalid negative target cannot disable the cap");
        Check(FramePacing::ClampTargetFps(0) == 60, "A legacy zero target is bounded");
        Check(FramePacing::ClampTargetFps(60) == 60, "Minimum target is preserved");
        Check(FramePacing::ClampTargetFps(144) == 144, "Configured target is preserved");
        Check(FramePacing::ClampTargetFps(240) == 240, "Maximum target is preserved");
        Check(FramePacing::ClampTargetFps(1000) == 240, "Oversized target cannot restore an unbounded loop");
        Check(FramePacing::FramePeriod(60) <= milliseconds(17), "Frame cap cannot request a long input wait");
        Check(FramePacing::FramePeriod(240) > milliseconds(4), "Upper target owns a positive frame period");

        const auto start = Clock::time_point(seconds(100));
        const auto period = FramePacing::FramePeriod(144);
        Check(FramePacing::RemainingFrameTime(start, start, 144) == period, "A fresh frame owns one full period");
        Check(FramePacing::RemainingFrameTime(start, start + milliseconds(2), 144) == period - milliseconds(2),
            "Rendering time is deducted from the wait");
        Check(FramePacing::RemainingFrameTime(start, start + period, 144) == Clock::duration::zero(),
            "An exact deadline requires no extra delay");
        Check(FramePacing::RemainingFrameTime(start, start + milliseconds(17), 144) == Clock::duration::zero(),
            "A completed 60 Hz VSync wait is not followed by another frame period");
        Check(FramePacing::RemainingFrameTime(start, start + seconds(2), 144) == Clock::duration::zero(),
            "Missed deadlines skip waiting rather than catch up");
        const auto nextStart = start + seconds(2);
        Check(FramePacing::RemainingFrameTime(nextStart, nextStart, 144) == period,
            "After a missed deadline, the next frame receives a new bounded period");
        Check(FramePacing::RemainingFrameTime(start, start - seconds(1), 144) == period,
            "Unexpected clock input cannot request an oversized wait");
        Check(FramePacing::RemainingFrameTime(start, start + milliseconds(1), 60) >
            FramePacing::RemainingFrameTime(start, start + milliseconds(1), 240),
            "A changed GUI target takes effect on the next wait");
        std::cout << "PASS: " << checks << " deterministic frame pacing checks; no DMA or GUI\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
