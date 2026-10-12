#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Diagnostics {
inline constexpr char Version[] = "0.1.12";
inline constexpr size_t MaxTimingLanes = 16;
inline constexpr size_t FrameTimingWindowSize = 120;
struct LaneSnapshot {
    // Fixed storage keeps the per-frame Snapshot copy allocation-free for timing lanes.
    std::array<char, 32> name{};
    uint64_t calls{}, failed{};
    double lastMs{}, maxMs{}, averageMs{};
};
struct Snapshot {
    std::string stage, lastError, session, logDirectory;
    std::string activeOperation, lastOperation;
    uint64_t operationSequence{};
    double operationAgeSeconds{-1}, lastOperationMs{};
    uint32_t targetPid{};
    bool dmaConnected{}, gameVisible{};
    uint64_t reads{}, failedReads{}, requestedBytes{}, readBytes{}, batches{}, failedBatches{};
    uint64_t boneReadRequests{};
    uint64_t openScatter{}, loops{}, slowLoops{}, ioErrors{};
    uint64_t pawns{}, controllers{}, troopers{}, camps{}, sinners{};
    double uptimeSeconds{}, stageAgeSeconds{}, workerAgeSeconds{-1}, frameAgeSeconds{-1}, maxLoopMs{}, gameCheckAgeSeconds{-1};
    uint64_t frames{}, slowFrames{}, pawnPublishes{};
    double lastFrameMs{}, maxFrameMs{}, frameWindowAverageMs{}, frameWindowMaxMs{};
    size_t frameWindowSamples{}, timingLaneCount{};
    double pawnPublishAgeSeconds{-1}, maxPawnPublishGapMs{};
    std::array<LaneSnapshot, MaxTimingLanes> timingLanes{};
};
inline constexpr double GameCheckMaxAgeSeconds = 10.0;
inline bool HasFreshGameConnection(const Snapshot& snapshot) noexcept {
    const bool attachedStage = snapshot.stage == "waiting_modules" || snapshot.stage == "resolving_offsets" || snapshot.stage == "running";
    return attachedStage && snapshot.dmaConnected && snapshot.gameVisible && snapshot.targetPid != 0 &&
        snapshot.gameCheckAgeSeconds >= 0 && snapshot.gameCheckAgeSeconds <= GameCheckMaxAgeSeconds;
}
inline bool HasFreshEspData(const Snapshot& snapshot) noexcept {
    // The worker updates the camera on every active cycle; failed camera reads
    // publish a zero matrix. Expire stalled cycles and old pawn publications
    // from the GUI clock, even when the remote process itself is still alive.
    return HasFreshGameConnection(snapshot) && snapshot.workerAgeSeconds >= 0 &&
        snapshot.workerAgeSeconds <= 0.25 && snapshot.pawnPublishAgeSeconds >= 0 &&
        snapshot.pawnPublishAgeSeconds <= 0.25;
}
extern std::atomic<uint64_t> Reads, FailedReads, RequestedBytes, ReadBytes;
extern std::atomic<uint64_t> Batches, FailedBatches, OpenScatter;
extern std::atomic<uint64_t> BoneReadRequests;
// All diagnostics APIs tolerate disk errors and must not interrupt the worker.
bool Initialize(const std::filesystem::path& directory) noexcept;
void Event(std::string_view level, std::string_view event, std::string_view detail = {}) noexcept;
void Warning(std::string_view event, std::string_view detail) noexcept;
void SetStage(std::string_view stage, uint32_t targetPid = 0) noexcept;
void SetDmaConnected(bool connected) noexcept;
void SetGameVisible(bool visible, uint32_t targetPid = 0) noexcept;
// Tracks one synchronous worker operation without per-call disk writes. Nested
// scopes do not replace the outer operation; no cancellation is implied.
class ScopedOperation {
    uint64_t sequence_{};
public:
    explicit ScopedOperation(std::string_view name) noexcept;
    ~ScopedOperation() noexcept;
    ScopedOperation(const ScopedOperation&) = delete;
    ScopedOperation& operator=(const ScopedOperation&) = delete;
};
void RecordRead(uint32_t expected, uint32_t actual, bool ok) noexcept;
void WorkerTick(double elapsedMs) noexcept;
void FrameTick() noexcept;
// Names must fit in 31 bytes; the bounded lane registry never writes per-call log events.
void LaneTick(std::string_view name, double elapsedMs, bool succeeded = true) noexcept;
void PawnPublished() noexcept;
void Entities(size_t pawns, size_t controllers, size_t troopers, size_t camps, size_t sinners) noexcept;
Snapshot GetSnapshot();
void PublishHeartbeat() noexcept;
void Shutdown() noexcept;
}
