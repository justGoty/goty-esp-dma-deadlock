#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Construct and destroy on the same thread. This request lasts only for this
// scope/thread and never changes the user's saved Windows power plan.
class DisplayAwake final {
public:
    DisplayAwake() noexcept {
        previous_ = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    }
    ~DisplayAwake() {
        // A failed acquisition changes no state. Otherwise preserve existing
        // requirements, including any caller-owned system/display/away flags.
        if (previous_) SetThreadExecutionState(previous_ | ES_CONTINUOUS);
    }
    DisplayAwake(const DisplayAwake&) = delete;
    DisplayAwake& operator=(const DisplayAwake&) = delete;

    bool Valid() const noexcept { return previous_ != 0; }

private:
    EXECUTION_STATE previous_{};
};
