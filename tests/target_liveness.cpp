#include "DMA/TargetLiveness.h"
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>

namespace Fake {
constexpr DWORD Pid = 23456;
constexpr ULONG64 Eprocess = 0xffffb00123456000;
constexpr ULONG64 Dtb = 0x12345000;
constexpr ULONG64 Created = 133000000000000000;
VMM_HANDLE Handle() { return reinterpret_cast<VMM_HANDLE>(1); }
VMMDLL_PROCESS_INFORMATION process{};
SIZE_T infoBytes{};
std::map<std::string, DWORD> sizes;
std::map<std::pair<std::string, std::string>, DWORD> fields;
std::vector<BYTE> memory;
std::vector<std::tuple<std::string, std::string, std::string>> events;
std::vector<std::tuple<DWORD, DWORD, bool>> readRecords;
std::vector<std::tuple<DWORD, ULONG64, DWORD, ULONG64>> memoryCalls;
size_t infoCalls{}, typeCalls{}, fieldCalls{}, readCalls{}, failReadAt{}, shortReadAt{}, excessReadAt{};
bool failInfo{}, symbolsAvailable{};
std::function<void(size_t)> beforeRead;

DWORD Offset(const std::string& type, const std::string& field) { return fields.at({type, field}); }
void Put(DWORD offset, ULONG64 value) {
    if (offset > memory.size() || sizeof(value) > memory.size() - offset) throw std::runtime_error("bad fixture write");
    std::memcpy(memory.data() + offset, &value, sizeof(value));
}
void SetIdentity(ULONG64 pid = Pid, ULONG64 created = Created, ULONG64 exited = 0, ULONG64 dtb = Dtb) {
    Put(Offset("_EPROCESS", "UniqueProcessId"), pid);
    Put(Offset("_EPROCESS", "CreateTime"), created);
    Put(Offset("_EPROCESS", "ExitTime"), exited);
    Put(Offset("_EPROCESS", "Pcb") + Offset("_KPROCESS", "DirectoryTableBase"), dtb);
}
void Reset() {
    process = {};
    process.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    process.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
    process.wSize = sizeof(process);
    process.tpMemoryModel = VMMDLL_MEMORYMODEL_X64;
    process.tpSystem = VMMDLL_SYSTEM_WINDOWS_64;
    process.dwPID = Pid;
    process.paDTB = Dtb;
    process.win.vaEPROCESS = Eprocess;
    strcpy_s(process.szNameLong, "deadlock.exe");
    infoBytes = sizeof(process);
    sizes = {{"_EPROCESS", 0x180}, {"_KPROCESS", 0x60}};
    fields = {{{"_EPROCESS", "Pcb"}, 0x10}, {{"_KPROCESS", "DirectoryTableBase"}, 0x20},
        {{"_EPROCESS", "UniqueProcessId"}, 0x90}, {{"_EPROCESS", "CreateTime"}, 0xa8},
        {{"_EPROCESS", "ExitTime"}, 0xc0}};
    memory.assign(0x400, 0);
    infoCalls = typeCalls = fieldCalls = readCalls = failReadAt = shortReadAt = excessReadAt = 0;
    failInfo = false; symbolsAvailable = true; beforeRead = {};
    events.clear(); readRecords.clear(); memoryCalls.clear();
    SetIdentity();
}
}

extern "C" BOOL VMMDLL_ProcessGetInformation(VMM_HANDLE handle, DWORD pid,
    PVMMDLL_PROCESS_INFORMATION info, PSIZE_T size) {
    ++Fake::infoCalls;
    if (Fake::failInfo || handle != Fake::Handle() || pid != Fake::Pid || !info || !size ||
        info->magic != VMMDLL_PROCESS_INFORMATION_MAGIC || info->wVersion != VMMDLL_PROCESS_INFORMATION_VERSION ||
        *size != sizeof(*info)) return FALSE;
    *info = Fake::process; *size = Fake::infoBytes; return TRUE;
}
extern "C" BOOL VMMDLL_PdbTypeSize(VMM_HANDLE handle, LPCSTR module, LPCSTR type, PDWORD size) {
    ++Fake::typeCalls;
    if (handle != Fake::Handle() || std::string(module) != "nt" || !Fake::symbolsAvailable || !Fake::sizes.contains(type)) return FALSE;
    *size = Fake::sizes.at(type); return TRUE;
}
extern "C" BOOL VMMDLL_PdbTypeChildOffset(VMM_HANDLE handle, LPCSTR module, LPCSTR type, LPCSTR field, PDWORD offset) {
    ++Fake::fieldCalls;
    const auto key = std::make_pair(std::string(type), std::string(field));
    if (handle != Fake::Handle() || std::string(module) != "nt" || !Fake::symbolsAvailable || !Fake::fields.contains(key)) return FALSE;
    *offset = Fake::fields.at(key); return TRUE;
}
extern "C" BOOL VMMDLL_MemReadEx(VMM_HANDLE handle, DWORD pid, ULONG64 address, PBYTE buffer,
    DWORD bytes, PDWORD actual, ULONG64 flags) {
    ++Fake::readCalls;
    Fake::memoryCalls.emplace_back(pid, address, bytes, flags);
    *actual = 0;
    if (Fake::beforeRead) Fake::beforeRead(Fake::readCalls);
    if (handle != Fake::Handle() || pid != 4 || flags != (VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING) ||
        address < Fake::Eprocess || address - Fake::Eprocess > Fake::memory.size() ||
        bytes > Fake::memory.size() - (address - Fake::Eprocess)) return FALSE;
    std::memcpy(buffer, Fake::memory.data() + (address - Fake::Eprocess), bytes);
    *actual = Fake::readCalls == Fake::shortReadAt ? bytes - 1 :
        Fake::readCalls == Fake::excessReadAt ? bytes + 1 : bytes;
    return Fake::readCalls == Fake::failReadAt ? FALSE : TRUE;
}
namespace Diagnostics {
void Event(std::string_view level, std::string_view event, std::string_view detail) noexcept {
    try { Fake::events.emplace_back(level, event, detail); } catch (...) {}
}
void RecordRead(uint32_t expected, uint32_t actual, bool ok) noexcept {
    try { Fake::readRecords.emplace_back(expected, actual, ok); } catch (...) {}
}
}

namespace {
unsigned checks{};
using Result = TargetLiveness::Result;
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
void CheckReadContract() {
    for (const auto& [pid, address, bytes, flags] : Fake::memoryCalls) {
        Check(pid == 4 && flags == (VMMDLL_FLAG_NOCACHE | VMMDLL_FLAG_NOPAGING), "Every read uses kernel PID with resident uncached flags");
        Check(address >= Fake::Eprocess && bytes <= 64 * 1024 && bytes > 0, "Every read has a bounded field span");
    }
    Check(Fake::memoryCalls.size() == Fake::readRecords.size(), "Every kernel read reports exact-byte diagnostics");
}
void NormalAndDifferentLayout() {
    Fake::Reset();
    TargetLiveness guard;
    Check(guard.Probe() == Result::Unavailable && !guard.Ready() && guard.Epoch() == 0, "Uninitialized guard cannot report alive");
    Check(guard.Initialize(Fake::Handle(), Fake::Pid) && guard.Ready() && guard.Epoch() == 1, "Valid matching baseline establishes epoch");
    Check(Fake::readCalls == 2, "Initialization requires two stable uncached identity samples");
    for (int i = 0; i < 10; ++i) Check(guard.Probe() == Result::Alive, "Fresh matching resident identity remains alive");
    Check(Fake::infoCalls == 1 && Fake::typeCalls == 2 && Fake::fieldCalls == 5, "Probe never enumerates processes or resolves PDB again");
    Check(Fake::events.size() == 1 && std::get<1>(Fake::events[0]) == "target_liveness_bound" &&
        std::get<2>(Fake::events[0]).find("source=MemProcFS.current_kernel_pdb") != std::string::npos,
        "Successful bind logs current-kernel symbol provenance once");
    CheckReadContract();

    Fake::Reset();
    Fake::sizes["_EPROCESS"] = 0x200; Fake::sizes["_KPROCESS"] = 0x80;
    Fake::fields[{"_EPROCESS", "Pcb"}] = 0x28;
    Fake::fields[{"_KPROCESS", "DirectoryTableBase"}] = 0x40;
    Fake::fields[{"_EPROCESS", "UniqueProcessId"}] = 0x100;
    Fake::fields[{"_EPROCESS", "CreateTime"}] = 0x108;
    Fake::fields[{"_EPROCESS", "ExitTime"}] = 0x178;
    Fake::memory.assign(0x400, 0); Fake::SetIdentity();
    Check(guard.Initialize(Fake::Handle(), Fake::Pid) && guard.Epoch() == 2 && guard.Probe() == Result::Alive,
        "Different PDB layout and nonzero Pcb work without built-in field offsets");
    CheckReadContract();
}
void ReadFailuresDoNotRebind() {
    for (int failure = 0; failure < 3; ++failure) {
        Fake::Reset(); TargetLiveness guard;
        Check(guard.Initialize(Fake::Handle(), Fake::Pid), "Read-failure fixture binds");
        if (failure == 0) Fake::failReadAt = 3;
        if (failure == 1) Fake::shortReadAt = 3;
        if (failure == 2) Fake::excessReadAt = 3;
        Check(guard.Probe() == Result::Unavailable && guard.Reason().find("kernel_identity_read_incomplete") == 0,
            "Failed, short, or excess-byte read never reports alive");
        Check(guard.Epoch() == 1 && Fake::infoCalls == 1, "Unavailable does not adopt a new cached process baseline");
        Check(guard.Probe() == Result::Alive && guard.Epoch() == 1, "A later complete matching read recovers within the same epoch");
    }
}
void TerminalIdentityLoss() {
    for (int mutation = 0; mutation < 5; ++mutation) {
        Fake::Reset(); TargetLiveness guard;
        Check(guard.Initialize(Fake::Handle(), Fake::Pid), "Identity-loss fixture binds");
        if (mutation == 0) Fake::SetIdentity(Fake::Pid + 1);
        if (mutation == 1) Fake::SetIdentity(Fake::Pid, Fake::Created + 1);
        if (mutation == 2) Fake::SetIdentity(Fake::Pid, Fake::Created, 0, Fake::Dtb + 0x1000);
        if (mutation == 3) Fake::SetIdentity(Fake::Pid, Fake::Created, 0, 0);
        if (mutation == 4) Fake::SetIdentity(0, 0, 0, 0);
        Check(guard.Probe() == Result::IdentityChanged, "PID, creation, DTB change or zeroed memory invalidates identity");
        const auto reads = Fake::readCalls;
        Fake::SetIdentity();
        Check(guard.Probe() == Result::IdentityChanged && Fake::readCalls == reads && guard.Epoch() == 1,
            "Identity loss is latched despite later restored bytes and never auto-rebinds");
    }
    Fake::Reset(); TargetLiveness guard;
    Check(guard.Initialize(Fake::Handle(), Fake::Pid), "Exit fixture binds");
    Fake::SetIdentity(Fake::Pid, Fake::Created, Fake::Created + 100);
    Check(guard.Probe() == Result::Exited, "Nonzero ExitTime marks a matching identity exited");
    Fake::SetIdentity();
    Check(guard.Probe() == Result::Exited, "Exited identity stays terminal until explicit fresh selection");
    Check(guard.Initialize(Fake::Handle(), Fake::Pid) && guard.Epoch() == 2, "Explicit new caller selection creates the next epoch");
    Fake::SetIdentity(Fake::Pid, Fake::Created, 0, Fake::Dtb | 0xabc);
    Check(guard.Probe() == Result::Alive, "Only low twelve CR3 PCID bits are ignored");
}
void RejectedInitialization() {
    const std::vector<std::function<void()>> preReadFailures{
        [] { Fake::failInfo = true; }, [] { --Fake::infoBytes; },
        [] { --Fake::process.wVersion; }, [] { --Fake::process.wSize; },
        [] { Fake::process.dwPID = 1; }, [] { Fake::process.dwState = 1; },
        [] { Fake::process.tpMemoryModel = VMMDLL_MEMORYMODEL_X86; },
        [] { Fake::process.tpSystem = VMMDLL_SYSTEM_WINDOWS_32; },
        [] { Fake::process.win.vaEPROCESS = 0; }, [] { ++Fake::process.win.vaEPROCESS; },
        [] { Fake::process.win.vaEPROCESS = 0x12345000; },
        [] { Fake::process.win.vaEPROCESS = 0xfffffffffffffff8; },
        [] { Fake::process.paDTB = 0; }, [] { Fake::process.paDTB = 0xfff; },
        [] { Fake::process.paDTB = ULONG64{1} << 63; }, [] { Fake::symbolsAvailable = false; },
        [] { Fake::sizes.erase("_KPROCESS"); }, [] { Fake::sizes["_EPROCESS"] = 0; },
        [] { Fake::sizes["_EPROCESS"] = 65 * 1024; }, [] { Fake::sizes["_EPROCESS"] = 0x181; },
        [] { Fake::sizes["_KPROCESS"] = 0x200; },
        [] { Fake::fields.erase({"_EPROCESS", "CreateTime"}); },
        [] { Fake::fields[{"_EPROCESS", "Pcb"}] = MAXDWORD; },
        [] { Fake::fields[{"_KPROCESS", "DirectoryTableBase"}] = MAXDWORD; },
        [] { Fake::fields[{"_KPROCESS", "DirectoryTableBase"}] = 0x60; },
        [] { Fake::fields[{"_EPROCESS", "UniqueProcessId"}] = 0x181; },
        [] { Fake::fields[{"_EPROCESS", "UniqueProcessId"}] = 0x30; },
        [] { Fake::fields[{"_EPROCESS", "CreateTime"}] = 0x90; },
        [] { Fake::fields[{"_EPROCESS", "ExitTime"}] = 0xc1; }
    };
    for (const auto& corrupt : preReadFailures) {
        Fake::Reset(); corrupt(); TargetLiveness guard;
        Check(!guard.Initialize(Fake::Handle(), Fake::Pid), "Invalid process metadata or PDB layout fails binding");
        Check(!guard.Ready() && guard.Epoch() == 0 && Fake::readCalls == 0 && !guard.Reason().empty(),
            "Rejected metadata performs no kernel reads and provides a reason");
    }
    const std::vector<std::function<void()>> dataFailures{
        [] { Fake::SetIdentity(1); }, [] { Fake::SetIdentity(Fake::Pid, 0); },
        [] { Fake::SetIdentity(Fake::Pid, ULONG64{1} << 63); },
        [] { Fake::SetIdentity(Fake::Pid, Fake::Created, 1); },
        [] { Fake::SetIdentity(Fake::Pid, Fake::Created, 0, 0); },
        [] { Fake::SetIdentity(Fake::Pid, Fake::Created, 0, Fake::Dtb + 0x1000); },
        [] { Fake::failReadAt = 1; }, [] { Fake::shortReadAt = 2; },
        [] { Fake::beforeRead = [](size_t count) { if (count == 2) Fake::SetIdentity(Fake::Pid, Fake::Created + 1); }; }
    };
    for (const auto& corrupt : dataFailures) {
        Fake::Reset(); corrupt(); TargetLiveness guard;
        Check(!guard.Initialize(Fake::Handle(), Fake::Pid) && !guard.Ready() && guard.Epoch() == 0,
            "Invalid or changing initial identity never establishes a baseline");
        Check(Fake::events.empty() && guard.Probe() == Result::Unavailable, "Failed binding emits no success provenance or alive result");
    }
    Fake::Reset(); TargetLiveness guard;
    Check(!guard.Initialize(nullptr, Fake::Pid) && !guard.Initialize(Fake::Handle(), 0) && Fake::infoCalls == 0,
        "Invalid handle or PID never enters MemProcFS");
}
}

int main() {
    try {
        NormalAndDifferentLayout(); ReadFailuresDoNotRebind(); TerminalIdentityLoss(); RejectedInitialization();
        std::cout << "PASS: " << checks << " target-liveness checks; mocked current-kernel PDB and memory; no DMA session\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
