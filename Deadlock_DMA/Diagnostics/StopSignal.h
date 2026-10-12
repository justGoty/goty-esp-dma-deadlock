#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cwchar>

namespace Diagnostics {

// Cooperatively asks an existing instance in this Windows session to exit.
// The owner still performs its normal worker join and resource cleanup.
class StopSignal final {
public:
    StopSignal() noexcept {
        const auto name = Name(GetCurrentProcessId());
        event_ = CreateEventW(nullptr, TRUE, FALSE, name.data());
    }
    ~StopSignal() {
        if (event_) CloseHandle(event_);
    }
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;

    bool Requested() const noexcept {
        return event_ && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
    }

    static bool Request(DWORD pid) noexcept {
        if (!pid) return false;
        const auto name = Name(pid);
        const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.data());
        if (!event) return false;
        const bool requested = SetEvent(event) != FALSE;
        CloseHandle(event);
        return requested;
    }

private:
    static std::array<wchar_t, 64> Name(DWORD pid) noexcept {
        std::array<wchar_t, 64> name{};
        // Fixed prefix and one DWORD always fit; no user-supplied object name.
        std::swprintf(name.data(), name.size(), L"Local\\GOTYDeadlockDMA.Stop.%lu",
            static_cast<unsigned long>(pid));
        return name;
    }

    HANDLE event_{};
};

}
