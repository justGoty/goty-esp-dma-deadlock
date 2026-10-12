#pragma once
#include <deque>
#include <format>
#include <string>
#include <type_traits>
#include "vmmdll.h"
#include "Diagnostics/Diagnostics.h"
#include "DMA/ReadFlags.h"

// Request byte counters remain at stable addresses until Clear/Close.
class ScatterBatch {
    struct Request { ULONG64 address{}; DWORD expected{}, actual{}; PDWORD caller{}; bool prepared{}; };
    VMMDLL_SCATTER_HANDLE handle{};
    DWORD pid{};
    std::deque<Request> requests;
    bool preparedOk{true};
public:
    template<typename T> static bool ReadValue(VMM_HANDLE vmm, DWORD process, ULONG64 address, T& value) {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= MAXDWORD);
        if (!vmm || !process || !address) return false;
        T next{};
        ScatterBatch batch(vmm, process);
        if (!batch.Prepare(address, sizeof(T), reinterpret_cast<PBYTE>(&next)) || !batch.Execute()) return false;
        value = next;
        return true;
    }
    ScatterBatch(VMM_HANDLE vmm, DWORD process) : pid(process) {
        if (vmm) handle = VMMDLL_Scatter_Initialize(vmm, pid, GameReadFlags());
        if (handle) ++Diagnostics::OpenScatter;
        else Diagnostics::Warning("scatter_init_failed", "No scatter handle");
    }
    ~ScatterBatch() { if (handle) { VMMDLL_Scatter_CloseHandle(handle); --Diagnostics::OpenScatter; } }
    ScatterBatch(const ScatterBatch&) = delete;
    ScatterBatch& operator=(const ScatterBatch&) = delete;
    bool Prepare(ULONG64 address, DWORD bytes, PBYTE buffer, PDWORD caller = nullptr) {
        requests.push_back({address, bytes, 0, caller, false}); auto& request = requests.back();
        if (caller) *caller = 0;
        if (!handle || !address || !buffer || !bytes || !VMMDLL_Scatter_PrepareEx(handle, address, bytes, buffer, &request.actual)) {
            preparedOk = false;
            Diagnostics::Warning("scatter_prepare_failed", "Request rejected");
            return false;
        }
        request.prepared = true;
        return true;
    }
    bool Execute() {
        if (requests.empty()) return preparedOk;
        ++Diagnostics::Batches;
        // ExecuteRead does not allow memory writes.
        bool executed = handle && VMMDLL_Scatter_ExecuteRead(handle);
        bool complete = executed && preparedOk;
        ULONG64 expectedTotal{}, actualTotal{};
        size_t badRequests{};
        std::string firstBad;
        for (auto& request : requests) {
            if (request.caller) *request.caller = request.actual;
            Diagnostics::RecordRead(request.expected, request.actual, executed);
            complete = complete && request.actual == request.expected;
            expectedTotal += request.expected;
            actualTotal += request.actual;
            if (!executed || !request.prepared || request.actual != request.expected) {
                ++badRequests;
                if (badRequests <= 3) {
                    firstBad += std::format(" [address=0x{:X},expected={},actual={},prepared={}]",
                        request.address, request.expected, request.actual, request.prepared);
                }
            }
        }
        if (!complete) {
            ++Diagnostics::FailedBatches;
            // Warning throttles this event to once per five seconds. Bound address samples
            // so a large failed batch never turns into a per-request log dump.
            Diagnostics::Warning("scatter_incomplete", std::format(
                "executed={}; prepared={}; pid={}; requests={}; bad_requests={}; expected_bytes={}; actual_bytes={}; first_bad={}",
                executed, preparedOk, pid, requests.size(), badRequests, expectedTotal, actualTotal, firstBad));
        }
        return complete;
    }
    bool Clear() {
        if (!handle || !VMMDLL_Scatter_Clear(handle, pid, GameReadFlags())) {
            Diagnostics::Warning("scatter_clear_failed", "Could not reset scatter batch"); return false;
        }
        requests.clear(); preparedOk = true; return true;
    }
};
