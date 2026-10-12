#pragma once
#include <atomic>
namespace RuntimeOptions {
// Published before worker startup. Hardware input stays opt-in.
inline bool EnableMakcu = false;
// Bounded backend A/B measurement; normal startup keeps refresh defaults.
inline bool DiagnoseRefresh = false;
inline bool ManagedRefresh = true;
// Enabled by the worker only after startup module/signature discovery.
inline bool ResidentGameReads = false;
inline std::atomic<int> ExitCode{};
}
