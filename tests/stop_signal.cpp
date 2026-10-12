#include "Diagnostics/StopSignal.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

int checks{};
void Check(bool ok, const char* reason) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << reason << '\n'; std::exit(1); }
}

std::wstring ReadyName(DWORD parentPid) {
    return L"Local\\GOTYDeadlockDMA.StopTest.Ready." + std::to_wstring(parentPid);
}

int Child(DWORD parentPid) {
    Diagnostics::StopSignal signal;
    const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ReadyName(parentPid).c_str());
    if (!ready) return 2;
    const bool announced = SetEvent(ready) != FALSE;
    CloseHandle(ready);
    if (!announced) return 3;
    // Test helpers terminate themselves on timeout; no process is killed.
    const auto deadline = GetTickCount64() + 5'000;
    do {
        if (signal.Requested()) return 0;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    return 4;
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--child")
        return Child(static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)));

    static_assert(!std::is_copy_constructible_v<Diagnostics::StopSignal>);
    static_assert(!std::is_move_constructible_v<Diagnostics::StopSignal>);
    const auto pid = GetCurrentProcessId();
    Check(!Diagnostics::StopSignal::Request(0), "PID zero is rejected");
    Check(!Diagnostics::StopSignal::Request(pid), "A nonexistent event is not created by Request");
    DWORD handlesBefore{}, handlesAfter{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), "Read baseline handle count");
    {
        Diagnostics::StopSignal signal;
        Check(!signal.Requested(), "New event starts unsignalled");
        Check(Diagnostics::StopSignal::Request(pid), "Request opens and signals the owning instance");
        Check(signal.Requested() && signal.Requested(), "Manual-reset event remains requested across polls");
        Check(Diagnostics::StopSignal::Request(pid) && signal.Requested(), "Repeated requests are harmless");
    }
    Check(!Diagnostics::StopSignal::Request(pid), "Event is removed when its last owner closes");
    for (int i = 0; i < 500; ++i) {
        Diagnostics::StopSignal signal;
        Check(!signal.Requested() && Diagnostics::StopSignal::Request(pid) && signal.Requested(),
            "Each new lifetime resets state and closes request handles");
    }
    Check(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter) && handlesBefore == handlesAfter,
        "500 event/request lifetimes leak no handles");

    const auto ready = CreateEventW(nullptr, TRUE, FALSE, ReadyName(pid).c_str());
    Check(ready != nullptr, "Create isolated child readiness event");
    std::wstring executable(32'768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(length > 0 && length < executable.size(), "Resolve this fixture executable");
    executable.resize(length);
    auto command = L"\"" + executable + L"\" --child " + std::to_wstring(pid);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &child) != FALSE, "Launch only the isolated offline test helper");
    CloseHandle(child.hThread);
    const bool childReady = WaitForSingleObject(ready, 5'000) == WAIT_OBJECT_0;
    CloseHandle(ready);
    DWORD beforeRequestHandles{}, afterRequestHandles{};
    const bool beforeRequestRead = GetProcessHandleCount(GetCurrentProcess(), &beforeRequestHandles) != FALSE;
    const bool requested = childReady && Diagnostics::StopSignal::Request(child.dwProcessId);
    const bool afterRequestRead = GetProcessHandleCount(GetCurrentProcess(), &afterRequestHandles) != FALSE;
    const bool childExited = WaitForSingleObject(child.hProcess, 8'000) == WAIT_OBJECT_0;
    DWORD childCode{};
    const bool exitRead = GetExitCodeProcess(child.hProcess, &childCode) != FALSE;
    CloseHandle(child.hProcess);
    Check(childReady, "Child creates its own stop event before signalling readiness");
    Check(requested, "Parent can request the child PID within the same Windows session");
    Check(childExited && exitRead && childCode == 0, "Child observes the request and exits cooperatively");
    Check(!Diagnostics::StopSignal::Request(child.dwProcessId), "Exited child leaves no stop event behind");
    // Compare the request itself: Windows may cache internal process-launch
    // handles during the fixture's first CreateProcess call.
    Check(beforeRequestRead && afterRequestRead && beforeRequestHandles == afterRequestHandles,
        "Cross-process request closes its local event handle");
    std::cout << "PASS: " << checks << " stop-signal checks; no runtime, DMA, UI, or process termination\n";
}
