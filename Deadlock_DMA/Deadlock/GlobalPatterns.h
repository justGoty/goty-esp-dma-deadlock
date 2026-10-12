#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace GlobalPatterns {
struct RipSignature {
    std::string_view bytes, mask;
    size_t displacementOffset, instructionLength;
};
// affectioned/Deadlock_DMA e127620518e97dc6d765a914cfbfaa325929f8d4 (2026-10-02).
// Both LEA targets are objects inside client.dll, not slots containing object pointers.
inline constexpr RipSignature ViewMatrix{
    "\x48\x8D\x0D????\x48\xC1\xE0\x06", "xxx????xxxx", 3, 7};
inline constexpr RipSignature PredictionInstance{
    "\x48\x8D\x05????\xC3\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\x40\x53\x56\x41\x54",
    "xxx????xxxxxxxxxxxxxx", 3, 7};

inline uintptr_t PredictionInstanceAddress(uintptr_t moduleBase, ptrdiff_t rva) noexcept {
    if (!moduleBase || rva <= 0 || static_cast<uintptr_t>(rva) > std::numeric_limits<uintptr_t>::max() - moduleBase)
        return 0;
    return moduleBase + static_cast<uintptr_t>(rva);
}
}
