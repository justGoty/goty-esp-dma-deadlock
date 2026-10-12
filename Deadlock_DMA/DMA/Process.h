#pragma once
#include <windows.h>
#include <vmmdll.h>
#include <string>
#include <unordered_map>
#include <cstring>
#include <format>
#include <array>
#include <chrono>
#include <initializer_list>
#include <string_view>
#include "DMA.h"
#include "Diagnostics/Diagnostics.h"
#include "DMA/ReadFlags.h"
namespace ConstStrings {
    const std::string Game = "deadlock.exe";
    const std::string Client = "client.dll";
}
class Process {
    DWORD m_PID{};
    std::unordered_map<std::string, uintptr_t> m_Modules{};
    mutable std::chrono::steady_clock::time_point m_LastProbeTrace{};
    std::chrono::steady_clock::time_point m_LastModuleTrace{};
    bool PopulateModules(DMA_Connection* conn);
public:
    struct DiscoveryCandidate {
        DWORD pid{}, state{};
        std::string shortName, longName;
    };
    struct DiscoverySummary {
        DWORD uniqueCandidate{};
        size_t exactActiveMatches{}, activeShortNameMatches{}, targetLikeCount{}, sampleCount{};
        std::array<DiscoveryCandidate, 3> samples{};
    };
    static bool IsActiveExactProcess(const VMMDLL_PROCESS_INFORMATION& process, DWORD expectedPid, const std::string& name) noexcept {
        return expectedPid != 0 && process.dwPID == expectedPid && process.dwState == 0 &&
            strnlen_s(process.szNameLong, sizeof(process.szNameLong)) < sizeof(process.szNameLong) &&
            _stricmp(process.szNameLong, name.c_str()) == 0;
    }
    // Pure classification for bounded diagnostics. Only the original long-name,
    // active, unique rule selects a PID; a short-name match is evidence only.
    static DiscoverySummary SummarizeDiscovery(const VMMDLL_PROCESS_INFORMATION* table, size_t count, const std::string& name) {
        DiscoverySummary result;
        if (!table) return result;
        auto targetLike = [](std::string_view value) {
            for (const auto word : {std::string_view("deadlock"), std::string_view("citadel")}) {
                for (size_t i = 0; i + word.size() <= value.size(); ++i) {
                    bool matches = true;
                    for (size_t j = 0; j < word.size(); ++j) {
                        auto character = value[i + j];
                        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
                        if (character != word[j]) { matches = false; break; }
                    }
                    if (matches) return true;
                }
            }
            return false;
        };
        for (size_t i = 0; i < count; ++i) {
            const auto& process = table[i];
            const auto shortLength = strnlen_s(process.szName, sizeof(process.szName));
            const auto longLength = strnlen_s(process.szNameLong, sizeof(process.szNameLong));
            if (IsActiveExactProcess(process, process.dwPID, name)) {
                ++result.exactActiveMatches;
                result.uniqueCandidate = process.dwPID;
            }
            if (process.dwPID && !process.dwState && shortLength < sizeof(process.szName) &&
                _stricmp(process.szName, name.c_str()) == 0) ++result.activeShortNameMatches;
            const std::string_view shortName(process.szName, shortLength), longName(process.szNameLong, longLength);
            if (!targetLike(shortName) && !targetLike(longName)) continue;
            ++result.targetLikeCount;
            if (result.sampleCount < result.samples.size()) {
                auto& sample = result.samples[result.sampleCount++];
                sample = {process.dwPID, process.dwState,
                    shortLength < sizeof(process.szName) ? std::string(shortName) : "<unterminated>",
                    longLength < sizeof(process.szNameLong) ? std::string(longName) : "<unterminated>"};
            }
        }
        if (result.exactActiveMatches != 1) result.uniqueCandidate = 0;
        return result;
    }
    bool GetProcessInfo(const std::string& name, DMA_Connection* conn);
    bool ProbeCurrentProcess(DMA_Connection* conn) const;
    const uintptr_t GetBaseAddress() const;
    const uintptr_t GetClientBase() const;
    const DWORD GetPID() const;
    const uintptr_t GetModuleAddress(const std::string& name);
    template<typename T> bool TryRead(DMA_Connection* conn, uintptr_t address, T& value) {
        T buffer{}; DWORD read{};
        bool ok = conn && conn->GetHandle() && m_PID && address &&
            VMMDLL_MemReadEx(conn->GetHandle(), m_PID, address, reinterpret_cast<PBYTE>(&buffer), sizeof(T), &read, GameReadFlags());
        Diagnostics::RecordRead(sizeof(T), read, ok);
        if (!ok || read != sizeof(T)) {
            Diagnostics::Warning("read_incomplete", std::format("pid={} address=0x{:X} bytes={}/{}", m_PID, address, read, sizeof(T)));
            return false;
        }
        value = buffer; return true;
    }
    template<typename T> T ReadMem(DMA_Connection* conn, uintptr_t address) {
        T value{}; TryRead(conn, address, value); return value;
    }
};
