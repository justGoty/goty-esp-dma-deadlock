#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

// Fault injection is confined to this translation unit. Real timer handles and
// the production helper are exercised without linking VMM or opening hardware.
namespace TimerProbe {
enum class Fault { None, HighResolutionCreate, AllCreate, Set, WaitFailed, WaitTimeout };
Fault fault{};
unsigned createCalls{}, createdHandles{}, closeCalls{}, setCalls{}, waitCalls{}, cancelCalls{};
unsigned failedCloses{};
LONGLONG lastDue{};
DWORD lastTimeout{}, lastCreateFlags{};
LONG lastPeriod{};

HANDLE WINAPI Create(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access) {
    ++createCalls;
    lastCreateFlags = flags;
    if (fault == Fault::AllCreate || (fault == Fault::HighResolutionCreate && flags != 0)) return nullptr;
    const auto timer = ::CreateWaitableTimerExW(attributes, name, flags, access);
    if (timer) ++createdHandles;
    return timer;
}
BOOL WINAPI Set(HANDLE timer, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE completion,
    LPVOID argument, BOOL resume) {
    ++setCalls;
    lastDue = due->QuadPart;
    lastPeriod = period;
    return fault == Fault::Set ? FALSE : ::SetWaitableTimer(timer, due, period, completion, argument, resume);
}
DWORD WINAPI Wait(HANDLE timer, DWORD timeout) {
    ++waitCalls;
    lastTimeout = timeout;
    if (fault == Fault::WaitFailed) return WAIT_FAILED;
    if (fault == Fault::WaitTimeout) return WAIT_TIMEOUT;
    return ::WaitForSingleObject(timer, timeout);
}
BOOL WINAPI Cancel(HANDLE timer) { ++cancelCalls; return ::CancelWaitableTimer(timer); }
BOOL WINAPI Close(HANDLE timer) {
    ++closeCalls;
    const auto closed = ::CloseHandle(timer);
    if (!closed) ++failedCloses;
    return closed;
}
}
#define CreateWaitableTimerExW TimerProbe::Create
#define SetWaitableTimer TimerProbe::Set
#define WaitForSingleObject TimerProbe::Wait
#define CancelWaitableTimer TimerProbe::Cancel
#define CloseHandle TimerProbe::Close
#include "Timing/ShortWait.h"
#undef CreateWaitableTimerExW
#undef SetWaitableTimer
#undef WaitForSingleObject
#undef CancelWaitableTimer
#undef CloseHandle

using namespace std::chrono;
int checks{};
void Check(bool ok, const char* reason) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << reason << '\n'; std::exit(1); }
}
template<typename F> double Elapsed(F operation) {
    const auto start = steady_clock::now();
    operation();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}
void Report(const char* name, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    std::cout << name << " count=" << samples.size() << " min_ms=" << samples.front()
        << " p50_ms=" << samples[samples.size() / 2] << " p95_ms=" << samples[samples.size() * 95 / 100]
        << " max_ms=" << samples.back() << '\n';
}

int main() {
    static_assert(!std::is_copy_constructible_v<ShortWait> && !std::is_move_constructible_v<ShortWait>);
    static_assert(!std::is_copy_assignable_v<ShortWait> && !std::is_move_assignable_v<ShortWait>);
    DWORD before{}, after{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &before), "Read initial process handle count");
    for (int i = 0; i < 500; ++i) { ShortWait waiter; waiter.WaitFor(nanoseconds::zero()); }
    Check(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after, "500 timer lifetimes leak no handles");
    Check(TimerProbe::createdHandles == 500 && TimerProbe::closeCalls == 500 && TimerProbe::failedCloses == 0,
        "Each created timer is closed exactly once");
    Check(TimerProbe::setCalls == 0 && TimerProbe::waitCalls == 0, "Zero wait never schedules or waits on a timer");
    {
        ShortWait waiter;
        const auto setBefore = TimerProbe::setCalls, waitBefore = TimerProbe::waitCalls;
        waiter.WaitFor(-seconds(100));
        waiter.WaitFor(ShortWait::Clock::duration::min());
        Check(TimerProbe::setCalls == setBefore && TimerProbe::waitCalls == waitBefore,
            "Negative waits never schedule a native timer");
        const auto createsBefore = TimerProbe::createCalls;
        std::vector<double> normal, old;
        for (int i = 0; i < 200; ++i) normal.push_back(Elapsed([&] { waiter.WaitFor(milliseconds(1)); }));
        for (int i = 0; i < 200; ++i) old.push_back(Elapsed([] { std::this_thread::sleep_for(milliseconds(1)); }));
        // Measurements are evidence, never scheduler-dependent pass/fail gates.
        Report("short_wait_1ms", normal);
        Report("old_sleep_for_1ms", old);
        Check(TimerProbe::createCalls == createsBefore, "200 waits reuse the same timer handle");
        Check(TimerProbe::lastDue == -10'000 && TimerProbe::lastPeriod == 0,
            "One millisecond requests a relative one-shot timer");
        Check(TimerProbe::lastTimeout == 3, "One millisecond wait retains a finite fail-safe timeout");
        waiter.WaitFor(ShortWait::Clock::duration::max());
        Check(TimerProbe::lastDue == -500'000 && TimerProbe::lastTimeout == 52,
            "Extreme positive duration is capped before conversion and native wait");
    }
    TimerProbe::fault = TimerProbe::Fault::HighResolutionCreate;
    const auto createBefore = TimerProbe::createCalls, closeBefore = TimerProbe::closeCalls;
    {
        ShortWait waiter;
        std::cout << "regular_timer_fallback_ms=" << Elapsed([&] { waiter.WaitFor(milliseconds(1)); }) << '\n';
        Check(TimerProbe::lastCreateFlags == 0, "Unsupported high-resolution timer retries a regular timer");
    }
    Check(TimerProbe::createCalls == createBefore + 2 && TimerProbe::closeCalls == closeBefore + 1,
        "High-resolution creation fallback closes only its successful handle");
    TimerProbe::fault = TimerProbe::Fault::AllCreate;
    const auto setsBeforeMissing = TimerProbe::setCalls, waitsBeforeMissing = TimerProbe::waitCalls;
    const auto closesBeforeMissing = TimerProbe::closeCalls;
    {
        ShortWait waiter;
        std::cout << "missing_timer_fallback_ms=" << Elapsed([&] { waiter.WaitFor(milliseconds(1)); }) << '\n';
    }
    Check(TimerProbe::setCalls == setsBeforeMissing && TimerProbe::waitCalls == waitsBeforeMissing &&
        TimerProbe::closeCalls == closesBeforeMissing, "Missing handle is never scheduled, waited on, or closed");
    TimerProbe::fault = TimerProbe::Fault::None;
    {
        ShortWait waiter;
        TimerProbe::fault = TimerProbe::Fault::Set;
        const auto waitsBeforeSetFailure = TimerProbe::waitCalls;
        std::cout << "set_failure_fallback_ms=" << Elapsed([&] { waiter.WaitFor(milliseconds(1)); }) << '\n';
        Check(TimerProbe::waitCalls == waitsBeforeSetFailure, "Failed timer setup never enters a native wait");
        for (const auto fault : {TimerProbe::Fault::WaitFailed, TimerProbe::Fault::WaitTimeout}) {
            TimerProbe::fault = fault;
            const auto cancelBefore = TimerProbe::cancelCalls, waitsBefore = TimerProbe::waitCalls;
            const auto elapsed = Elapsed([&] { waiter.WaitFor(milliseconds(1)); });
            std::cout << (fault == TimerProbe::Fault::WaitFailed ? "wait_failed" : "wait_timeout")
                << "_fallback_ms=" << elapsed << '\n';
            Check(TimerProbe::cancelCalls == cancelBefore + 1 && TimerProbe::waitCalls == waitsBefore + 1,
                "Failed or timed-out wait cancels its timer without retrying the native wait");
        }
    }
    Check(TimerProbe::createdHandles == TimerProbe::closeCalls && TimerProbe::failedCloses == 0,
        "All success and failure paths retain exclusive handle ownership");
    std::cout << "PASS: " << checks << " short-wait checks; timing is informational; no DMA or GUI initialized\n";
}
