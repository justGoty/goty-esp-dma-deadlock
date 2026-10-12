#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

class CEntityListEntry
{
public:
	uintptr_t pEnt; //0x0000
	char pad_0008[24]; //0x0008
	uintptr_t pName; //0x0020
	char pad_0028[72]; //0x0028
}; //Size: 0x0070
static_assert(sizeof(CEntityListEntry) == 0x70);
static_assert(std::is_standard_layout_v<CEntityListEntry> && std::is_trivially_copyable_v<CEntityListEntry>);
static_assert(offsetof(CEntityListEntry, pEnt) == 0x00 && offsetof(CEntityListEntry, pName) == 0x20);
static_assert(sizeof(CEntityListEntry::pEnt) == 8 && sizeof(CEntityListEntry::pName) == 8);

namespace EntityListLayout {
// Consumers use pEnt for handle lookup and pEnt/pName for class discovery.
// Keep the full remote stride between entries, but do not require resident
// padding after the last pName just to publish an otherwise complete list.
inline constexpr size_t RequiredEntryBytes = (std::max)(
    offsetof(CEntityListEntry, pEnt) + sizeof(CEntityListEntry::pEnt),
    offsetof(CEntityListEntry, pName) + sizeof(CEntityListEntry::pName));
static_assert(RequiredEntryBytes <= sizeof(CEntityListEntry));

inline constexpr size_t ReadBytes(size_t entryCount) noexcept {
    if (!entryCount || entryCount - 1 > ((std::numeric_limits<size_t>::max)() - RequiredEntryBytes) / sizeof(CEntityListEntry))
        return 0;
    return (entryCount - 1) * sizeof(CEntityListEntry) + RequiredEntryBytes;
}
}
