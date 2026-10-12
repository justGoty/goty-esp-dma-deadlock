#pragma once
#include "vmmdll.h"
#include "Diagnostics/RuntimeOptions.h"

inline DWORD GameReadFlags() noexcept {
    // Managed sessions retain the selected process object. Do not consult its
    // potentially older VAD/paging metadata for a page that is no longer resident.
    return VMMDLL_FLAG_NOCACHE | (RuntimeOptions::ResidentGameReads ? VMMDLL_FLAG_NOPAGING : 0);
}
