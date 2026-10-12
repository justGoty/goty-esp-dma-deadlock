#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vmmdll.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "Diagnostics/Diagnostics.h"

// This guard binds only after the caller has selected a unique active process
// using a freshly refreshed process table. It never discovers or reattaches a
// process. Call Probe on the worker; the caller separately expires GUI freshness.
class TargetLiveness final {
public:
    enum class Result { Alive, Exited, Unavailable, IdentityChanged };

    bool Initialize(VMM_HANDLE vmm, DWORD pid) {
        ready_ = false;
        terminal_ = Result::Alive;
        reason_.clear();
        vmm_ = vmm;
        pid_ = pid;
        eprocess_ = 0;
        layout_ = {};
        baseline_ = {};
        buffer_.clear();
        if (!vmm_ || !pid_) return Fail("invalid_vmm_or_pid");

        VMMDLL_PROCESS_INFORMATION process{};
        process.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
        process.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
        SIZE_T bytes = sizeof(process);
        if (!VMMDLL_ProcessGetInformation(vmm_, pid_, &process, &bytes))
            return Fail("selected_process_information_unavailable");
        if (bytes != sizeof(process) || process.magic != VMMDLL_PROCESS_INFORMATION_MAGIC ||
            process.wVersion != VMMDLL_PROCESS_INFORMATION_VERSION || process.wSize != sizeof(process))
            return Fail("selected_process_information_layout_mismatch");
        if (process.tpSystem != VMMDLL_SYSTEM_WINDOWS_64 || process.tpMemoryModel != VMMDLL_MEMORYMODEL_X64)
            return Fail("requires_windows_x64_kernel_layout");
        if (process.dwPID != pid_ || process.dwState != 0)
            return Fail("selected_process_not_active_or_pid_mismatch");
        if (!KernelAddress(process.win.vaEPROCESS)) return Fail("invalid_eprocess_address");
        if (!ValidDtb(process.paDTB)) return Fail("invalid_process_information_dtb");
        eprocess_ = process.win.vaEPROCESS;

        // MemProcFS maps the special module name "nt" to PDB_HANDLE_KERNEL for
        // the current acquisition session, rather than an arbitrary local PDB.
        if (!ResolveLayout()) return false;
        if (eprocess_ > (std::numeric_limits<ULONG64>::max)() - layout_.eprocessSize)
            return Fail("eprocess_address_range_overflow");
        buffer_.resize(layout_.readBytes);

        Values first{}, second{};
        if (!Read(first) || !ValidateInitial(first, process.paDTB)) return false;
        if (!Read(second) || !ValidateInitial(second, process.paDTB)) return false;
        if (first.pid != second.pid || first.created != second.created ||
            NormalizeDtb(first.dtb) != NormalizeDtb(second.dtb))
            return Fail("identity_changed_during_initialization");
        baseline_ = second;
        if (epoch_ == (std::numeric_limits<uint64_t>::max)()) return Fail("identity_epoch_exhausted");
        ++epoch_;
        ready_ = true;
        reason_ = "alive";
        const auto nameLength = strnlen_s(process.szNameLong, sizeof(process.szNameLong));
        Diagnostics::Event("info", "target_liveness_bound", std::format(
            "source=MemProcFS.current_kernel_pdb; module=nt; pid={}; selected_name={}; epoch={}; "
            "eprocess=0x{:X}; create_time={}; dtb=0x{:X}; eprocess_size={}; kprocess_size={}; "
            "offsets=UniqueProcessId:0x{:X},CreateTime:0x{:X},ExitTime:0x{:X},Pcb:0x{:X},DirectoryTableBase:0x{:X}; "
            "read_pid=4; read_flags=NOCACHE|NOPAGING; read_bytes={}",
            pid_, std::string_view(process.szNameLong, nameLength), epoch_, eprocess_, baseline_.created,
            NormalizeDtb(baseline_.dtb), layout_.eprocessSize, layout_.kprocessSize, layout_.pid,
            layout_.created, layout_.exited, layout_.pcb, layout_.kprocessDtb, layout_.readBytes));
        return true;
    }

    Result Probe() {
        if (!ready_) {
            if (reason_.empty()) reason_ = "not_initialized";
            return Result::Unavailable;
        }
        // Terminal identity loss cannot be reversed by a later cached/stale
        // read. Only an explicit caller-authorized new selection may rebind.
        if (terminal_ != Result::Alive) return terminal_;
        Values current{};
        if (!Read(current)) return Result::Unavailable;
        if (current.pid != baseline_.pid) return Lost("unique_process_id_changed");
        if (current.created != baseline_.created) return Lost("create_time_changed");
        if (!ValidDtb(current.dtb) || NormalizeDtb(current.dtb) != NormalizeDtb(baseline_.dtb))
            return Lost("directory_table_base_changed");
        if (current.exited != 0) {
            terminal_ = Result::Exited;
            reason_ = "exit_time_nonzero";
            return terminal_;
        }
        reason_ = "alive";
        return Result::Alive;
    }

    // Ready means a baseline exists, not that its latest probe is alive.
    bool Ready() const noexcept { return ready_; }
    uint64_t Epoch() const noexcept { return epoch_; }
    const std::string& Reason() const noexcept { return reason_; }

private:
    struct Layout {
        DWORD eprocessSize{}, kprocessSize{}, pid{}, created{}, exited{}, pcb{}, kprocessDtb{};
        DWORD dtb{}, first{}, readBytes{};
    } layout_;
    struct Values { ULONG64 pid{}, created{}, exited{}, dtb{}; } baseline_;
    static constexpr DWORD ValueBytes = sizeof(ULONG64);
    // Sanity cap only: every actual type size and field offset comes from PDB.
    static constexpr DWORD MaximumTypeBytes = 64 * 1024;
    VMM_HANDLE vmm_{};
    DWORD pid_{};
    ULONG64 eprocess_{};
    uint64_t epoch_{};
    bool ready_{};
    Result terminal_{Result::Alive};
    std::string reason_;
    std::vector<BYTE> buffer_;

    bool Fail(std::string reason) { reason_ = std::move(reason); return false; }
    Result Lost(const char* reason) {
        reason_ = reason;
        terminal_ = Result::IdentityChanged;
        return terminal_;
    }
    static bool Contains(DWORD size, DWORD offset, DWORD bytes) noexcept {
        return offset <= size && bytes <= size - offset;
    }
    static bool Overlaps(DWORD left, DWORD leftBytes, DWORD right, DWORD rightBytes) noexcept {
        return static_cast<uint64_t>(left) < static_cast<uint64_t>(right) + rightBytes &&
            static_cast<uint64_t>(right) < static_cast<uint64_t>(left) + leftBytes;
    }
    static bool KernelAddress(ULONG64 value) noexcept {
        return (value >> 47) == 0x1ffff && (value & (ValueBytes - 1)) == 0;
    }
    static ULONG64 NormalizeDtb(ULONG64 value) noexcept { return value & ~ULONG64{0xfff}; }
    static bool ValidDtb(ULONG64 value) noexcept {
        // x64 CR3 is a physical page base with optional low PCID bits. Reject
        // zero and values beyond the architecture's 52-bit physical limit.
        return NormalizeDtb(value) != 0 && (value >> 52) == 0;
    }
    bool TypeSize(const char* type, DWORD& size) {
        if (!VMMDLL_PdbTypeSize(vmm_, "nt", type, &size))
            return Fail(std::format("current_kernel_pdb_type_unavailable:{}", type));
        if (size < ValueBytes || size > MaximumTypeBytes || size % ValueBytes != 0)
            return Fail(std::format("invalid_pdb_type_size:{}", type));
        return true;
    }
    bool Field(const char* type, const char* field, DWORD& offset) {
        if (!VMMDLL_PdbTypeChildOffset(vmm_, "nt", type, field, &offset))
            return Fail(std::format("current_kernel_pdb_field_unavailable:{}.{}", type, field));
        return true;
    }
    bool ResolveLayout() {
        if (!TypeSize("_EPROCESS", layout_.eprocessSize) || !TypeSize("_KPROCESS", layout_.kprocessSize) ||
            !Field("_EPROCESS", "UniqueProcessId", layout_.pid) ||
            !Field("_EPROCESS", "CreateTime", layout_.created) ||
            !Field("_EPROCESS", "ExitTime", layout_.exited) ||
            !Field("_EPROCESS", "Pcb", layout_.pcb) ||
            !Field("_KPROCESS", "DirectoryTableBase", layout_.kprocessDtb)) return false;
        if (!Contains(layout_.eprocessSize, layout_.pcb, layout_.kprocessSize) ||
            !Contains(layout_.kprocessSize, layout_.kprocessDtb, ValueBytes) ||
            layout_.pcb % ValueBytes != 0 || layout_.kprocessDtb % ValueBytes != 0)
            return Fail("invalid_pdb_pcb_or_dtb_range");
        layout_.dtb = layout_.pcb + layout_.kprocessDtb;
        const std::array<DWORD, 3> fields{layout_.pid, layout_.created, layout_.exited};
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!Contains(layout_.eprocessSize, fields[i], ValueBytes) || fields[i] % ValueBytes != 0 ||
                Overlaps(fields[i], ValueBytes, layout_.pcb, layout_.kprocessSize))
                return Fail("invalid_pdb_identity_field_range");
            for (size_t j = 0; j < i; ++j)
                if (Overlaps(fields[i], ValueBytes, fields[j], ValueBytes))
                    return Fail("overlapping_pdb_identity_fields");
        }
        layout_.first = (std::min)({layout_.dtb, layout_.pid, layout_.created, layout_.exited});
        const auto last = (std::max)({layout_.dtb, layout_.pid, layout_.created, layout_.exited});
        layout_.readBytes = last + ValueBytes - layout_.first;
        return true;
    }
    bool Read(Values& values) {
        DWORD actual{};
        const bool read = VMMDLL_MemReadEx(vmm_, 4, eprocess_ + layout_.first, buffer_.data(),
            layout_.readBytes, &actual, VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING) != FALSE;
        Diagnostics::RecordRead(layout_.readBytes, actual, read);
        if (!read || actual != layout_.readBytes)
            return Fail(std::format("kernel_identity_read_incomplete:success={},expected={},actual={}",
                read, layout_.readBytes, actual));
        const auto value = [&](DWORD offset) {
            ULONG64 result{};
            std::memcpy(&result, buffer_.data() + offset - layout_.first, sizeof(result));
            return result;
        };
        values = {value(layout_.pid), value(layout_.created), value(layout_.exited), value(layout_.dtb)};
        return true;
    }
    bool ValidateInitial(const Values& values, ULONG64 processDtb) {
        if (values.pid != pid_) return Fail("initial_unique_process_id_mismatch");
        if (!values.created || (values.created >> 63) != 0) return Fail("initial_create_time_invalid");
        if (values.exited != 0) return Fail("initial_exit_time_nonzero");
        if (!ValidDtb(values.dtb) || NormalizeDtb(values.dtb) != NormalizeDtb(processDtb))
            return Fail("initial_directory_table_base_mismatch");
        return true;
    }
};
