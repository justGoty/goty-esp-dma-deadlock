#define NOMINMAX
#include <windows.h>
#include "Diagnostics.h"
#include <json.hpp>
#include <chrono>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Diagnostics {
std::atomic<uint64_t> Reads{}, FailedReads{}, RequestedBytes{}, ReadBytes{};
std::atomic<uint64_t> Batches{}, FailedBatches{}, OpenScatter{};
std::atomic<uint64_t> BoneReadRequests{};
namespace {
using Clock = std::chrono::steady_clock;
std::mutex mutex;
Snapshot state;
auto started = Clock::now();
auto stageStarted = started;
Clock::time_point worker{}, frame{}, gameChecked{}, pawnPublished{}, operationStarted{};
std::array<double, FrameTimingWindowSize> frameIntervals{};
size_t nextFrameInterval{};
double frameIntervalSum{};
bool connectingDelayReported{};
std::unordered_map<std::string, Clock::time_point> lastWarning;
std::ofstream log;
std::filesystem::path logDirectory;
uint64_t logBytes{};
unsigned segment{};
HANDLE crashFile = INVALID_HANDLE_VALUE;
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter{};
constexpr uint64_t MaxLogBytes = 4 * 1024 * 1024;
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception) {
    // A separate pre-opened handle avoids logger locks, JSON and allocation during a native crash.
    char buffer[192]{};
    auto record = exception ? exception->ExceptionRecord : nullptr;
    int length = std::snprintf(buffer, sizeof(buffer), "local_pid=%lu thread_id=%lu exception=0x%08lX address=%p\r\n",
        GetCurrentProcessId(), GetCurrentThreadId(), record ? record->ExceptionCode : 0,
        record ? record->ExceptionAddress : nullptr);
    DWORD written{};
    if (crashFile != INVALID_HANDLE_VALUE && length > 0) {
        WriteFile(crashFile, buffer, DWORD(length), &written, nullptr); FlushFileBuffers(crashFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
std::string Timestamp() {
    SYSTEMTIME time{}; GetSystemTime(&time);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}
void OpenLog() {
    log.open(logDirectory / (state.session + "." + std::to_string(segment) + ".jsonl"),
        std::ios::binary | std::ios::trunc);
    logBytes = 0;
}
void Write(std::string_view level, std::string_view event, std::string_view detail) {
    if (level == "error" || level == "warning") state.lastError = std::string(event) + ": " + std::string(detail);
    nlohmann::json record{{"utc", Timestamp()}, {"session", state.session}, {"level", level},
        {"event", event}, {"detail", detail.substr(0, 512 * 1024)}, {"stage", state.stage}, {"target_pid", state.targetPid},
        {"thread_id", GetCurrentThreadId()}};
    auto line = record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
    if (logBytes + line.size() > MaxLogBytes) {
        log.close(); segment = (segment + 1) % 3; OpenLog();
    }
    if (log.is_open()) { log << line; log.flush(); logBytes += line.size(); }
    if (!log) ++state.ioErrors;
    if (level == "error" || level == "warning")
        std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", int(level.size()), level.data(), int(event.size()), event.data(), int(detail.size()), detail.data());
}
}
bool Initialize(const std::filesystem::path& directory) noexcept {
    try {
        std::lock_guard lock(mutex);
        logDirectory = directory;
        std::filesystem::create_directories(directory);
        state = {}; state.stage = "starting"; started = Clock::now(); stageStarted = started;
        worker = {}; frame = {}; gameChecked = {}; pawnPublished = {}; operationStarted = {}; connectingDelayReported = false;
        frameIntervals.fill(0); nextFrameInterval = 0; frameIntervalSum = 0;
        lastWarning.clear(); segment = 0;
        auto stamp = Timestamp(); std::replace(stamp.begin(), stamp.end(), ':', '-');
        state.session = "goty-" + stamp + "-" + std::to_string(GetCurrentProcessId());
        state.logDirectory = directory.string();
        OpenLog();
        crashFile = CreateFileW((directory / (state.session + ".crash.txt")).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        previousFilter = SetUnhandledExceptionFilter(CrashFilter);
        Write("info", "session_start", Version);
        return bool(log);
    } catch (...) { std::fprintf(stderr, "Diagnostics initialization failed\n"); return false; }
}
void Event(std::string_view level, std::string_view event, std::string_view detail) noexcept {
    try { std::lock_guard lock(mutex); Write(level, event, detail); } catch (...) {}
}
void Warning(std::string_view event, std::string_view detail) noexcept {
    try {
        std::lock_guard lock(mutex);
        state.lastError = std::string(event) + ": " + std::string(detail);
        auto now = Clock::now(); auto key = std::string(event);
        auto it = lastWarning.find(key);
        if (it != lastWarning.end() && now - it->second < std::chrono::seconds(5)) return;
        if (lastWarning.size() >= 64) lastWarning.clear();
        lastWarning[key] = now;
        Write("warning", event, detail);
    } catch (...) {}
}
void SetStage(std::string_view stage, uint32_t targetPid) noexcept {
    try {
        std::lock_guard lock(mutex);
        if (state.stage == stage && state.targetPid == targetPid) return;
        state.stage = stage; state.targetPid = targetPid; stageStarted = Clock::now();
        if (stage == "starting" || stage == "connecting_dma" || stage == "waiting_process" || stage == "failed" || stage == "stopped") {
            state.gameVisible = false; gameChecked = {};
        }
        connectingDelayReported = false;
        Write("info", "stage_changed", stage);
    } catch (...) {}
}
void SetDmaConnected(bool connected) noexcept {
    try {
        std::lock_guard lock(mutex);
        state.dmaConnected = connected;
        if (!connected) { state.gameVisible = false; gameChecked = {}; }
    } catch (...) {}
}
void SetGameVisible(bool visible, uint32_t targetPid) noexcept {
    try {
        std::lock_guard lock(mutex);
        visible = visible && targetPid != 0 && state.dmaConnected;
        const bool changed = state.gameVisible != visible || (visible && state.targetPid != targetPid);
        state.gameVisible = visible; gameChecked = Clock::now();
        if (targetPid) state.targetPid = targetPid;
        if (changed) Write("info", "game_visibility_changed", visible ? "Active exact-name target process confirmed" : "Target process no longer visible");
    } catch (...) {}
}
ScopedOperation::ScopedOperation(std::string_view name) noexcept {
    if (name.empty() || name.size() > 63 || name.find('\0') != std::string_view::npos) return;
    try {
        std::lock_guard lock(mutex);
        if (!state.activeOperation.empty()) return;
        state.activeOperation = name;
        sequence_ = ++state.operationSequence;
        operationStarted = Clock::now();
    } catch (...) {}
}
ScopedOperation::~ScopedOperation() noexcept {
    if (!sequence_) return;
    try {
        std::lock_guard lock(mutex);
        if (state.operationSequence != sequence_ || state.activeOperation.empty()) return;
        state.lastOperation = state.activeOperation;
        state.lastOperationMs = std::chrono::duration<double, std::milli>(Clock::now() - operationStarted).count();
        state.activeOperation.clear();
        operationStarted = {};
    } catch (...) {}
}
void RecordRead(uint32_t expected, uint32_t actual, bool ok) noexcept {
    ++Reads; RequestedBytes += expected; ReadBytes += actual;
    if (!ok || expected != actual) ++FailedReads;
}
void WorkerTick(double elapsedMs) noexcept {
    try { std::lock_guard lock(mutex); worker = Clock::now(); ++state.loops;
        if (elapsedMs > 50) ++state.slowLoops;
        state.maxLoopMs = std::max(state.maxLoopMs, elapsedMs);
    } catch (...) {}
}
void FrameTick() noexcept {
    try {
        std::lock_guard lock(mutex);
        const auto now = Clock::now();
        ++state.frames;
        if (frame != Clock::time_point{}) {
            const auto elapsedMs = std::chrono::duration<double, std::milli>(now - frame).count();
            state.lastFrameMs = elapsedMs;
            state.maxFrameMs = std::max(state.maxFrameMs, elapsedMs);
            if (elapsedMs > 50) ++state.slowFrames;
            frameIntervalSum -= frameIntervals[nextFrameInterval];
            frameIntervals[nextFrameInterval] = elapsedMs;
            frameIntervalSum += elapsedMs;
            nextFrameInterval = (nextFrameInterval + 1) % FrameTimingWindowSize;
            state.frameWindowSamples = std::min(state.frameWindowSamples + 1, FrameTimingWindowSize);
        }
        frame = now;
    } catch (...) {}
}
void LaneTick(std::string_view name, double elapsedMs, bool succeeded) noexcept {
    // Reject invalid measurements rather than poisoning the averages or heartbeat JSON.
    if (name.empty() || name.size() >= LaneSnapshot{}.name.size() || name.find('\0') != std::string_view::npos ||
        !std::isfinite(elapsedMs) || elapsedMs < 0) return;
    try {
        std::lock_guard lock(mutex);
        size_t index{};
        for (; index < state.timingLaneCount; ++index) {
            if (name == state.timingLanes[index].name.data()) break;
        }
        if (index == state.timingLaneCount) {
            if (state.timingLaneCount == MaxTimingLanes) return;
            std::memcpy(state.timingLanes[index].name.data(), name.data(), name.size());
            ++state.timingLaneCount;
        }
        auto& lane = state.timingLanes[index];
        ++lane.calls;
        if (!succeeded) ++lane.failed;
        lane.lastMs = elapsedMs;
        lane.maxMs = std::max(lane.maxMs, elapsedMs);
        lane.averageMs += (elapsedMs - lane.averageMs) / static_cast<double>(lane.calls);
    } catch (...) {}
    // Log only unusually slow lanes. Warning applies a five-second throttle per
    // event; release the metrics lock before entering the logger.
    if (elapsedMs > 50) {
        try {
            Warning(std::format("slow_lane_{}", name), std::format(
                "lane={}; elapsed_ms={:.3f}; succeeded={}", name, elapsedMs, succeeded));
        } catch (...) {}
    }
}
void PawnPublished() noexcept {
    try {
        std::lock_guard lock(mutex);
        const auto now = Clock::now();
        if (pawnPublished != Clock::time_point{})
            state.maxPawnPublishGapMs = std::max(state.maxPawnPublishGapMs,
                std::chrono::duration<double, std::milli>(now - pawnPublished).count());
        pawnPublished = now;
        ++state.pawnPublishes;
    } catch (...) {}
}
void Entities(size_t pawns, size_t controllers, size_t troopers, size_t camps, size_t sinners) noexcept {
    try { std::lock_guard lock(mutex); state.pawns = pawns; state.controllers = controllers;
        state.troopers = troopers; state.camps = camps; state.sinners = sinners; } catch (...) {}
}
Snapshot GetSnapshot() {
    std::lock_guard lock(mutex); auto result = state; auto now = Clock::now();
    result.uptimeSeconds = std::chrono::duration<double>(now - started).count();
    result.stageAgeSeconds = std::chrono::duration<double>(now - stageStarted).count();
    result.workerAgeSeconds = worker == Clock::time_point{} ? -1 : std::chrono::duration<double>(now - worker).count();
    result.frameAgeSeconds = frame == Clock::time_point{} ? -1 : std::chrono::duration<double>(now - frame).count();
    result.gameCheckAgeSeconds = gameChecked == Clock::time_point{} ? -1 : std::chrono::duration<double>(now - gameChecked).count();
    result.pawnPublishAgeSeconds = pawnPublished == Clock::time_point{} ? -1 : std::chrono::duration<double>(now - pawnPublished).count();
    result.operationAgeSeconds = operationStarted == Clock::time_point{} ? -1 : std::chrono::duration<double>(now - operationStarted).count();
    if (result.frameWindowSamples) {
        result.frameWindowAverageMs = frameIntervalSum / static_cast<double>(result.frameWindowSamples);
        result.frameWindowMaxMs = *std::max_element(frameIntervals.begin(), frameIntervals.end());
    }
    result.gameVisible = HasFreshGameConnection(result);
    result.reads = Reads; result.failedReads = FailedReads; result.requestedBytes = RequestedBytes; result.readBytes = ReadBytes;
    result.batches = Batches; result.failedBatches = FailedBatches; result.openScatter = OpenScatter;
    result.boneReadRequests = BoneReadRequests;
    return result;
}
void PublishHeartbeat() noexcept {
    try {
        auto s = GetSnapshot();
        bool reportDelay{};
        {
            std::lock_guard lock(mutex);
            if (state.stage == "connecting_dma" && Clock::now() - stageStarted > std::chrono::seconds(30) && !connectingDelayReported) {
                connectingDelayReported = true; reportDelay = true;
            }
        }
        if (reportDelay) Warning("dma_connecting_delay", "MemProcFS initialization has not returned after 30 seconds; check this session's .memprocfs.log. This is an observation, not a timeout or cancellation.");
        nlohmann::json record{{"schema_version", 1}, {"version", Version}, {"utc", Timestamp()}, {"session", s.session},
            {"local_pid", GetCurrentProcessId()}, {"stage", s.stage}, {"target_pid", s.targetPid}, {"uptime_s", s.uptimeSeconds},
            {"stage_age_s", s.stageAgeSeconds}, {"dma_connected", s.dmaConnected}, {"game_visible", s.gameVisible}, {"game_check_age_s", s.gameCheckAgeSeconds},
            {"active_operation", s.activeOperation}, {"operation_age_s", s.operationAgeSeconds}, {"operation_sequence", s.operationSequence},
            {"last_operation", s.lastOperation}, {"last_operation_ms", s.lastOperationMs},
            {"worker_age_s", s.workerAgeSeconds}, {"frame_age_s", s.frameAgeSeconds}, {"loops", s.loops},
            {"slow_loops", s.slowLoops}, {"max_loop_ms", s.maxLoopMs}, {"reads", s.reads}, {"failed_reads", s.failedReads},
            {"frames", s.frames}, {"slow_frames", s.slowFrames}, {"last_frame_ms", s.lastFrameMs}, {"max_frame_ms", s.maxFrameMs},
            {"frame_window_samples", s.frameWindowSamples}, {"frame_window_average_ms", s.frameWindowAverageMs}, {"frame_window_max_ms", s.frameWindowMaxMs},
            {"pawn_publishes", s.pawnPublishes}, {"pawn_publish_age_s", s.pawnPublishAgeSeconds}, {"max_pawn_publish_gap_ms", s.maxPawnPublishGapMs},
            {"requested_bytes", s.requestedBytes}, {"read_bytes", s.readBytes}, {"batches", s.batches},
            {"failed_batches", s.failedBatches}, {"open_scatter", s.openScatter}, {"log_io_errors", s.ioErrors},
            {"bone_read_requests", s.boneReadRequests},
            {"pawns", s.pawns}, {"controllers", s.controllers}, {"troopers", s.troopers}, {"camps", s.camps},
            {"sinners", s.sinners}, {"last_error", s.lastError}};
        record["lane_timings"] = nlohmann::json::object();
        for (size_t i = 0; i < s.timingLaneCount; ++i) {
            const auto& lane = s.timingLanes[i];
            record["lane_timings"][lane.name.data()] = {{"calls", lane.calls}, {"failed", lane.failed},
                {"last_ms", lane.lastMs}, {"max_ms", lane.maxMs}, {"average_ms", lane.averageMs}};
        }
        auto path = logDirectory / (s.session + ".status.json"); auto temp = path; temp += ".tmp";
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output << record.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); output.flush();
        bool ok = bool(output); output.close();
        if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            Warning("heartbeat_io_failed", "Could not publish status JSON");
    } catch (...) { Warning("heartbeat_io_failed", "Exception publishing status JSON"); }
}
void Shutdown() noexcept {
    Event("info", "session_end"); PublishHeartbeat();
    try { std::lock_guard lock(mutex); log.close(); SetUnhandledExceptionFilter(previousFilter);
        if (crashFile != INVALID_HANDLE_VALUE) { CloseHandle(crashFile); crashFile = INVALID_HANDLE_VALUE; }
    } catch (...) {}
}
}
