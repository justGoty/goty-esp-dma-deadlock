#include "Diagnostics/Diagnostics.h"
#include <thread>
#include <chrono>
#include <string_view>
#include <fstream>
#include <json.hpp>
#include <cstdio>
#include <windows.h>

// Synthetic process for the monitoring integration test. Never reads target memory.
int main(int argc, char** argv) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    Diagnostics::Initialize(std::filesystem::path(executable).parent_path() / "logs");
    const std::string_view mode = argc > 1 ? argv[1] : "";
    const bool legacy = mode == "legacy";
    const bool connecting = mode == "connecting" || legacy;
    Diagnostics::SetStage(connecting ? "connecting_dma" : "running", connecting ? 0 : 123);
    bool stale = mode == "stale";
    if (!connecting) Diagnostics::WorkerTick(0);
    const int ticks = legacy ? 40 : connecting ? 60 : 100;
    for (int i = 0; i < ticks; ++i) {
        if (!stale && !connecting) Diagnostics::WorkerTick(0);
        Diagnostics::FrameTick();
        if (legacy) {
            const auto snapshot = Diagnostics::GetSnapshot();
            const auto path = std::filesystem::path(snapshot.logDirectory) / (snapshot.session + ".status.json");
            SYSTEMTIME time{}; GetSystemTime(&time); char utc[40]{};
            std::snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
            nlohmann::json record{{"schema_version", 1}, {"version", "0.1.1"}, {"utc", utc}, {"session", snapshot.session},
                {"local_pid", GetCurrentProcessId()}, {"stage", snapshot.stage}, {"target_pid", 0}, {"worker_age_s", -1},
                {"frame_age_s", snapshot.frameAgeSeconds}, {"failed_reads", 0}, {"open_scatter", 0}, {"slow_loops", 0}};
            auto temp = path; temp += ".tmp";
            std::ofstream output(temp, std::ios::binary | std::ios::trunc); output << record.dump(); output.close();
            MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        } else Diagnostics::PublishHeartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Diagnostics::SetStage("stopped"); Diagnostics::Shutdown();
}
