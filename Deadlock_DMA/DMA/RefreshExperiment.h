#pragma once
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include "vmmdll.h"
#include "Diagnostics/Diagnostics.h"

// Opt-in, handle-local A/B/A diagnostic. Only process refresh intervals are
// changed; data/TLB caches, paging, acquisition flags and drivers stay untouched.
// Call Tick before all worker visibility/continue gates so restoration is not
// skipped when the target disappears. Forced termination cannot execute RAII;
// a newly initialized VMM handle has its own default settings.
class RefreshExperiment {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFunction = TimePoint(*)();
    enum class Phase { Disabled, Settling, Baseline, Held, Restored, Done, Aborted, Failed };

private:
    struct Options {
        ULONG64 enabled{}, tick{}, memory{}, tlb{}, partial{}, total{};
    };
    VMM_HANDLE handle_{};
    NowFunction now_{};
    Phase phase_{Phase::Disabled};
    TimePoint created_{}, phaseStarted_{}, nextAttempt_{};
    Options original_{};
    DWORD heldTicks_{};
    bool partialTouched_{}, totalTouched_{};
    Diagnostics::Snapshot phaseCounters_{};
    static constexpr auto PhaseDuration = std::chrono::seconds(60);
    static constexpr auto SettleTimeout = std::chrono::seconds(15);
    static constexpr ULONG64 HoldPeriodMs = 3600000;

    static std::string_view Name(Phase phase) noexcept {
        switch (phase) {
        case Phase::Settling: return "settling";
        case Phase::Baseline: return "baseline";
        case Phase::Held: return "process_refresh_hold";
        case Phase::Restored: return "restored";
        case Phase::Done: return "done";
        case Phase::Aborted: return "aborted";
        case Phase::Failed: return "failed";
        default: return "disabled";
        }
    }
    static uint64_t Delta(uint64_t current, uint64_t previous) noexcept {
        return current >= previous ? current - previous : 0;
    }
    bool ReadOptions(Options& options) {
        bool supported = true;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED, &options.enabled) && supported;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_TICK_PERIOD, &options.tick) && supported;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_READCACHE_TICKS, &options.memory) && supported;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_TLBCACHE_TICKS, &options.tlb) && supported;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL, &options.partial) && supported;
        supported = VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, &options.total) && supported;
        return supported;
    }
    static bool Positive(const Options& options) noexcept {
        constexpr auto maximum = (std::numeric_limits<DWORD>::max)();
        return options.tick && options.tick <= maximum && options.memory && options.memory <= maximum &&
            options.tlb && options.tlb <= maximum && options.partial && options.partial <= maximum &&
            options.total && options.total <= maximum;
    }
    bool OtherOptionsMatch(const Options& options) const noexcept {
        return options.enabled == 1 && options.tick == original_.tick &&
            options.memory == original_.memory && options.tlb == original_.tlb;
    }
    static bool SetVerified(VMM_HANDLE handle, ULONG64 option, ULONG64 expected) {
        const bool set = VMMDLL_ConfigSet(handle, option, expected);
        ULONG64 actual{};
        const bool read = VMMDLL_ConfigGet(handle, option, &actual);
        const bool verified = set && read && actual == expected;
        if (!verified) Diagnostics::Event("warning", "refresh_experiment_option_unverified", std::format(
            "option=0x{:X}; set_ok={}; requested={}; read_ok={}; actual={}",
            option, set, expected, read, read ? std::to_string(actual) : "unavailable"));
        return verified;
    }
    // Try both restorations even if the first fails. Keep ownership of a failed
    // option so the destructor can make a final best-effort attempt.
    bool Restore() noexcept {
        bool restored = true;
        const auto restoreOption = [&](bool& touched, ULONG64 option, ULONG64 value) noexcept {
            if (!touched) return;
            bool ok{};
            try { ok = SetVerified(handle_, option, value); } catch (...) {}
            touched = !ok;
            restored = ok && restored;
        };
        restoreOption(partialTouched_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL, original_.partial);
        restoreOption(totalTouched_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, original_.total);
        if (!restored) {
            Diagnostics::Event("critical", "refresh_experiment_restore_failed",
                "Original process refresh intervals could not be restored and verified; stop this VMM session");
        }
        return restored;
    }
    void FinishPhase(TimePoint now) noexcept {
        try {
            const auto current = Diagnostics::GetSnapshot();
            std::string detail = std::format(
                "phase={}; wall_s={:.3f}; loops={}; slow_loops={}; reads={}; failed_reads={}; failed_batches={}; "
                "pawn_publishes={}; frames={}; slow_frames={}; pawn_age_s={:.3f}; worker_age_s={:.3f}; "
                "cumulative_max_loop_ms={:.3f}; cumulative_max_pawn_gap_ms={:.3f}",
                Name(phase_), std::chrono::duration<double>(now - phaseStarted_).count(),
                Delta(current.loops, phaseCounters_.loops), Delta(current.slowLoops, phaseCounters_.slowLoops),
                Delta(current.reads, phaseCounters_.reads), Delta(current.failedReads, phaseCounters_.failedReads),
                Delta(current.failedBatches, phaseCounters_.failedBatches),
                Delta(current.pawnPublishes, phaseCounters_.pawnPublishes), Delta(current.frames, phaseCounters_.frames),
                Delta(current.slowFrames, phaseCounters_.slowFrames), current.pawnPublishAgeSeconds,
                current.workerAgeSeconds, current.maxLoopMs, current.maxPawnPublishGapMs);
            for (size_t i = 0; i < current.timingLaneCount && i < Diagnostics::MaxTimingLanes; ++i) {
                const auto& lane = current.timingLanes[i];
                const auto name = std::string_view(lane.name.data());
                uint64_t previousCalls{}, previousFailures{};
                double previousElapsed{};
                for (size_t j = 0; j < phaseCounters_.timingLaneCount && j < Diagnostics::MaxTimingLanes; ++j) {
                    const auto& previous = phaseCounters_.timingLanes[j];
                    if (name == previous.name.data()) {
                        previousCalls = previous.calls;
                        previousFailures = previous.failed;
                        previousElapsed = previous.averageMs * static_cast<double>(previous.calls);
                        break;
                    }
                }
                const double elapsed = lane.averageMs * static_cast<double>(lane.calls) - previousElapsed;
                detail += std::format("; lane[{}]:calls={},failed={},elapsed_ms={:.3f}", name,
                    Delta(lane.calls, previousCalls), Delta(lane.failed, previousFailures),
                    std::isfinite(elapsed) && elapsed > 0 ? elapsed : 0);
            }
            Diagnostics::Event("info", "refresh_experiment_phase_end", detail);
        } catch (...) {
            Diagnostics::Event("warning", "refresh_experiment_metrics_unavailable", "Phase summary unavailable");
        }
    }
    void StartPhase(Phase phase, const Options& options) {
        phase_ = phase;
        phaseStarted_ = now_();
        phaseCounters_ = Diagnostics::GetSnapshot();
        Diagnostics::Event("info", "refresh_experiment_phase", std::format(
            "phase={}; duration_s=60; refresh_enabled={}; tick_period_ms={}; memory_cache_ticks={}; "
            "tlb_cache_ticks={}; process_partial_ticks={}; process_total_ticks={}; "
            "freshness_limit={}", Name(phase_), options.enabled, options.tick, options.memory, options.tlb,
            options.partial, options.total, phase == Phase::Held ?
            "PID/module metadata can remain stale until restoration; cached PID checks do not prove liveness; TLB refresh remains enabled" :
            "normal background refresh; phase counters are client measurements, not a target-engine timestamp"));
    }
    void Abort(std::string_view reason) noexcept {
        phase_ = Phase::Aborted;
        Diagnostics::Event("warning", "refresh_experiment_aborted", reason);
    }
    void RestoreOrStop() {
        if (!Restore()) {
            phase_ = Phase::Failed;
            throw std::runtime_error("Refresh experiment could not restore original backend settings");
        }
    }
    void BeginHold() {
        Options current{};
        if (!ReadOptions(current) || !OtherOptionsMatch(current) ||
            current.partial != original_.partial || current.total != original_.total) {
            Abort("Backend settings changed or could not be read before hold; no settings changed");
            return;
        }
        try {
            // An attempted native Set might have changed the value even if it
            // reports failure. Record restoration responsibility before calling.
            partialTouched_ = true;
            const bool partial = SetVerified(handle_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL, heldTicks_);
            bool total{};
            if (partial) {
                totalTouched_ = true;
                total = SetVerified(handle_, VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, heldTicks_);
            }
            Options held{};
            if (!partial || !total || !ReadOptions(held) || !OtherOptionsMatch(held) ||
                held.partial != heldTicks_ || held.total != heldTicks_) {
                RestoreOrStop();
                Abort("Hold settings could not be verified; original process intervals restored");
                return;
            }
            StartPhase(Phase::Held, held);
        } catch (...) {
            if (phase_ != Phase::Failed) {
                RestoreOrStop();
                Abort("Exception applying hold; original process intervals restored");
            }
            throw;
        }
    }

public:
    RefreshExperiment(VMM_HANDLE handle, bool enabled, NowFunction now = &Clock::now)
        : handle_(handle), now_(now ? now : &Clock::now), phase_(enabled ? Phase::Settling : Phase::Disabled) {
        created_ = nextAttempt_ = now_();
        if (enabled) Diagnostics::Event("info", "refresh_experiment_begin",
            "Opt-in A/B/A: 60s baseline, 60s process-refresh hold, 60s restored; MEM/TLB/paging unchanged. "
            "SLOW refresh still runs at its native cadence and can overlap a phase. Disabled defaults are not changed.");
    }
    ~RefreshExperiment() noexcept {
        if (!partialTouched_ && !totalTouched_) return;
        try { FinishPhase(now_()); } catch (...) {}
        const bool restored = Restore();
        if (restored) {
            try { Diagnostics::Event("info", "refresh_experiment_restored_on_exit", std::format(
                "Original process intervals restored and verified; process_partial_ticks={}; process_total_ticks={}",
                original_.partial, original_.total)); } catch (...) {}
        }
    }
    RefreshExperiment(const RefreshExperiment&) = delete;
    RefreshExperiment& operator=(const RefreshExperiment&) = delete;
    Phase GetPhase() const noexcept { return phase_; }
    bool Done() const noexcept {
        return phase_ == Phase::Disabled || phase_ == Phase::Done || phase_ == Phase::Aborted || phase_ == Phase::Failed;
    }
    static DWORD PostponedTicks(ULONG64 tickMs) {
        if (!tickMs) throw std::invalid_argument("A nonzero VMM tick period is required");
        const ULONG64 ticks = HoldPeriodMs / tickMs + (HoldPeriodMs % tickMs ? 1 : 0);
        const auto bounded = ticks ? ticks : 1;
        if (bounded > (std::numeric_limits<DWORD>::max)()) throw std::overflow_error("Postponed refresh period exceeds DWORD");
        return static_cast<DWORD>(bounded);
    }
    void Tick() {
        if (Done()) return;
        const auto now = now_();
        if (phase_ == Phase::Settling) {
            if (!handle_) { Abort("Missing VMM handle; no settings changed"); return; }
            if (now < nextAttempt_) return;
            Options defaults{};
            if (!ReadOptions(defaults)) { Abort("Required ConfigGet options unsupported; no settings changed"); return; }
            if (defaults.enabled != 1) { Abort("Background refresh is not enabled; no settings changed"); return; }
            if (!Positive(defaults)) {
                if (now - created_ >= SettleTimeout) Abort("VMM defaults did not settle within 15 seconds; no settings changed");
                else nextAttempt_ = now + std::chrono::milliseconds(250);
                return;
            }
            original_ = defaults;
            heldTicks_ = PostponedTicks(defaults.tick);
            StartPhase(Phase::Baseline, defaults);
            return;
        }
        if (now - phaseStarted_ < PhaseDuration) return;
        FinishPhase(now);
        if (phase_ == Phase::Baseline) {
            BeginHold();
        } else if (phase_ == Phase::Held) {
            RestoreOrStop();
            Options restored{};
            if (!ReadOptions(restored) || !OtherOptionsMatch(restored) ||
                restored.partial != original_.partial || restored.total != original_.total) {
                phase_ = Phase::Failed;
                Diagnostics::Event("critical", "refresh_experiment_restore_failed",
                    "Full restored backend configuration could not be verified; stop this VMM session");
                throw std::runtime_error("Restored refresh experiment configuration could not be verified");
            }
            StartPhase(Phase::Restored, restored);
        } else if (phase_ == Phase::Restored) {
            phase_ = Phase::Done;
            Diagnostics::Event("info", "refresh_experiment_done", "A/B/A completed; original process intervals retained; no further mutations");
        }
    }
};
