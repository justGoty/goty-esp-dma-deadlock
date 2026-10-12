#pragma once
#include <chrono>
#include <format>
#include <string_view>
#include "vmmdll.h"
#include "Diagnostics/Diagnostics.h"

// Application-owned maintenance for the verified MemProcFS 5.15.9 backend.
// The caller initializes -norefresh and primes process metadata before attach.
// Hot game reads must be resident-only (NOCACHE | NOPAGING), because MEM/TLB
// maintenance does not invalidate cached VAD/prototype metadata.
// targetGuardAlive may be true only for a recent, directly read kernel identity
// including PID/creation/exit state and unchanged DTB. Cached process metadata
// and a successful ConfigSet are not evidence of target liveness.
class ManagedRefresh {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFunction = TimePoint(*)();
    struct Metrics {
        uint64_t memoryCalls{}, tlbCalls{}, fastCalls{}, mediumCalls{}, slowCalls{}, failedCalls{};
        double elapsedMs{}, maxCallMs{};
    };

private:
    VMM_HANDLE handle_{};
    NowFunction now_{};
    bool enabled_{}, validated_{}, failed_{}, modeKnown_{}, previousGuardAlive_{}, forceMedium_{};
    TimePoint memoryAt_{}, tlbAt_{}, fastAt_{}, mediumAt_{}, slowAt_{}, summaryAt_{};
    Metrics metrics_{};
    static constexpr auto MemoryPeriod = std::chrono::milliseconds(300);
    static constexpr auto TlbPeriod = std::chrono::seconds(2);
    static constexpr auto FastPeriod = std::chrono::seconds(5);
    static constexpr auto MediumPeriod = std::chrono::seconds(15);
    static constexpr auto SlowPeriod = std::chrono::seconds(300);

    bool Fail(std::string_view reason) noexcept {
        failed_ = true;
        Diagnostics::Event("critical", "managed_refresh_failed", reason);
        return false;
    }
    bool Validate() {
        if (!handle_) return Fail("Missing VMM handle; application-owned refresh cannot run");
        ULONG64 enabled{}, major{}, minor{}, revision{};
        const bool supported =
            VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED, &enabled) &&
            VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_VMM_VERSION_MAJOR, &major) &&
            VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_VMM_VERSION_MINOR, &minor) &&
            VMMDLL_ConfigGet(handle_, VMMDLL_OPT_CONFIG_VMM_VERSION_REVISION, &revision);
        if (!supported) return Fail("Required ConfigGet capability unavailable; stop the -norefresh session");
        if (enabled != 0) return Fail("Background refresh remains enabled; application-owned mode cannot suppress process scans");
        if (major != 5 || minor != 15 || revision != 9)
            return Fail("Backend is not verified MemProcFS 5.15.9; stop the -norefresh session");
        validated_ = true;
        Diagnostics::Event("info", "managed_refresh_ready",
            "vmm=5.15.9; background_refresh=false; memory_partial_ms=300; tlb_partial_ms=2000; "
            "fallback_fast_ms=5000; fallback_medium_ms=15000; fallback_slow_ms=300000; "
            "startup_full_metadata_priming=caller; resident_only_hot_reads=required; "
            "ConfigSet acceptance does not prove fresh process metadata or liveness");
        return true;
    }
    bool Refresh(ULONG64 option, std::string_view name, uint64_t& counter) {
        ++counter;
        const auto started = now_();
        const bool accepted = VMMDLL_ConfigSet(handle_, option, 1);
        const auto elapsed = std::chrono::duration<double, std::milli>(now_() - started).count();
        metrics_.elapsedMs += elapsed;
        if (elapsed > metrics_.maxCallMs) metrics_.maxCallMs = elapsed;
        // One bounded lane avoids consuming five slots in the fixed registry.
        Diagnostics::LaneTick("refresh_maintenance", elapsed, accepted);
        if (!accepted) {
            ++metrics_.failedCalls;
            Diagnostics::Event("critical", "managed_refresh_operation_failed", std::format(
                "operation={}; option=0x{:X}; API rejected manual maintenance; stop the -norefresh session", name, option));
            failed_ = true;
            return false;
        }
        return true;
    }
    void Summary(TimePoint now) {
        if (now - summaryAt_ < std::chrono::seconds(60)) return;
        summaryAt_ = now;
        Diagnostics::Event("info", "managed_refresh_metrics", std::format(
            "mode={}; api_calls_cumulative:mem={},tlb={},fast={},medium={},slow={},failed={}; "
            "elapsed_ms_cumulative={:.3f}; max_call_ms_cumulative={:.3f}; API acceptance is not fresh-PID proof",
            previousGuardAlive_ ? "guard_alive_resident" : "guard_unavailable_bulk_fallback",
            metrics_.memoryCalls, metrics_.tlbCalls, metrics_.fastCalls, metrics_.mediumCalls,
            metrics_.slowCalls, metrics_.failedCalls, metrics_.elapsedMs, metrics_.maxCallMs));
    }

public:
    ManagedRefresh(VMM_HANDLE handle, bool enabled, NowFunction now = &Clock::now)
        : handle_(handle), now_(now ? now : &Clock::now), enabled_(enabled) {
        memoryAt_ = tlbAt_ = fastAt_ = mediumAt_ = slowAt_ = summaryAt_ = now_();
    }
    ManagedRefresh(const ManagedRefresh&) = delete;
    ManagedRefresh& operator=(const ManagedRefresh&) = delete;
    const Metrics& GetMetrics() const noexcept { return metrics_; }
    bool Failed() const noexcept { return failed_; }

    // False is terminal for a -norefresh session; the caller must stop it rather
    // than silently running without required maintenance. No reinitialization.
    bool Tick(bool targetGuardAlive) {
        if (!enabled_) return true;
        if (failed_) return false;
        if (!validated_ && !Validate()) return false;

        if (!modeKnown_ || previousGuardAlive_ != targetGuardAlive) {
            if (modeKnown_ && previousGuardAlive_ && !targetGuardAlive) forceMedium_ = true;
            previousGuardAlive_ = targetGuardAlive;
            modeKnown_ = true;
            Diagnostics::Event(targetGuardAlive ? "info" : "warning", "managed_refresh_mode",
                targetGuardAlive ? "Fresh kernel identity verified; keep MEM/TLB rotation and suppress process bulk scans" :
                "Kernel identity unavailable/lost; caller must hide ESP; FAST/MEDIUM/SLOW fallback enabled; cached process checks cannot mark green");
        }

        auto now = now_();
        if (now - memoryAt_ >= MemoryPeriod) {
            if (!Refresh(VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL, "memory_partial", metrics_.memoryCalls)) return false;
            memoryAt_ = now_();
        }
        now = now_();
        if (now - tlbAt_ >= TlbPeriod) {
            if (!Refresh(VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL, "tlb_partial", metrics_.tlbCalls)) return false;
            tlbAt_ = now_();
        }

        if (targetGuardAlive) {
            forceMedium_ = false;
        } else {
            now = now_();
            // SLOW includes MEDIUM; MEDIUM includes a full process refresh.
            // Run one highest-due bulk layer and never replay missed intervals.
            if (now - slowAt_ >= SlowPeriod) {
                if (!Refresh(VMMDLL_OPT_REFRESH_FREQ_SLOW, "slow", metrics_.slowCalls)) return false;
                slowAt_ = mediumAt_ = fastAt_ = now_();
                forceMedium_ = false;
            } else if (forceMedium_ || now - mediumAt_ >= MediumPeriod) {
                if (!Refresh(VMMDLL_OPT_REFRESH_FREQ_MEDIUM, "medium", metrics_.mediumCalls)) return false;
                mediumAt_ = fastAt_ = now_();
                forceMedium_ = false;
            } else if (now - fastAt_ >= FastPeriod) {
                if (!Refresh(VMMDLL_OPT_REFRESH_FREQ_FAST, "fast", metrics_.fastCalls)) return false;
                fastAt_ = now_();
            }
        }
        Summary(now_());
        return true;
    }
};
