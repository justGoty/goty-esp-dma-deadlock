#include "pch.h"
#include "GUI/Main Window/Main Window.h"
#include "DMA/DMA Thread.h"
#include "GUI/Config/Config.h"
#include "Makcu/MyMakcu.h"
#include "Diagnostics/RuntimeOptions.h"
#include "Diagnostics/StopSignal.h"
#include <charconv>
#include "GUI/Query.h"
#include "GUI/Fuser/Fuser.h"
#include "GUI/Main Menu/Main Menu.h"
#include "GUI/Fuser/ESP/ESP.h"
#include "GUI/Fuser/ESP/Draw/Players.h"
#include "GUI/Fuser/ESP/Draw/PlayerBoxGeometry.h"
#include "GUI/FramePacer.h"
#include "Timing/DisplayAwake.h"
#include "imgui_internal.h"

namespace {
void VerifyGuiOutput() {
    const auto* window = ImGui::FindWindowByName("Fuser");
    const auto index = Fuser::GetOutputMonitorIndex();
    const auto& monitors = ImGui::GetPlatformIO().Monitors;
    if (!window || !window->Viewport || index < 0 || index >= monitors.Size)
        throw std::runtime_error("GUI smoke: Fuser viewport or monitor missing");
    const auto hwnd = static_cast<HWND>(window->Viewport->PlatformHandleRaw ? window->Viewport->PlatformHandleRaw : window->Viewport->PlatformHandle);
    RECT client{}; POINT origin{};
    if (!hwnd || hwnd == MainWindow::g_hWnd || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
        throw std::runtime_error("GUI smoke: Fuser must have a separate native window");
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const auto& monitor = monitors[index];
    if ((style & (WS_CAPTION | WS_THICKFRAME)) || !(style & WS_POPUP) || window->DockId != 0 ||
        client.right != static_cast<LONG>(monitor.MainSize.x) || client.bottom != static_cast<LONG>(monitor.MainSize.y) ||
        origin.x != static_cast<LONG>(monitor.MainPos.x) || origin.y != static_cast<LONG>(monitor.MainPos.y))
        throw std::runtime_error("GUI smoke: borderless fullscreen geometry failed");
    if (ImGui::FindWindowByName("GOTY Diagnostics") && ImGui::FindWindowByName("GOTY Diagnostics")->Active)
        throw std::runtime_error("GUI smoke: detailed diagnostics should be closed by default");
    const auto* controls = ImGui::FindWindowByName("Controls");
    if (MainMenu::bControlsVisible && (!controls || controls->Size.x > 640 || controls->Size.y > 600))
        throw std::runtime_error("GUI smoke: controls should remain compact");
    if (!IsWindowVisible(hwnd)) throw std::runtime_error("GUI smoke: Fuser output must remain visible");
    Diagnostics::Event("info", "gui_smoke", std::format("Fuser separate=1 borderless=1 dock=0 pixels={}x{} origin={},{} monitor={}; no DMA initialized", client.right, client.bottom, origin.x, origin.y, index));
    std::println("PASS: GUI output {}x{}, borderless separate viewport; no DMA", client.right, client.bottom);
}
}
#ifdef CATCH_ENABLE
#include "Tests/All Tests.h"
#endif
std::atomic<bool> bRunning{true};
#ifndef CATCH_ENABLE
int main(int argc, char** argv) {
    // This control invocation opens only the named event; it never initializes
    // diagnostics, graphics or a second DMA client.
    if (argc > 1 && std::string_view(argv[1]) == "--stop") {
        DWORD targetPid{};
        if (argc != 3) { std::println("Usage: --stop <local PID>"); return 2; }
        const std::string_view value(argv[2]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), targetPid);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !targetPid) {
            std::println("Invalid local PID"); return 2;
        }
        if (!Diagnostics::StopSignal::Request(targetPid)) {
            std::println("No cooperative stop endpoint for local PID {}", targetPid); return 1;
        }
        std::println("Shutdown requested for local PID {}; wait for process exit before restarting", targetPid);
        return 0;
    }
    Diagnostics::StopSignal stopSignal;
    std::unique_ptr<DisplayAwake> displayAwake;
    std::thread worker, monitor;
    bool guiReady = false;
    try {
        std::vector<wchar_t> executable(32768);
        auto length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Could not resolve executable directory");
        // Resource and fallback-config paths now work from Explorer or any launch directory.
        auto directory = std::filesystem::path(executable.data()).parent_path();
        std::filesystem::current_path(directory);
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Diagnostics::Initialize(directory / "logs");
        Diagnostics::PublishHeartbeat();
        Diagnostics::Event("info", "build", std::format("GOTY Deadlock DMA {}; upstream d14f328; Release x64", Diagnostics::Version));
        Offsets::LogLayout();
        bool smoke = false, guiSmoke = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--enable-makcu") RuntimeOptions::EnableMakcu = true;
            else if (std::string_view(argv[i]) == "--diagnostics-smoke") smoke = true;
            else if (std::string_view(argv[i]) == "--gui-smoke") guiSmoke = true;
            else if (std::string_view(argv[i]) == "--diagnose-refresh") RuntimeOptions::DiagnoseRefresh = true;
            else if (std::string_view(argv[i]) == "--background-refresh") RuntimeOptions::ManagedRefresh = false;
            else throw std::runtime_error("Unknown argument; supported: --enable-makcu, --diagnostics-smoke, --gui-smoke, --diagnose-refresh, --background-refresh");
        }
        if (RuntimeOptions::DiagnoseRefresh || RuntimeOptions::EnableMakcu) RuntimeOptions::ManagedRefresh = false;
        if (smoke) {
            Diagnostics::Event("info", "diagnostics_smoke", "No GPU, config, DMA or hardware input initialized");
            Diagnostics::SetStage("stopped"); Diagnostics::Shutdown();
            std::println("PASS: diagnostics smoke; logs at {}", (directory / "logs").string()); return 0;
        }
        if (!guiSmoke) Config::LoadConfig("default");
        displayAwake = std::make_unique<DisplayAwake>();
        Diagnostics::Event(displayAwake->Valid() ? "info" : "warning", "display_awake",
            displayAwake->Valid() ? "Display and system idle sleep inhibited for this process lifetime; saved power plan unchanged" :
            "Windows rejected the display/system idle sleep request");
        Query::Publish();
#ifndef DEADLOCK_DLL
        if (!MainWindow::Initialize()) throw std::runtime_error("Main window / Direct3D initialization failed");
        guiReady = true;
#endif
        if (guiSmoke) {
            // Exercise a legacy profile with all unwanted categories enabled.
            Config::DeserializeConfig(nlohmann::json{{"Fuser", {
                {"ESP", {{"bMasterToggle", true}}},
                {"Draw_Troopers", {{"bMasterToggle", true}}},
                {"Draw_Camps", {{"bMasterToggle", true}}},
                {"Draw_Sinners", {{"bMasterToggle", true}}},
                {"bDrawSoulsPerMinute", true},
                {"StatusBars", {{"bMasterToggle", true}}}
            }}, {"Radar", {{"bMasterToggle", true}}}});
            Query::Publish();
            if (!ESP::bPlayersOnly || !Query::IsUsingPlayers() || Query::IsUsingTroopers() ||
                Query::IsUsingCamps() || Query::IsUsingSinners() || Query::IsUsingServerTime() ||
                Query::IsUsingBones() || Query::IsUsingVelocity() || Query::IsUsingPlayerSouls() ||
                !Query::IsUsingMinimalControllers() || !Draw_Players::bBoxOnly || !Draw_Players::bDrawBox ||
                !Config::SerializeConfig()["Fuser"]["ESP"]["bPlayersOnly"].get<bool>() ||
                !Config::SerializeConfig()["Fuser"]["Draw_Players"]["bBoxOnly"].get<bool>())
                throw std::runtime_error("GUI smoke: legacy config characters-only migration/query failed");
            Matrix44 boxCamera{}; boxCamera.m12 = .01f; boxCamera.m33 = 1.f;
            const auto box = PlayerBoxGeometry::Project(boxCamera, Vector3{}, 2560.f, 1440.f, 72.f, .45f);
            if (!box || std::abs(box->bottom - box->top - 518.4f) > .02f || Diagnostics::BoneReadRequests.load() != 0)
                throw std::runtime_error("GUI smoke: bone-free box projection/read policy failed");
            Diagnostics::Event("info", "gui_smoke_boxes", "Legacy box-only profile skips bones/velocity/souls; finite 2K box projection verified without DMA");
            Diagnostics::Event("info", "gui_smoke_players_only", "Legacy enabled NPC/HUD/radar config suppressed; only player read lane active");
            Fuser::bMasterToggle = true; Fuser::bFullscreen = true; Fuser::MonitorIndex = -1;
            for (int frame = 0; frame < 60; ++frame) {
                if (!MainWindow::OnFrame()) throw std::runtime_error("GUI smoke: frame failed");
                Diagnostics::FrameTick();
            }
            VerifyGuiOutput();
            MainMenu::bControlsVisible = false;
            ShowWindow(MainWindow::g_hWnd, SW_HIDE);
            for (int frame = 0; frame < 30; ++frame)
                if (!MainWindow::OnFrame()) throw std::runtime_error("GUI smoke: hidden controls frame failed");
            VerifyGuiOutput();
            Diagnostics::Event("info", "gui_smoke_hidden_controls", "Fuser remains visible and fullscreen while control HWND is hidden");
            bRunning = false;
        }
        if (!guiSmoke && RuntimeOptions::EnableMakcu) {
            auto library = directory / "makcu-cpp.dll";
            if (!LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                throw std::runtime_error("Requested Makcu DLL missing or incompatible: place matching x64 makcu-cpp.dll next to EXE");
            if (!MyMakcu::Initialize()) throw std::runtime_error("Requested Makcu device is unavailable");
        }
        if (!guiSmoke) monitor = std::thread([] {
            while (bRunning) {
                Diagnostics::PublishHeartbeat();
                for (int i = 0; i < 10 && bRunning; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        if (!guiSmoke) worker = std::thread(DMA_Thread_Main);
#ifndef DEADLOCK_DLL
        FramePacer framePacer;
#endif
        while (bRunning) {
            if (stopSignal.Requested()) {
                Diagnostics::Event("info", "stop_requested", "Cooperative local stop signal");
                bRunning = false; break;
            }
#ifndef DEADLOCK_DLL
            framePacer.BeginFrame();
#endif
            if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
                Diagnostics::Event("info", "stop_requested", "Delete hotkey on the local computer");
                bRunning = false; break;
            }
#ifndef DEADLOCK_DLL
            if (!MainWindow::OnFrame()) {
                if (MainWindow::g_FatalError) { RuntimeOptions::ExitCode = 1; Diagnostics::SetStage("failed"); }
                bRunning = false; break;
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
            Diagnostics::FrameTick(); FrameMark;
#ifndef DEADLOCK_DLL
            // The secondary Fuser viewport keeps rendering when Controls is
            // hidden and its main swap chain no longer blocks on VSync.
            if (bRunning) framePacer.WaitForNextFrame(MainMenu::iTargetFPS);
#endif
        }
    } catch (const std::exception& e) {
        RuntimeOptions::ExitCode = 1; Diagnostics::Event("error", "main_exception", e.what()); Diagnostics::SetStage("failed");
    } catch (...) {
        RuntimeOptions::ExitCode = 1; Diagnostics::Event("error", "main_exception", "Unknown exception"); Diagnostics::SetStage("failed");
    }
    bRunning = false;
    Diagnostics::Event("info", "cleanup_begin");
    if (worker.joinable()) worker.join();
    Diagnostics::Event("info", "worker_joined");
    if (monitor.joinable()) monitor.join();
    Diagnostics::Event("info", "monitor_joined");
#ifndef DEADLOCK_DLL
    if (guiReady) MainWindow::Cleanup();
    Diagnostics::Event("info", "gui_cleanup_complete");
#endif
    displayAwake.reset();
    if (!RuntimeOptions::ExitCode) Diagnostics::SetStage("stopped");
    Diagnostics::Shutdown();
    return RuntimeOptions::ExitCode.load();
}
#endif
#ifdef DEADLOCK_DLL
DWORD WINAPI StartingThread(HMODULE) { return main(0, nullptr); }
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        auto thread = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(StartingThread), module, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
#endif
