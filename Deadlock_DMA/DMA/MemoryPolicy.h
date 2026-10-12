#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace MemoryPolicy {
inline std::optional<size_t> FindPattern(std::span<const uint8_t> buffer, std::string_view pattern, std::string_view mask) {
    if (pattern.empty() || pattern.size() != mask.size() || pattern.size() > buffer.size()) return {};
    for (char c : mask) if (c != 'x' && c != '?') return {};
    for (size_t i = 0; i <= buffer.size() - pattern.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (mask[j] != '?' && buffer[i + j] != static_cast<uint8_t>(pattern[j])) { match = false; break; }
        }
        if (match) return i;
    }
    return {};
}
inline bool ValidEntityHandle(uint32_t value, size_t capacity) {
    return value != UINT32_MAX && (value & 0x7fff) < capacity;
}
inline std::optional<ptrdiff_t> RipRelative(size_t instruction, size_t length, int32_t displacement) {
    const auto target = static_cast<int64_t>(instruction) + static_cast<int64_t>(length) + displacement;
    if (target <= 0 || target > PTRDIFF_MAX) return {};
    return static_cast<ptrdiff_t>(target);
}
}
