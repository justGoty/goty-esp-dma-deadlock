#define NOMINMAX
#include <windows.h>
#include "Diagnostics/Diagnostics.h"
#include <json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
void TestEspFreshness() {
    const auto fresh = [] {
        Diagnostics::Snapshot snapshot;
        snapshot.stage = "running";
        snapshot.dmaConnected = true;
        snapshot.gameVisible = true;
        snapshot.targetPid = 1234;
        snapshot.gameCheckAgeSeconds = 1.0;
        snapshot.workerAgeSeconds = .01;
        snapshot.pawnPublishAgeSeconds = .015;
        snapshot.pawnPublishes = 1;
        return snapshot;
    };
    auto sample = fresh();
    Check(Diagnostics::HasFreshEspData(sample), "Active target with fresh worker and published pawns may render ESP");
    sample.workerAgeSeconds = sample.pawnPublishAgeSeconds = 0;
    Check(Diagnostics::HasFreshEspData(sample), "A publication sampled at age zero is fresh");
    sample.workerAgeSeconds = sample.pawnPublishAgeSeconds = .25;
    Check(Diagnostics::HasFreshEspData(sample), "The 250 ms freshness boundary is inclusive for both ages");
    const auto expired = std::nextafter(.25, std::numeric_limits<double>::infinity());
    sample = fresh(); sample.workerAgeSeconds = expired;
    Check(Diagnostics::HasFreshGameConnection(sample) && !Diagnostics::HasFreshEspData(sample),
        "A stalled worker hides ESP while preserving the independent game-connection status");
    sample = fresh(); sample.pawnPublishAgeSeconds = expired;
    Check(Diagnostics::HasFreshGameConnection(sample) && !Diagnostics::HasFreshEspData(sample),
        "Stale pawn publications hide ESP even if the worker and game probe continue");
    sample = fresh(); sample.pawnPublishes = 0; sample.pawnPublishAgeSeconds = -1;
    Check(!Diagnostics::HasFreshEspData(sample), "An unpublished pawn cache cannot render ESP");
    for (const auto invalidAge : {-1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        sample = fresh(); sample.workerAgeSeconds = invalidAge;
        Check(!Diagnostics::HasFreshEspData(sample), "Negative or nonfinite worker age is never fresh");
        sample = fresh(); sample.pawnPublishAgeSeconds = invalidAge;
        Check(!Diagnostics::HasFreshEspData(sample), "Negative or nonfinite pawn age is never fresh");
        sample = fresh(); sample.gameCheckAgeSeconds = invalidAge;
        Check(!Diagnostics::HasFreshEspData(sample), "Invalid game-check age cannot be rescued by fresh ESP data");
    }
    sample = fresh(); sample.gameCheckAgeSeconds = Diagnostics::GameCheckMaxAgeSeconds;
    Check(Diagnostics::HasFreshEspData(sample), "The game-check freshness boundary retains its own semantics");
    sample.gameCheckAgeSeconds = std::nextafter(Diagnostics::GameCheckMaxAgeSeconds,
        std::numeric_limits<double>::infinity());
    Check(!Diagnostics::HasFreshEspData(sample), "Expired game confirmation hides ESP despite fresh publications");
    sample = fresh(); sample.dmaConnected = false;
    Check(!Diagnostics::HasFreshEspData(sample), "Fresh-looking data cannot render after DMA disconnects");
    sample = fresh(); sample.gameVisible = false;
    Check(!Diagnostics::HasFreshEspData(sample), "Fresh-looking data cannot render an inactive game");
    sample = fresh(); sample.targetPid = 0;
    Check(!Diagnostics::HasFreshEspData(sample), "Fresh-looking data without a selected PID cannot render");
    for (const auto* stage : {"starting", "waiting_process", "failed", "stopped"}) {
        sample = fresh(); sample.stage = stage;
        Check(!Diagnostics::HasFreshEspData(sample), "Unattached application stages cannot render old ESP data");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected an isolated diagnostics directory");
        TestEspFreshness();
        const std::filesystem::path directory(argv[1]);
        Check(Diagnostics::Initialize(directory), "Diagnostics must initialize");
        auto initial = Diagnostics::GetSnapshot();
        Check(initial.frames == 0 && initial.frameWindowSamples == 0, "Frame intervals start empty");
        Check(initial.pawnPublishes == 0 && initial.pawnPublishAgeSeconds == -1, "No unpublished pawn freshness claim");
        Check(initial.timingLaneCount == 0, "Lane registry starts empty");
        Check(initial.activeOperation.empty() && initial.operationAgeSeconds == -1 && initial.operationSequence == 0,
            "No operation is in flight before acquisition starts");
        {
            Diagnostics::ScopedOperation operation("process_enumerate");
            Sleep(25);
            auto active = Diagnostics::GetSnapshot();
            Check(active.activeOperation == "process_enumerate" && active.operationAgeSeconds >= .02 && active.operationSequence == 1,
                "An unfinished native operation exposes its elapsed time independently of worker completion");
            {
                Diagnostics::ScopedOperation nested("nested");
                Check(Diagnostics::GetSnapshot().activeOperation == "process_enumerate", "Nested scope does not hide the outer native operation");
            }
            Diagnostics::PublishHeartbeat();
            std::ifstream activeHeartbeat(directory / (active.session + ".status.json"));
            const auto activeStatus = nlohmann::json::parse(activeHeartbeat);
            Check(activeStatus.at("active_operation") == "process_enumerate" && activeStatus.at("operation_age_s").get<double>() >= .02,
                "Heartbeat exposes a blocked operation while that operation is still active");
        }
        auto completed = Diagnostics::GetSnapshot();
        Check(completed.activeOperation.empty() && completed.operationAgeSeconds == -1 && completed.lastOperation == "process_enumerate" && completed.lastOperationMs >= 20,
            "Completed operation clears in-flight age and retains the last duration");
        try {
            Diagnostics::ScopedOperation failed("process_lookup");
            throw std::runtime_error("synthetic operation failure");
        } catch (const std::runtime_error&) {}
        Check(Diagnostics::GetSnapshot().activeOperation.empty() && Diagnostics::GetSnapshot().operationSequence == 2,
            "Exceptional operation exit cannot leave a false in-flight label");
        {
            Diagnostics::ScopedOperation invalid(std::string(64, 'x'));
            Check(Diagnostics::GetSnapshot().operationSequence == 2, "Operation names have a fixed upper bound");
        }

        Diagnostics::LaneTick("pawn_quick", 10);
        Diagnostics::LaneTick("pawn_quick", 20, false);
        Diagnostics::LaneTick("camera", 0);
        Diagnostics::LaneTick("pawn_quick", -1);
        Diagnostics::LaneTick("pawn_quick", std::numeric_limits<double>::infinity());
        Diagnostics::LaneTick("pawn_quick", std::numeric_limits<double>::quiet_NaN());
        Diagnostics::LaneTick("", 10);
        Diagnostics::LaneTick(std::string(32, 'x'), 10);
        Diagnostics::LaneTick(std::string("invalid\0name", 12), 10);
        auto lanes = Diagnostics::GetSnapshot();
        Check(lanes.timingLaneCount == 2, "Invalid samples and lane names must not register");
        Check(std::string(lanes.timingLanes[0].name.data()) == "pawn_quick", "Lane identity remains stable");
        Check(lanes.timingLanes[0].calls == 2 && lanes.timingLanes[0].failed == 1, "Failure is counted in its lane");
        Check(lanes.timingLanes[0].lastMs == 20 && lanes.timingLanes[0].maxMs == 20, "Latest and maximum include failed invocation");
        Check(lanes.timingLanes[0].averageMs == 15, "Mean includes each measured invocation once");
        Check(lanes.timingLanes[1].calls == 1 && lanes.timingLanes[1].averageMs == 0, "Zero elapsed time is a valid measurement");
        for (size_t i = 2; i < Diagnostics::MaxTimingLanes; ++i)
            Diagnostics::LaneTick("lane_" + std::to_string(i), static_cast<double>(i));
        Diagnostics::LaneTick("registry_full", 1);
        Diagnostics::LaneTick("pawn_quick", 0);
        lanes = Diagnostics::GetSnapshot();
        Check(lanes.timingLaneCount == Diagnostics::MaxTimingLanes, "Dynamic lane registration is bounded");
        Check(lanes.timingLanes[0].calls == 3 && lanes.timingLanes[0].averageMs == 10, "Existing lanes still update at capacity");

        Diagnostics::FrameTick();
        auto firstFrame = Diagnostics::GetSnapshot();
        Check(firstFrame.frames == 1 && firstFrame.frameWindowSamples == 0, "First frame cannot manufacture an interval");
        Sleep(60);
        Diagnostics::FrameTick();
        auto delayed = Diagnostics::GetSnapshot();
        Check(delayed.lastFrameMs >= 50 && delayed.slowFrames >= 1, "An observed frame delay is reported");
        for (size_t i = 0; i < Diagnostics::FrameTimingWindowSize + 10; ++i) Diagnostics::FrameTick();
        auto frames = Diagnostics::GetSnapshot();
        Check(frames.frameWindowSamples == Diagnostics::FrameTimingWindowSize, "Frame timing history remains bounded");
        Check(frames.frameWindowAverageMs >= 0 && std::isfinite(frames.frameWindowAverageMs), "Bounded window average stays finite");
        Check(frames.frameWindowAverageMs <= frames.frameWindowMaxMs + 1e-6 && frames.frameWindowMaxMs <= frames.maxFrameMs,
            "Window timing extrema and mean are internally consistent");
        Check(frames.frameAgeSeconds >= 0, "Frame freshness uses the last measured frame");

        Diagnostics::PawnPublished();
        Sleep(10);
        Diagnostics::PawnPublished();
        auto pawns = Diagnostics::GetSnapshot();
        Check(pawns.pawnPublishes == 2 && pawns.pawnPublishAgeSeconds >= 0, "Successful pawn publication owns its freshness");
        Check(pawns.maxPawnPublishGapMs >= 5, "A pawn publish gap is measured");

        Diagnostics::PublishHeartbeat();
        std::ifstream heartbeat(directory / (pawns.session + ".status.json"));
        const auto status = nlohmann::json::parse(heartbeat);
        Check(status.at("lane_timings").size() == Diagnostics::MaxTimingLanes, "Heartbeat includes bounded per-lane metrics");
        Check(status.at("lane_timings").at("pawn_quick").at("calls") == 3 &&
            status.at("lane_timings").at("pawn_quick").at("failed") == 1, "Heartbeat preserves lane success/failure evidence");
        Check(status.at("frame_window_samples") == Diagnostics::FrameTimingWindowSize && status.at("slow_frames").get<uint64_t>() >= 1,
            "Heartbeat includes frame timing evidence");
        Check(status.at("pawn_publishes") == 2 && status.at("pawn_publish_age_s").get<double>() >= 0,
            "Heartbeat includes pawn freshness evidence");
        std::ifstream events(directory / (pawns.session + ".0.jsonl"));
        size_t eventCount{};
        for (std::string line; std::getline(events, line);) if (!line.empty()) ++eventCount;
        Check(eventCount == 1, "High-frequency timing APIs must not write per-call events");
        events.close(); heartbeat.close();
        for (int i = 0; i < 4; ++i) Diagnostics::LaneTick("camera", 1000, true);
        auto slow = Diagnostics::GetSnapshot();
        Check(slow.lastError.find("slow_lane_camera") != std::string::npos && slow.lastError.find("elapsed_ms=1000.000") != std::string::npos,
            "Slow successful DMA call has bounded diagnostic detail without requiring a failed read");
        std::ifstream slowEvents(directory / (pawns.session + ".0.jsonl"));
        size_t slowEventCount{};
        for (std::string line; std::getline(slowEvents, line);) {
            if (!line.empty() && nlohmann::json::parse(line).at("event") == "slow_lane_camera") ++slowEventCount;
        }
        Check(slowEventCount == 1, "Repeated slow calls write at most one warning in five seconds");
        slowEvents.close();
        Diagnostics::Shutdown();

        Check(Diagnostics::Initialize(directory / "reset"), "A subsequent diagnostics session initializes");
        auto reset = Diagnostics::GetSnapshot();
        Check(reset.timingLaneCount == 0 && reset.frames == 0 && reset.frameWindowSamples == 0,
            "Session reset clears timing counters and registry");
        Check(reset.pawnPublishes == 0 && reset.pawnPublishAgeSeconds == -1 && reset.maxPawnPublishGapMs == 0,
            "Session reset clears publication freshness");
        Check(reset.activeOperation.empty() && reset.lastOperation.empty() && reset.operationSequence == 0 && reset.operationAgeSeconds == -1,
            "Session reset clears operation history and in-flight state");
        Diagnostics::Shutdown();
        std::cout << "PASS: " << checks << " performance diagnostics checks without DMA or GUI\n";
        return 0;
    } catch (const std::exception& exception) {
        Diagnostics::Shutdown();
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
