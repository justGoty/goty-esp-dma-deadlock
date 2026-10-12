#include "pch.h"
#include "Process.h"
#include "Diagnostics/Diagnostics.h"
extern std::atomic<bool> bRunning;
namespace {
using Clock = std::chrono::steady_clock;
bool TraceDue(Clock::time_point& previous, std::chrono::seconds interval = std::chrono::seconds(5)) {
    const auto now = Clock::now();
    if (previous != Clock::time_point{} && now - previous < interval) return false;
    previous = now;
    return true;
}
double ElapsedMs(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}
std::string CandidateDetails(const Process::DiscoverySummary& summary) {
    std::string detail;
    for (size_t i = 0; i < summary.sampleCount; ++i) {
        const auto& candidate = summary.samples[i];
        detail += std::format(" [pid={},state={},short={},long={}]", candidate.pid, candidate.state,
            candidate.shortName, candidate.longName);
    }
    return detail.empty() ? "none" : detail;
}
void WaitForRetry() {
    for (int i = 0; i < 10 && bRunning; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
void RefreshForSelection(DMA_Connection* conn, Clock::time_point& previous) {
    if (!RuntimeOptions::ManagedRefresh) return;
    const auto now = Clock::now();
    if (previous != Clock::time_point{} && now - previous < std::chrono::seconds(5)) return;
    Diagnostics::ScopedOperation operation("process_refresh_attach");
    for (const auto option : {VMMDLL_OPT_REFRESH_FREQ_MEM, VMMDLL_OPT_REFRESH_FREQ_TLB, VMMDLL_OPT_REFRESH_FREQ_MEDIUM}) {
        if (!VMMDLL_ConfigSet(conn->GetHandle(), option, 1))
            throw std::runtime_error("Managed startup refresh API rejected an operation");
    }
    previous = Clock::now();
    // ConfigSet acceptance is not proof of a successful native refresh. The
    // selected PID is validated from its kernel identity before ESP can run.
    Diagnostics::Event("info", "process_refresh_attach", "Full MEM/TLB + MEDIUM requested; subsequent identity/module/read validation required");
}
}
bool Process::GetProcessInfo(const std::string& name, DMA_Connection* conn) {
    m_PID = 0; m_Modules.clear(); Diagnostics::SetStage("waiting_process");
    Clock::time_point lastTrace{};
    Clock::time_point lastRefresh{};
    uint64_t attempt{};
    while (bRunning) {
        RefreshForSelection(conn, lastRefresh);
        ++attempt;
        const bool trace = TraceDue(lastTrace);
        DWORD lookupPid{};
        bool lookupOk{};
        if (trace) Diagnostics::Event("info", "process_lookup_begin", std::format(
            "attempt={}; name={}; diagnostic_only=true", attempt, name));
        const auto lookupStarted = Clock::now();
        {
            Diagnostics::ScopedOperation operation("process_lookup");
            lookupOk = VMMDLL_PidGetFromName(conn->GetHandle(), name.c_str(), &lookupPid);
        }
        if (trace) Diagnostics::Event("info", "process_lookup_end", std::format(
            "attempt={}; success={}; candidate_pid={}; elapsed_ms={:.3f}; diagnostic_only=true",
            attempt, lookupOk, lookupPid, ElapsedMs(lookupStarted)));
        if (!bRunning) return false;

        PVMMDLL_PROCESS_INFORMATION table{};
        DWORD count{};
        bool ok{};
        if (trace) Diagnostics::Event("info", "process_enum_begin", std::format("attempt={}", attempt));
        const auto enumStarted = Clock::now();
        {
            Diagnostics::ScopedOperation operation("process_enumerate");
            ok = VMMDLL_ProcessGetInformationAll(conn->GetHandle(), &table, &count);
        }
        const auto enumElapsedMs = ElapsedMs(enumStarted);
        auto freeTable = [](VMMDLL_PROCESS_INFORMATION* value) { if (value) VMMDLL_MemFree(value); };
        std::unique_ptr<VMMDLL_PROCESS_INFORMATION, decltype(freeTable)> tableGuard(table, freeTable);
        const auto summary = SummarizeDiscovery(ok ? table : nullptr, count, name);
        if (trace) Diagnostics::Event("info", "process_enum_end", std::format(
            "attempt={}; success={}; table_present={}; entries={}; exact_active_matches={}; active_short_name_matches={}; "
            "target_like_count={}; elapsed_ms={:.3f}; candidate_details={}",
            attempt, ok, table != nullptr, count, summary.exactActiveMatches, summary.activeShortNameMatches,
            summary.targetLikeCount, enumElapsedMs, CandidateDetails(summary)));
        // The cached-name preflight never selects a process: uniqueness and the
        // original exact active long-name check remain mandatory.
        tableGuard.reset();
        if (!ok) Diagnostics::Warning("process_table_failed", "Could not enumerate target processes");
        else if (summary.exactActiveMatches > 1) Diagnostics::Warning("process_ambiguous", "Multiple active exact-name matches");
        else if (summary.exactActiveMatches == 1) {
            m_PID = summary.uniqueCandidate; Diagnostics::SetStage("waiting_modules", m_PID);
            Diagnostics::SetGameVisible(true, m_PID);
            Diagnostics::Event("info", "process_selected", std::format("name={} pid={}", name, m_PID));
            if (PopulateModules(conn)) return true;
            m_PID = 0; m_Modules.clear(); Diagnostics::SetStage("waiting_process");
        }
        WaitForRetry();
    }
    return false;
}
bool Process::ProbeCurrentProcess(DMA_Connection* conn) const {
    VMMDLL_PROCESS_INFORMATION process{};
    process.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    process.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    SIZE_T bytes = sizeof(process);
    const bool trace = TraceDue(m_LastProbeTrace);
    if (trace) Diagnostics::Event("info", "process_probe_begin", std::format("pid={}", m_PID));
    const auto started = Clock::now();
    bool queried{};
    if (conn && conn->GetHandle() && m_PID) {
        Diagnostics::ScopedOperation operation("process_probe");
        queried = VMMDLL_ProcessGetInformation(conn->GetHandle(), m_PID, &process, &bytes);
    }
    const bool visible = queried && IsActiveExactProcess(process, m_PID, ConstStrings::Game);
    if (trace) Diagnostics::Event("info", "process_probe_end", std::format(
        "pid={}; queried={}; visible={}; returned_pid={}; state={}; bytes={}; elapsed_ms={:.3f}",
        m_PID, queried, visible, process.dwPID, process.dwState, bytes, ElapsedMs(started)));
    Diagnostics::SetGameVisible(visible, m_PID);
    if (!visible) Diagnostics::Warning("game_not_visible", queried ? "Selected target PID is inactive or no longer deadlock.exe" : "Current target process query failed");
    return visible;
}
const uintptr_t Process::GetBaseAddress() const { return m_Modules.at(ConstStrings::Game); }
const uintptr_t Process::GetClientBase() const { return m_Modules.at(ConstStrings::Client); }
const DWORD Process::GetPID() const { return m_PID; }
const uintptr_t Process::GetModuleAddress(const std::string& name) { return m_Modules.at(name); }
bool Process::PopulateModules(DMA_Connection* conn) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto lastRefresh = Clock::now();
    while (bRunning && std::chrono::steady_clock::now() < until) {
        RefreshForSelection(conn, lastRefresh);
        if (!ProbeCurrentProcess(conn)) return false;
        const bool trace = TraceDue(m_LastModuleTrace);
        for (const auto* module : {&ConstStrings::Game, &ConstStrings::Client}) {
            if (trace) Diagnostics::Event("info", "process_module_begin", std::format("pid={}; module={}", m_PID, *module));
            const auto started = Clock::now();
            uintptr_t address{};
            {
                Diagnostics::ScopedOperation operation(module == &ConstStrings::Game ? "module_game" : "module_client");
                address = VMMDLL_ProcessGetModuleBaseU(conn->GetHandle(), m_PID, module->c_str());
            }
            m_Modules[*module] = address;
            if (trace) Diagnostics::Event("info", "process_module_end", std::format(
                "pid={}; module={}; base=0x{:X}; elapsed_ms={:.3f}", m_PID, *module, address, ElapsedMs(started)));
        }
        if (m_Modules[ConstStrings::Game] && m_Modules[ConstStrings::Client]) {
            Diagnostics::Event("info", "modules_ready", std::format("game=0x{:X} client=0x{:X}", GetBaseAddress(), GetClientBase()));
            return true;
        }
        Diagnostics::Warning("modules_pending", "Waiting for modules; re-enumeration after 30s"); WaitForRetry();
    }
    return false;
}
