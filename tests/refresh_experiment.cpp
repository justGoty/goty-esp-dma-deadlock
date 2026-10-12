#define NOMINMAX
#include <windows.h>
#include "DMA/RefreshExperiment.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace Fake {
using Clock = RefreshExperiment::Clock;
Clock::time_point current{};
Clock::time_point Now() { return current; }
std::map<ULONG64, ULONG64> options;
std::vector<std::pair<ULONG64, ULONG64>> writes;
std::vector<std::tuple<std::string, std::string, std::string>> events;
Diagnostics::Snapshot snapshot;
size_t gets{}, failSetAt{};
ULONG64 unsupported{}, mismatch{};
bool failRestore{}, throwHeldSnapshot{};
void Reset() {
    current = {}; gets = failSetAt = 0; unsupported = mismatch = 0; failRestore = throwHeldSnapshot = false;
    writes.clear(); events.clear(); snapshot = {};
    options = {
        {VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED, 1},
        {VMMDLL_OPT_CONFIG_TICK_PERIOD, 100},
        {VMMDLL_OPT_CONFIG_READCACHE_TICKS, 3},
        {VMMDLL_OPT_CONFIG_TLBCACHE_TICKS, 20},
        {VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL, 50},
        {VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, 150},
    };
}
void At(int seconds) { current = Clock::time_point{} + std::chrono::seconds(seconds); }
bool HasEvent(std::string_view event, std::string_view detail = {}) {
    return std::any_of(events.begin(), events.end(), [&](const auto& item) {
        return std::get<1>(item) == event && std::get<2>(item).find(detail) != std::string::npos;
    });
}
}

extern "C" BOOL VMMDLL_ConfigGet(VMM_HANDLE, ULONG64 option, PULONG64 value) {
    ++Fake::gets;
    if (option == Fake::unsupported || !Fake::options.contains(option)) return FALSE;
    *value = Fake::options.at(option);
    if (option == Fake::mismatch && *value == 36000) {
        ++*value;
        Fake::mismatch = 0;
    }
    return TRUE;
}
extern "C" BOOL VMMDLL_ConfigSet(VMM_HANDLE, ULONG64 option, ULONG64 value) {
    Fake::writes.emplace_back(option, value);
    if (Fake::writes.size() == Fake::failSetAt ||
        (Fake::failRestore && option == VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL && value == 150)) return FALSE;
    Fake::options[option] = value;
    return TRUE;
}
namespace Diagnostics {
void Event(std::string_view level, std::string_view event, std::string_view detail) noexcept {
    try { Fake::events.emplace_back(level, event, detail); } catch (...) {}
}
Snapshot GetSnapshot() {
    if (Fake::throwHeldSnapshot &&
        Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL) == 36000 &&
        Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL) == 36000) {
        Fake::throwHeldSnapshot = false;
        throw std::runtime_error("Synthetic C++ snapshot exception");
    }
    return Fake::snapshot;
}
}

namespace {
unsigned checks{};
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
VMM_HANDLE Handle() { return reinterpret_cast<VMM_HANDLE>(1); }
void CheckOriginals() {
    Check(Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL) == 50, "partial original restored");
    Check(Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL) == 150, "total original restored");
    Check(Fake::options.at(VMMDLL_OPT_CONFIG_READCACHE_TICKS) == 3, "memory cache untouched");
    Check(Fake::options.at(VMMDLL_OPT_CONFIG_TLBCACHE_TICKS) == 20, "TLB cache untouched");
    Check(Fake::options.at(VMMDLL_OPT_CONFIG_TICK_PERIOD) == 100, "base tick untouched");
    for (const auto& [option, value] : Fake::writes) {
        (void)value;
        Check(option == VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL ||
            option == VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL, "only two process intervals may be written");
    }
}
void TestNormal() {
    Fake::Reset();
    {
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Settling, "constructor does not call VMM");
        Check(Fake::gets == 0 && Fake::writes.empty(), "construction has no backend access");
        Fake::snapshot.loops = 100;
        experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Baseline && Fake::writes.empty(), "baseline begins after settled reads");
        Fake::At(59); experiment.Tick();
        Check(Fake::writes.empty(), "baseline is at least sixty seconds");
        Fake::snapshot.loops = 130;
        Fake::snapshot.slowLoops = 4;
        Fake::At(60); experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Held, "hold begins at sixty seconds");
        Check(Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL) == 36000 &&
            Fake::options.at(VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL) == 36000, "both process intervals postponed for one hour");
        Check(Fake::HasEvent("refresh_experiment_phase_end", "loops=30; slow_loops=4"), "phase counters use deltas");
        Check(Fake::HasEvent("refresh_experiment_phase", "PID/module metadata can remain stale"), "hold logs cached-liveness limitation");
        Fake::At(119); experiment.Tick();
        Check(Fake::writes.size() == 2, "hold performs no periodic backend writes");
        Fake::At(120); experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Restored, "restore is a separate measurement phase");
        CheckOriginals();
        const auto before = Fake::writes.size();
        Fake::At(180); experiment.Tick();
        Check(experiment.Done() && experiment.GetPhase() == RefreshExperiment::Phase::Done, "all phases finish");
        Fake::At(500); experiment.Tick();
        Check(Fake::writes.size() == before, "completed experiment never mutates backend again");
    }
    Check(Fake::writes.size() == 4, "destructor does not repeat verified restoration");
}
void TestApplyFailure(bool readbackMismatch) {
    Fake::Reset();
    RefreshExperiment experiment(Handle(), true, Fake::Now);
    experiment.Tick();
    if (readbackMismatch) Fake::mismatch = VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL;
    else Fake::failSetAt = 2;
    Fake::At(60); experiment.Tick();
    Check(experiment.GetPhase() == RefreshExperiment::Phase::Aborted, "unverified hold aborts experiment");
    CheckOriginals();
    Check(Fake::HasEvent("refresh_experiment_aborted", "original process intervals restored"), "successful apply rollback is recorded");
    Check(Fake::HasEvent("refresh_experiment_option_unverified", readbackMismatch ? "actual=36001" : "set_ok=false"),
        "failure diagnostics distinguish readback mismatch from native Set failure");
}
void TestExitDuringHold() {
    Fake::Reset();
    {
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        experiment.Tick(); Fake::At(60); experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Held, "seed held phase before exit");
        Fake::At(75);
    }
    CheckOriginals();
    Check(Fake::HasEvent("refresh_experiment_restored_on_exit"), "RAII exit restoration is recorded");
    Check(Fake::HasEvent("refresh_experiment_phase_end", "phase=process_refresh_hold; wall_s=15.000"), "exit logs the interrupted hold duration");
}
void TestRestoreFailure() {
    Fake::Reset();
    {
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        experiment.Tick(); Fake::At(60); experiment.Tick();
        Fake::failRestore = true;
        Fake::At(120);
        bool threw{};
        try { experiment.Tick(); } catch (const std::runtime_error&) { threw = true; }
        Check(threw && experiment.GetPhase() == RefreshExperiment::Phase::Failed, "normal restore failure stops the session");
        Check(Fake::HasEvent("refresh_experiment_restore_failed", "stop this VMM session"), "restore failure is critical and explicit");
        Fake::failRestore = false;
    }
    CheckOriginals();
    Check(Fake::HasEvent("refresh_experiment_restored_on_exit"), "destructor retries a failed owned option without throwing");
}
void TestObserverException() {
    Fake::Reset();
    {
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        experiment.Tick();
        Fake::throwHeldSnapshot = true;
        Fake::At(60);
        bool threw{};
        try { experiment.Tick(); } catch (const std::runtime_error&) { threw = true; }
        Check(threw, "unexpected C++ observer exception propagates after cleanup");
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Aborted,
            "exception after native changes aborts the experiment");
    }
    CheckOriginals();
}
void TestSettlingAndDisabled() {
    Fake::Reset();
    {
        RefreshExperiment experiment(Handle(), false, Fake::Now);
        experiment.Tick(); Fake::At(500); experiment.Tick();
    }
    Check(Fake::gets == 0 && Fake::writes.empty(), "disabled mode never accesses VMM");
    Fake::Reset();
    {
        Fake::options[VMMDLL_OPT_CONFIG_TICK_PERIOD] = 0;
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Settling, "zero defaults trigger nonblocking settling");
        const auto before = Fake::gets;
        experiment.Tick();
        Check(Fake::gets == before, "settling retry has a bounded interval");
        Fake::At(1); Fake::options[VMMDLL_OPT_CONFIG_TICK_PERIOD] = 100; experiment.Tick();
        Check(experiment.GetPhase() == RefreshExperiment::Phase::Baseline, "settled defaults start baseline after retry");
        Fake::At(60); experiment.Tick();
        Check(Fake::writes.empty(), "settling time does not shorten baseline");
    }
    Fake::Reset();
    {
        Fake::options[VMMDLL_OPT_CONFIG_TLBCACHE_TICKS] = 0;
        RefreshExperiment experiment(Handle(), true, Fake::Now);
        experiment.Tick(); Fake::At(15); experiment.Tick();
        Check(experiment.Done() && Fake::writes.empty(), "zero defaults time out after fifteen seconds without writes");
    }
    Fake::Reset();
    {
        Fake::unsupported = VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL;
        RefreshExperiment experiment(Handle(), true, Fake::Now); experiment.Tick();
        Check(experiment.Done() && Fake::writes.empty(), "unsupported ConfigGet aborts without guessing zero");
    }
    Fake::Reset();
    {
        Fake::options[VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED] = 0;
        RefreshExperiment experiment(Handle(), true, Fake::Now); experiment.Tick();
        Check(experiment.Done() && Fake::writes.empty(), "already disabled background refresh is not changed");
    }
    Check(RefreshExperiment::PostponedTicks(100) == 36000, "one-hour ticks at normal period");
    Check(RefreshExperiment::PostponedTicks(333) == 10811, "one-hour ticks round up");
    Check(RefreshExperiment::PostponedTicks(4000000) == 1, "one-hour ticks never become zero");
}
}

int main() {
    try {
        TestNormal(); TestApplyFailure(false); TestApplyFailure(true);
        TestExitDuringHold(); TestRestoreFailure(); TestObserverException(); TestSettlingAndDisabled();
        std::cout << "PASS: " << checks << " refresh experiment checks with mocked ConfigGet/ConfigSet; no hardware APIs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
