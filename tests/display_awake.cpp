#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace PowerProbe {
EXECUTION_STATE state{ES_CONTINUOUS};
std::array<EXECUTION_STATE, 16> calls{};
size_t count{}, failAt{};
EXECUTION_STATE WINAPI Set(EXECUTION_STATE requested) {
    calls.at(count++) = requested;
    if (count == failAt) return 0;
    const auto previous = state;
    state = requested;
    return previous;
}
void Reset(EXECUTION_STATE initial = ES_CONTINUOUS, size_t failureCall = 0) {
    state = initial;
    count = 0;
    failAt = failureCall;
    calls.fill(0);
}
}
#define SetThreadExecutionState PowerProbe::Set
#include "Timing/DisplayAwake.h"
#undef SetThreadExecutionState

int checks{};
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

int main() {
    static_assert(!std::is_copy_constructible_v<DisplayAwake> && !std::is_move_constructible_v<DisplayAwake>);
    static_assert(!std::is_copy_assignable_v<DisplayAwake> && !std::is_move_assignable_v<DisplayAwake>);
    constexpr auto requested = ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED;
    for (const EXECUTION_STATE initial : {ES_CONTINUOUS, ES_CONTINUOUS | ES_SYSTEM_REQUIRED,
        ES_CONTINUOUS | ES_DISPLAY_REQUIRED, ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED}) {
        PowerProbe::Reset(initial);
        {
            DisplayAwake awake;
            Check(awake.Valid(), "Successful execution-state request is observable");
            Check(PowerProbe::count == 1 && PowerProbe::calls[0] == requested,
                "Requests continuous display and system activity exactly once");
        }
        Check(PowerProbe::count == 2 && PowerProbe::calls[1] == initial && PowerProbe::state == initial,
            "Destructor restores the precise previous continuous execution state");
    }
    PowerProbe::Reset(ES_DISPLAY_REQUIRED);
    { DisplayAwake awake; }
    Check(PowerProbe::state == (ES_DISPLAY_REQUIRED | ES_CONTINUOUS),
        "Restoration is continuous even if the previous-state result lacked that flag");

    PowerProbe::Reset(ES_CONTINUOUS | ES_SYSTEM_REQUIRED, 1);
    {
        DisplayAwake awake;
        Check(!awake.Valid(), "Failed acquisition is observable");
    }
    Check(PowerProbe::count == 1 && PowerProbe::state == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED),
        "Failed acquisition performs no restore and preserves the caller state");

    PowerProbe::Reset(ES_CONTINUOUS, 2);
    { DisplayAwake awake; Check(awake.Valid(), "Restore failure does not alter acquisition success"); }
    Check(PowerProbe::count == 2 && PowerProbe::calls[1] == ES_CONTINUOUS,
        "Restore failure returns without retries or a blocking wait");

    PowerProbe::Reset(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    {
        DisplayAwake outer;
        { DisplayAwake inner; }
        Check(PowerProbe::state == requested, "Nested scope restores the outer activity request");
    }
    Check(PowerProbe::state == (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) && PowerProbe::count == 4,
        "Outer scope restores the original caller state after nested cleanup");
    std::cout << "PASS: " << checks << " display-awake checks; mocked API; no power, UI, or DMA changes\n";
}
