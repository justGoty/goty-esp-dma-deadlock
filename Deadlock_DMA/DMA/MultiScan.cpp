#include "pch.h"
#include "MultiScan.h"
#include "MemoryPolicy.h"
#include "Diagnostics/Diagnostics.h"
bool MultiScan::Initialize(VMM_HANDLE vmh, DWORD pid, std::string name) {
    Close(); m_ModuleBase = VMMDLL_ProcessGetModuleBaseU(vmh, pid, name.c_str());
    if (!m_ModuleBase) { Diagnostics::Warning("scan_module_missing", name); return false; }
    DWORD count{};
    if (!VMMDLL_ProcessGetSectionsU(vmh, pid, name.c_str(), nullptr, 0, &count) || !count || count > 96) {
        Diagnostics::Warning("scan_sections_failed", name); return false;
    }
    auto headers = std::make_unique<IMAGE_SECTION_HEADER[]>(count);
    if (!VMMDLL_ProcessGetSectionsU(vmh, pid, name.c_str(), headers.get(), count, &count)) return false;
    for (DWORD i = 0; i < count; ++i) {
        const auto& section = headers[i]; if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        DWORD size = section.Misc.VirtualSize;
        if (!size || size > 128 * 1024 * 1024) { Diagnostics::Warning("scan_section_invalid", name); return false; }
        auto data = std::make_unique<BYTE[]>(size); DWORD total{};
        // Each retry starts at cumulative progress, never the previous chunk size.
        for (int attempt = 0; total < size && attempt < 100; ++attempt) {
            DWORD read{}; DWORD remaining = size - total;
            bool ok = VMMDLL_MemReadEx(vmh, pid, m_ModuleBase + section.VirtualAddress + total,
                data.get() + total, remaining, &read, VMMDLL_FLAG_NOCACHE);
            Diagnostics::RecordRead(remaining, read, ok);
            if (!read || read > remaining) break;
            total += read;
        }
        if (total != size) { Diagnostics::Warning("scan_section_incomplete", std::format("{} bytes={}/{}", name, total, size)); return false; }
        m_pBuffer = std::move(data); m_SectionBase = section.VirtualAddress; m_BufferSize = size;
        Diagnostics::Event("info", "scan_section_ready", std::format("{} rva=0x{:X} bytes={}", name, m_SectionBase, size)); return true;
    }
    Diagnostics::Warning("scan_executable_section_missing", name); return false;
}
uintptr_t MultiScan::Scan(PatternInfo pattern) {
    auto offset = FindSectionOffset(pattern); return offset ? m_ModuleBase + m_SectionBase + *offset : 0;
}
uintptr_t MultiScan::ScanOffset(PatternInfo pattern) {
    auto offset = FindSectionOffset(pattern); return offset ? m_SectionBase + *offset : 0;
}
uintptr_t MultiScan::ScanSectionOffset(PatternInfo pattern) {
    auto offset = FindSectionOffset(pattern); return offset.value_or(SIZE_MAX); // zero is a valid section offset
}
std::optional<size_t> MultiScan::FindSectionOffset(const PatternInfo& pattern) const {
    auto result = MemoryPolicy::FindPattern({m_pBuffer.get(), m_BufferSize}, pattern.Pattern, pattern.Mask);
    if (!result) { Diagnostics::Warning("pattern_not_found", pattern.Mask); return {}; }
    auto remaining = std::span<const uint8_t>(m_pBuffer.get(), m_BufferSize).subspan(*result + 1);
    if (MemoryPolicy::FindPattern(remaining, pattern.Pattern, pattern.Mask)) {
        Diagnostics::Warning("pattern_ambiguous", pattern.Mask); return {};
    }
    return result;
}
bool MultiScan::Close() { m_pBuffer.reset(); m_BufferSize = 0; m_ModuleBase = 0; m_SectionBase = 0; return true; }
BYTE* MultiScan::GetBuffer() { return m_pBuffer.get(); }
uintptr_t MultiScan::GetModuleBase() { return m_ModuleBase; }
uintptr_t MultiScan::GetSectionBase() { return m_SectionBase; }
