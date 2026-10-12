#include "pch.h"
#include "DMA.h"
#include "Diagnostics/Diagnostics.h"
#include "Diagnostics/RuntimeOptions.h"
#include <chrono>
#include <cstddef>
#include <format>

namespace {
std::string InitializationErrorText(const LC_CONFIG_ERRORINFO* error) {
    if (!error || error->dwVersion != LC_CONFIG_ERRORINFO_VERSION) return {};
    constexpr auto headerBytes = offsetof(LC_CONFIG_ERRORINFO, wszUserText);
    if (error->cbStruct < headerBytes) return {};
    const auto capacity = (error->cbStruct - headerBytes) / sizeof(wchar_t);
    // Bound reads by the native allocation's declared size and a small diagnostic limit.
    const auto characters = std::min<size_t>({error->cwszUserText, capacity, 2048});
    size_t length{};
    while (length < characters && error->wszUserText[length]) ++length;
    if (!length) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, error->wszUserText, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "Native error text could not be converted to UTF-8";
    std::string text(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, error->wszUserText, static_cast<int>(length), text.data(), bytes, nullptr, nullptr) != bytes)
        return "Native error text could not be converted to UTF-8";
    return text;
}

void LogBackendConfiguration(VMM_HANDLE handle) noexcept {
    if (!handle) return;
    try {
        struct Option { const char* name; ULONG64 option; };
        static constexpr Option options[] = {
            {"refresh_enabled", VMMDLL_OPT_CONFIG_IS_REFRESH_ENABLED},
            {"tick_period_ms", VMMDLL_OPT_CONFIG_TICK_PERIOD},
            {"memory_cache_ticks", VMMDLL_OPT_CONFIG_READCACHE_TICKS},
            {"tlb_cache_ticks", VMMDLL_OPT_CONFIG_TLBCACHE_TICKS},
            {"process_partial_ticks", VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_PARTIAL},
            {"process_total_ticks", VMMDLL_OPT_CONFIG_PROCCACHE_TICKS_TOTAL},
            {"paging_enabled", VMMDLL_OPT_CONFIG_IS_PAGING_ENABLED},
            {"vmm_major", VMMDLL_OPT_CONFIG_VMM_VERSION_MAJOR},
            {"vmm_minor", VMMDLL_OPT_CONFIG_VMM_VERSION_MINOR},
            {"vmm_revision", VMMDLL_OPT_CONFIG_VMM_VERSION_REVISION},
        };
        std::string supported, unsupported;
        size_t supportedCount{};
        for (const auto& option : options) {
            ULONG64 value{};
            // Read the initialized session's settings; never alter refresh, paging or flags.
            if (VMMDLL_ConfigGet(handle, option.option, &value)) {
                ++supportedCount;
                supported += std::format("{}{}={}", supported.empty() ? "" : "; ", option.name, value);
            } else {
                unsupported += std::format("{}{}", unsupported.empty() ? "" : ",", option.name);
            }
        }
        Diagnostics::Event("info", "dma_backend_config", std::format(
            "readonly=true; supported={}/{}; {}; unsupported={}",
            supportedCount, std::size(options), supported, unsupported.empty() ? "none" : unsupported));
    } catch (...) {
        Diagnostics::Warning("dma_backend_config_unavailable", "Could not construct readonly backend configuration snapshot");
    }
}
}
DMA_Connection* DMA_Connection::GetInstance() { static DMA_Connection instance; return &instance; }
VMM_HANDLE DMA_Connection::GetHandle() const { return m_VMMHandle; }
bool DMA_Connection::EndConnection() {
	Diagnostics::SetDmaConnected(false);
    if (m_VMMHandle) { VMMDLL_Close(m_VMMHandle); m_VMMHandle = nullptr; Diagnostics::Event("info", "dma_disconnected"); }
    return true;
}
DMA_Connection::DMA_Connection() {
    Diagnostics::SetStage("connecting_dma");
    const auto snapshot = Diagnostics::GetSnapshot();
    const auto nativeLog = (std::filesystem::path(snapshot.logDirectory) / (snapshot.session + ".memprocfs.log")).string();
    std::vector<LPCSTR> args = { "", "-device", "fpga://algo=0", "-logfile", nativeLog.c_str(), "-loglevel", "fflush,f:4", "-printf", "-v" };
    if (RuntimeOptions::ManagedRefresh) args.push_back("-norefresh");
    Diagnostics::Event("info", "dma_initialize_begin", std::format("device=fpga://algo=0; native_log={}; refresh_policy={}; memory_map=default", nativeLog,
        RuntimeOptions::ManagedRefresh ? "worker_managed_with_target_guard" : "background_default"));
    const auto begin = std::chrono::steady_clock::now();
    PLC_CONFIG_ERRORINFO nativeError{};
    {
        Diagnostics::ScopedOperation operation("dma_initialize");
        m_VMMHandle = VMMDLL_InitializeEx(static_cast<DWORD>(args.size()), args.data(), &nativeError);
    }
    auto freeError = [](LC_CONFIG_ERRORINFO* error) { if (error) LcMemFree(error); };
    std::unique_ptr<LC_CONFIG_ERRORINFO, decltype(freeError)> errorGuard(nativeError, freeError);
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    Diagnostics::Event(m_VMMHandle ? "info" : "error", "dma_initialize_end",
        std::format("success={}; elapsed_ms={:.3f}; native_log={}", bool(m_VMMHandle), elapsed, nativeLog));
    if (!m_VMMHandle) {
        const auto detail = InitializationErrorText(nativeError);
        if (!detail.empty()) Diagnostics::Event("error", "dma_native_error", detail);
        throw std::runtime_error("VMMDLL_InitializeEx failed; see this session's .memprocfs.log and dma_native_error for transport/system initialization details");
    }
    Diagnostics::SetDmaConnected(true);
    Diagnostics::Event("info", "dma_connected", std::format("elapsed_ms={:.3f}", elapsed));
    LogBackendConfiguration(m_VMMHandle);
}
DMA_Connection::~DMA_Connection() { EndConnection(); }
