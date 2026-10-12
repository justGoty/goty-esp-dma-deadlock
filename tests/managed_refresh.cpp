#define NOMINMAX
#include <windows.h>
#include "DMA/ManagedRefresh.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace Fake {
ManagedRefresh::TimePoint current{};
ManagedRefresh::TimePoint Now() { return current; }
std::map<ULONG64, ULONG64> options;
std::vector<ULONG64> operations;
std::vector<std::tuple<std::string, std::string, std::string>> events;
size_t gets{}, laneCalls{};
ULONG64 unsupported{}, rejected{};
void Reset() {
    current = {}; gets = laneCalls = 0; unsupported = rejected = 0;
    operations.clear(); events.clear();
    options = {{VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED, 0}, {VMMDLL_OPT_CONFIG_VMM_VERSION_MAJOR, 5},
        {VMMDLL_OPT_CONFIG_VMM_VERSION_MINOR, 15}, {VMMDLL_OPT_CONFIG_VMM_VERSION_REVISION, 9}};
}
void At(int ms) { current = ManagedRefresh::TimePoint{} + std::chrono::milliseconds(ms); }
size_t Count(ULONG64 option) { return std::count(operations.begin(), operations.end(), option); }
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
    return TRUE;
}
extern "C" BOOL VMMDLL_ConfigSet(VMM_HANDLE, ULONG64 option, ULONG64 value) {
    if (value != 1) return FALSE;
    Fake::operations.push_back(option);
    return option != Fake::rejected;
}
namespace Diagnostics {
void Event(std::string_view level, std::string_view event, std::string_view detail) noexcept {
    try { Fake::events.emplace_back(level, event, detail); } catch (...) {}
}
void LaneTick(std::string_view, double, bool) noexcept { ++Fake::laneCalls; }
}
namespace {
unsigned checks{};
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
VMM_HANDLE Handle() { return reinterpret_cast<VMM_HANDLE>(1); }
void TestAlive() {
    Fake::Reset();
    ManagedRefresh refresh(Handle(), true, Fake::Now);
    Check(Fake::gets == 0 && Fake::operations.empty(), "construction has no backend access");
    Check(refresh.Tick(true), "known disabled background backend accepted");
    Fake::At(299); Check(refresh.Tick(true) && Fake::operations.empty(), "MEM cadence not early");
    Fake::At(300); Check(refresh.Tick(true) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL) == 1, "MEM partial runs at300ms");
    Fake::At(2000); Check(refresh.Tick(true) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_TLB_PARTIAL) == 1, "TLB partial runs at2s");
    Fake::At(600000); Check(refresh.Tick(true), "long alive interval permits only cache rotation");
    Check(Fake::Count(VMMDLL_OPT_REFRESH_FREQ_FAST) == 0 && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 0 &&
        Fake::Count(VMMDLL_OPT_REFRESH_FREQ_SLOW) == 0, "alive target never triggers process bulk maintenance");
    Check(refresh.GetMetrics().memoryCalls == 3 && refresh.GetMetrics().tlbCalls == 2, "missed cache intervals never cause catch-up bursts");
    Check(Fake::laneCalls == Fake::operations.size(), "all manual calls produce bounded timing metrics");
    Check(Fake::HasEvent("managed_refresh_ready", "resident_only_hot_reads=required"), "paging/VAD prerequisite is explicit");
    Check(Fake::HasEvent("managed_refresh_metrics", "API acceptance is not fresh-PID proof"), "metrics do not claim kernel liveness");
}
void TestFallback() {
    Fake::Reset();
    ManagedRefresh refresh(Handle(), true, Fake::Now);
    Check(refresh.Tick(false), "initial unavailable guard enables fallback");
    Fake::At(4999); Check(refresh.Tick(false) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_FAST) == 0, "FAST cadence not early");
    Fake::At(5000); Check(refresh.Tick(false) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_FAST) == 1, "FAST runs at5s without guard");
    Fake::At(15000); Check(refresh.Tick(false), "MEDIUM due accepted");
    Check(Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 1 && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_FAST) == 1,
        "MEDIUM supersedes a simultaneously due FAST");
    Fake::At(300000); Check(refresh.Tick(false), "SLOW due accepted");
    Check(Fake::Count(VMMDLL_OPT_REFRESH_FREQ_SLOW) == 1 && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 1,
        "SLOW supersedes simultaneously due MEDIUM and FAST");
    const auto before = Fake::operations.size();
    Check(refresh.Tick(false) && Fake::operations.size() == before, "completed overdue bulk schedules do not replay on next loop");
    Check(Fake::HasEvent("managed_refresh_mode", "caller must hide ESP"), "unavailable guard cannot imply green ESP");
}
void TestTransition() {
    Fake::Reset();
    ManagedRefresh refresh(Handle(), true, Fake::Now);
    Check(refresh.Tick(true), "seed verified target");
    Fake::At(1000); Check(refresh.Tick(false), "guard loss requests recovery");
    Check(Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 1, "guard loss refreshes metadata immediately after verified liveness");
    Fake::At(1100); Check(refresh.Tick(false) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 1, "lost-guard recovery is not repeated each loop");
    Fake::At(15000); Check(refresh.Tick(true), "guard recovery suppresses pending bulk work");
    Fake::At(16000); Check(refresh.Tick(true) && Fake::Count(VMMDLL_OPT_REFRESH_FREQ_MEDIUM) == 1, "verified guard prevents periodic MEDIUM");
}
void TestTerminalFailure() {
    Fake::Reset();
    ManagedRefresh refresh(Handle(), true, Fake::Now);
    Check(refresh.Tick(true), "seed maintenance session");
    Fake::rejected = VMMDLL_OPT_REFRESH_FREQ_MEM_PARTIAL;
    Fake::At(300); Check(!refresh.Tick(true) && refresh.Failed(), "rejected manual MEM stops -norefresh session");
    const auto before = Fake::operations.size();
    Fake::rejected = 0; Fake::At(5000);
    Check(!refresh.Tick(false) && Fake::operations.size() == before, "failed maintenance never silently resumes");
    Check(refresh.GetMetrics().failedCalls == 1 && Fake::HasEvent("managed_refresh_operation_failed", "stop the -norefresh session"),
        "rejection is counted and logged critically");
}
void TestCapabilities() {
    Fake::Reset();
    { ManagedRefresh disabled(Handle(), false, Fake::Now); Fake::At(600000); Check(disabled.Tick(false), "disabled manager is inert"); }
    Check(Fake::gets == 0 && Fake::operations.empty(), "disabled mode has no backend calls");
    Fake::Reset();
    { ManagedRefresh missing(nullptr, true, Fake::Now); Check(!missing.Tick(true) && Fake::gets == 0, "missing handle fails without access"); }
    Fake::Reset();
    Fake::unsupported = VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED;
    { ManagedRefresh unsupported(Handle(), true, Fake::Now); Check(!unsupported.Tick(true), "unsupported setup fails closed"); }
    Check(Fake::operations.empty(), "unsupported setup performs no writes");
    Fake::Reset();
    Fake::options[VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED] = 1;
    { ManagedRefresh background(Handle(), true, Fake::Now); Check(!background.Tick(true), "background mode not silently combined with manual mode"); }
    Fake::Reset();
    Fake::options[VMMDLL_OPT_CONFIG_VMM_VERSION_MINOR] = 19;
    { ManagedRefresh unverified(Handle(), true, Fake::Now); Check(!unverified.Tick(true), "unverified backend version rejected"); }
}
}
int main() {
    try {
        TestAlive(); TestFallback(); TestTransition(); TestTerminalFailure(); TestCapabilities();
        std::cout << "PASS: " << checks << " managed refresh checks with mocked ConfigGet/ConfigSet; no hardware APIs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
