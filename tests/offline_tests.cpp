#define NOMINMAX
#include <windows.h>
#include "DMA/MemoryPolicy.h"
#include "DMA/ScatterBatch.h"
#include "DMA/Process.h"
#include "Diagnostics/Diagnostics.h"
#include "Deadlock/Classes/CCitadelPlayerPawn.h"
#include "Deadlock/Classes/CEntityListEntry.h"
#include <json.hpp>
#include <vector>
#include <array>
#include <thread>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <random>

namespace Fake {
struct Request { ULONG64 address; DWORD size; PBYTE buffer; PDWORD read; };
struct Region { ULONG64 address; std::vector<BYTE> bytes; };
std::vector<Request> requests;
std::vector<Region> regions;
bool initOk = true, prepareOk = true, executeOk = true, partial = false, clearOk = true;
bool remoteFixture = false;
ULONG64 shortAddress{};
int opened{}, closed{};
DWORD initializeFlags{}, clearFlags{};
void AddRegion(ULONG64 address, size_t size) { regions.push_back({address, std::vector<BYTE>(size, 0xa5)}); }
template<typename T> void Put(ULONG64 address, const T& value) {
    for (auto& region : regions) {
        if (address < region.address) continue;
        const auto offset = address - region.address;
        if (offset <= region.bytes.size() && sizeof(T) <= region.bytes.size() - offset) {
            memcpy(region.bytes.data() + offset, &value, sizeof(T)); return;
        }
    }
    throw std::runtime_error("fixture write outside mapped memory");
}
DWORD Copy(ULONG64 address, DWORD size, BYTE* buffer) {
    for (const auto& region : regions) {
        if (address < region.address) continue;
        const auto offset = address - region.address;
        if (offset <= region.bytes.size() && size <= region.bytes.size() - offset) {
            memcpy(buffer, region.bytes.data() + offset, size); return size;
        }
    }
    return 0;
}
}
extern "C" VMMDLL_SCATTER_HANDLE VMMDLL_Scatter_Initialize(VMM_HANDLE, DWORD, DWORD flags) {
    Fake::initializeFlags = flags;
    if (!Fake::initOk) return nullptr;
    ++Fake::opened; return reinterpret_cast<VMMDLL_SCATTER_HANDLE>(1);
}
extern "C" BOOL VMMDLL_Scatter_PrepareEx(VMMDLL_SCATTER_HANDLE, ULONG64 address, DWORD size, PBYTE buffer, PDWORD read) {
    if (!Fake::prepareOk) return FALSE;
    Fake::requests.push_back({address, size, buffer, read}); return TRUE;
}
extern "C" BOOL VMMDLL_Scatter_ExecuteRead(VMMDLL_SCATTER_HANDLE) {
    if (!Fake::executeOk) return FALSE;
    for (auto& r : Fake::requests) {
        const auto size = Fake::partial || r.address == Fake::shortAddress ? r.size - 1 : r.size;
        if (Fake::remoteFixture) *r.read = Fake::Copy(r.address, size, r.buffer);
        else { *r.read = size; memset(r.buffer, 0x42, *r.read); }
    }
    return TRUE;
}
extern "C" BOOL VMMDLL_Scatter_Clear(VMMDLL_SCATTER_HANDLE, DWORD, DWORD flags) {
    Fake::clearFlags = flags;
    if (!Fake::clearOk) return FALSE;
    Fake::requests.clear(); return TRUE;
}
extern "C" VOID VMMDLL_Scatter_CloseHandle(VMMDLL_SCATTER_HANDLE) { ++Fake::closed; Fake::requests.clear(); }
int checks{};
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void CheckRequest(ULONG64 address, DWORD size, const char* message) {
    size_t found{};
    for (const auto& request : Fake::requests) if (request.address == address && request.size == size) ++found;
    Check(found == 1, message);
}
bool SameVector(const Vector3& actual, const Vector3& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z;
}
void TestEntityListPageBoundary(VMM_HANDLE vmm) {
    constexpr size_t EntryCount = 512;
    constexpr ULONG64 address = 0x700008;
    constexpr size_t mappedBeforeNextPage = 0xe000 - 8;
    constexpr auto requiredBytes = EntityListLayout::ReadBytes(EntryCount);
    using EntryArray = std::array<CEntityListEntry, EntryCount>;
    Check(EntityListLayout::ReadBytes(0) == 0 && EntityListLayout::ReadBytes(1) == 0x28,
        "Empty and single-entry reads require only actual entity fields");
    Check(EntityListLayout::ReadBytes((std::numeric_limits<size_t>::max)()) == 0,
        "An impossible entity count cannot overflow its read size");
    Fake::remoteFixture = true;
    Fake::AddRegion(address, mappedBeforeNextPage);
    // Independent fixture layout: full 0x70 stride, pEnt at 0, pName at 0x20.
    // The observed pointer starts eight bytes into its page; the next page is
    // absent. All 512 pairs of used fields still reside in the mapped region.
    for (size_t index = 0; index < EntryCount; ++index) {
        Fake::Put(address + index * 0x70, uintptr_t{0x100000 + index * 0x100});
        Fake::Put(address + index * 0x70 + 0x20, uintptr_t{0x200000 + index * 0x80});
    }
    {
        EntryArray staged{};
        ScatterBatch batch(vmm, 10);
        batch.Prepare(address, sizeof(staged), reinterpret_cast<BYTE*>(staged.data()));
        Check(!batch.Execute(), "Former full-array read fails because only unused final padding crosses into an unmapped page");
    }
    {
        EntryArray staged{};
        ScatterBatch batch(vmm, 10);
        batch.Prepare(address, static_cast<DWORD>(requiredBytes), reinterpret_cast<BYTE*>(staged.data()));
        Check(batch.Execute(), "Field-bounded read succeeds with the next page completely unmapped");
        for (size_t index = 0; index < EntryCount; ++index) {
            Check(staged[index].pEnt == 0x100000 + index * 0x100, "Full stride retains each entity pointer including entry 511");
            Check(staged[index].pName == 0x200000 + index * 0x80, "Full stride retains each class pointer including entry 511");
        }
        const auto* bytes = reinterpret_cast<const BYTE*>(staged.data());
        Check(std::all_of(bytes + requiredBytes, bytes + sizeof(staged), [](BYTE value) { return value == 0; }),
            "Omitted tail padding stays zero-initialized in staging");
    }
    // Also remove all padding after the final used field, not just the page's
    // final eight bytes. The exact required range remains sufficient.
    Fake::regions.front().bytes.resize(511 * 0x70 + 0x28);
    {
        EntryArray staged{};
        ScatterBatch batch(vmm, 10);
        batch.Prepare(address, static_cast<DWORD>(requiredBytes), reinterpret_cast<BYTE*>(staged.data()));
        Check(batch.Execute() && staged.back().pName == 0x200000 + 511 * 0x80,
            "No final-entry padding is required to obtain its complete pName");
    }
    Fake::regions.front().bytes.pop_back();
    {
        EntryArray staged{};
        ScatterBatch batch(vmm, 10);
        batch.Prepare(address, static_cast<DWORD>(requiredBytes), reinterpret_cast<BYTE*>(staged.data()));
        Check(!batch.Execute(), "Missing the final byte of entry 511 pName still fails exact-byte validation");
    }
    Fake::remoteFixture = false;
    Fake::regions.clear();
}
void TestBuild6766Layout(VMM_HANDLE vmm) {
    static_assert(sizeof(CHandle) == 4 && sizeof(ETeam) == 1 && sizeof(Vector3) == 12);
    constexpr ULONG64 pawnAddress = 0x100000, controllerAddress = 0x200000;
    constexpr ULONG64 pawnScene = 0x300000, bonesAddress = 0x400000, controllerScene = 0x500000;
    // Offsets are independent fixture facts from the 2026-10-08 schema, not production constants.
    // Poisoning the former layout makes regression to an old offset observable in values and requests.
    Fake::remoteFixture = true;
    Fake::AddRegion(pawnAddress, 0x1600); Fake::AddRegion(controllerAddress, 0x1200);
    Fake::AddRegion(pawnScene, 0x300); Fake::AddRegion(controllerScene, 0x200);
    Fake::AddRegion(bonesAddress, MAX_BONES * 0x20);
    Fake::Put(pawnAddress + 0x10a8, uint32_t{9001});
    Fake::Put(pawnAddress + 0x12d8, int32_t{99111});
    Fake::Put(pawnAddress + 0x12e4, int32_t{99222});
    Fake::Put(pawnAddress + 0x438, Vector3{900.0f, 901.0f, 902.0f});
    Fake::Put(pawnAddress + 0x3f3, uint8_t{99});
    Fake::Put(controllerAddress + 0x8ac, uint32_t{9002});
    Fake::Put(controllerAddress + 0x8f0 + 0x24, int32_t{99333});
    Fake::Put(controllerAddress + 0x8f0 + 0x4c, int32_t{99444});
    Fake::Put(pawnScene + 0x150 + 0x80, uintptr_t{0xdead0000});
    Fake::Put(pawnAddress + 0x330, uintptr_t{pawnScene});
    Fake::Put(pawnAddress + 0x354, int32_t{700});
    Fake::Put(pawnAddress + 0x3ef, uint8_t{2});
    Fake::Put(pawnAddress + 0x1050, uint32_t{42});
    const std::array<int32_t, 6> currencies{1450, 100001, 100002, 230, 100004, 100005};
    Fake::Put(pawnAddress + 0x1234, currencies);
    const Vector3 velocity{12.0f, -4.5f, 0.0f}, position{100.0f, 200.0f, 300.0f};
    Fake::Put(pawnAddress + 0x400, velocity);
    Fake::Put(pawnScene + 0xc8, position);
    Fake::Put(pawnScene + 0x103, uint8_t{0});
    Fake::Put(pawnScene + 0x140 + 0x80, uintptr_t{bonesAddress});
    for (int i = 0; i < MAX_BONES; ++i) Fake::Put(bonesAddress + i * 0x20, Vector3{float(i), float(i + 1), float(i + 2)});
    Fake::Put(controllerAddress + 0x330, uintptr_t{controllerScene});
    Fake::Put(controllerAddress + 0x3ef, uint8_t{3});
    Fake::Put(controllerAddress + 0x8c0, uint32_t{84});
    Fake::Put(controllerAddress + 0x908 + 0x08, int32_t{19});
    Fake::Put(controllerAddress + 0x908 + 0x10, int32_t{1200});
    Fake::Put(controllerAddress + 0x908 + 0x1c, int32_t{13});
    Fake::Put(controllerAddress + 0x908 + 0x2c, int32_t{23333});
    Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{700});
    Fake::Put(controllerScene + 0xc8, Vector3{400.0f, 500.0f, 600.0f});
    Fake::Put(controllerScene + 0x103, uint8_t{0});
    {
        ScatterBatch batch(vmm, 10);
        CCitadelPlayerPawn pawn{pawnAddress};
        CCitadelPlayerController controller{controllerAddress};
        pawn.PrepareRead_1(batch);
        Check(Fake::requests.size() == 6, "pawn primary read count");
        CheckRequest(pawnAddress + 0x330, 8, "pawn scene pointer width");
        CheckRequest(pawnAddress + 0x3ef, 1, "TeamNum is one byte at build 6766 offset");
        CheckRequest(pawnAddress + 0x1050, 4, "pawn controller handle offset and width");
        CheckRequest(pawnAddress + 0x1234, 4, "gold uses currency index zero");
        CheckRequest(pawnAddress + 0x1240, 4, "unsecured gold uses currency index three");
        CheckRequest(pawnAddress + 0x400, 12, "absolute velocity is a plain Vector");
        Check(batch.Execute(), "pawn build 6766 primary read");
        Check(pawn.m_GameSceneNodeAddress == pawnScene && pawn.m_TeamNum == ETeam::HIDDEN_KING, "pawn scene and team values");
        Check(pawn.m_hController.Data == 42 && pawn.m_TotalUnspentSouls == 1450 && pawn.m_UnsecuredSouls == 230, "pawn handle and currency values");
        Check(SameVector(pawn.m_Velocity, velocity), "pawn velocity avoids network wrapper");
        Check(batch.Clear(), "clear pawn primary");
        controller.PrepareRead_1(batch);
        Check(Fake::requests.size() == 8, "controller primary read count");
        CheckRequest(controllerAddress + 0x8c0, 4, "hero pawn handle offset and width");
        CheckRequest(controllerAddress + 0x908 + 0x08, 4, "player data level address");
        CheckRequest(controllerAddress + 0x908 + 0x10, 4, "player data max health address");
        CheckRequest(controllerAddress + 0x908 + 0x1c, 4, "player data hero ID address");
        CheckRequest(controllerAddress + 0x908 + 0x2c, 4, "player data net worth address");
        CheckRequest(controllerAddress + 0x908 + 0x54, 4, "player data health address");
        Check(batch.Execute(), "controller build 6766 primary read");
        Check(controller.m_hPawn.Data == 84 && controller.m_CurrentLevel == 19 && controller.m_MaxHealth == 1200, "controller primary identity values");
        Check(controller.m_CurrentHealth == 700 && controller.m_TotalSouls == 23333 && controller.m_HeroID == HeroId::Haze, "controller new health net worth and hero values");
        Check(controller.m_TeamNum == ETeam::ARCH_MOTHER, "controller byte team value");
        Check(batch.Clear(), "clear controller primary");
        pawn.PrepareRead_2(batch);
        Check(Fake::requests.size() == 3, "pawn secondary read count");
        CheckRequest(pawnScene + 0x140 + 0x80, 8, "bone pointer follows current model state offset");
        Check(batch.Execute(), "pawn build 6766 scene read");
        Check(pawn.m_BoneArrayAddress == bonesAddress && SameVector(pawn.m_Position, position), "scene position and bone pointer");
        Check(batch.Clear(), "clear pawn secondary");
        pawn.PrepareRead_3(batch);
        Check(Fake::requests.size() == 1, "bone transforms use one contiguous read");
        CheckRequest(bonesAddress, 69 * 0x20 + 12, "bone span includes last vector without trailing padding");
        Check(batch.Execute(), "bone array mapped read");
        Check(SameVector(pawn.m_BonePositions[1], Vector3{}), "DMA staging does not mutate published bones");
        Check(pawn.CommitBoneRead(), "complete bone span commits");
        for (int i = 0; i < MAX_BONES; ++i) Check(SameVector(pawn.m_BonePositions[i], Vector3{float(i), float(i + 1), float(i + 2)}), "bone vector stride excludes poisoned transform padding");
        Check(!pawn.CommitBoneRead(), "bone staging cannot be committed twice");
        Check(batch.Clear(), "clear bone read");
        Fake::Put(pawnAddress + 0x400, Vector3{-8.0f, 9.0f, 10.0f});
        Fake::Put(pawnScene + 0xc8, Vector3{101.0f, 202.0f, 303.0f});
        Fake::Put(bonesAddress, Vector3{7.0f, 8.0f, 9.0f});
        pawn.QuickRead(batch);
        Check(Fake::requests.size() == 4, "quick pawn movement and one bone span read");
        CheckRequest(pawnAddress + 0x400, 12, "quick pawn uses current absolute velocity");
        Check(batch.Execute(), "quick pawn read succeeds");
        Check(pawn.CommitBoneRead(), "quick pawn complete bone span commits");
        Check(SameVector(pawn.m_Velocity, Vector3{-8.0f, 9.0f, 10.0f}) && SameVector(pawn.m_Position, Vector3{101.0f, 202.0f, 303.0f}), "quick pawn movement updated");
        Check(SameVector(pawn.m_BonePositions[0], Vector3{7.0f, 8.0f, 9.0f}), "quick pawn bone updated");
        Check(batch.Clear(), "clear quick pawn");
        Fake::Put(bonesAddress, Vector3{70.0f, 80.0f, 90.0f});
        Fake::Put(bonesAddress + 69 * 0x20, Vector3{700.0f, 800.0f, 900.0f});
        Fake::shortAddress = bonesAddress;
        pawn.PrepareRead_3(batch);
        Check(!batch.Execute(), "short contiguous bone read rejects batch");
        Check(!pawn.CommitBoneRead(), "short bone span cannot commit");
        Check(SameVector(pawn.m_BonePositions[0], Vector3{7.0f, 8.0f, 9.0f}) &&
            SameVector(pawn.m_BonePositions[69], Vector3{69.0f, 70.0f, 71.0f}), "short bone read preserves all last-good vectors");
        Check(batch.Clear(), "clear short bone read");
        Fake::shortAddress = 0;
        pawn.m_BoneArrayAddress = 0;
        pawn.QuickRead(batch);
        Check(Fake::requests.size() == 3, "null optional bone array only reads movement");
        Check(batch.Execute(), "missing bones do not fail movement batch");
        Check(!pawn.CommitBoneRead(), "null bone array cannot commit stale staging");
        Check(SameVector(pawn.m_BonePositions[0], Vector3{7.0f, 8.0f, 9.0f}), "missing bones preserve last-good skeleton");
        Check(batch.Clear(), "clear null bone array read");
        pawn.m_BoneArrayAddress = (std::numeric_limits<uintptr_t>::max)() - 10;
        pawn.PrepareRead_3(batch);
        Check(Fake::requests.empty() && batch.Execute(), "overflowing optional bone span is skipped");
        Check(!pawn.CommitBoneRead(), "overflow bone span cannot commit");
        Check(batch.Clear(), "clear overflow bone span");
        pawn.m_BoneArrayAddress = bonesAddress;
        Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{500});
        Fake::shortAddress = controllerAddress + 0x908 + 0x54;
        controller.QuickRead(batch);
        Check(Fake::requests.size() == 3, "quick controller read count");
        CheckRequest(controllerAddress + 0x908 + 0x54, 4, "quick controller health uses new layout");
        Check(!batch.Execute(), "quick health short read rejects batch");
        Check(batch.Clear(), "clear layout short read");
        Fake::shortAddress = 0;
        controller.QuickRead(batch);
        Check(batch.Execute(), "quick health recovers on next complete read");
        Check(controller.m_CurrentHealth == 500, "quick controller health updated after recovery");
        Check(batch.Clear(), "clear layout recovery");
        CBaseEntity base{pawnAddress}; base.PrepareRead_1(batch);
        CheckRequest(pawnAddress + 0x354, 4, "base entity health remains at schema offset");
        Check(batch.Execute() && base.m_CurrentHealth == 700, "base health independent of controller player data");
    }
    Fake::remoteFixture = false; Fake::regions.clear();
}
void TestBoxOnlyPawnReads(VMM_HANDLE vmm) {
    constexpr ULONG64 pawnAddress = 0x600000, pawnScene = 0x700000, bonesAddress = 0x800000;
    // Only box-required remote fields exist. Any bone, velocity, currency or HP read fails.
    Fake::remoteFixture = true;
    Fake::AddRegion(pawnAddress + 0x330, 8); Fake::Put(pawnAddress + 0x330, uintptr_t{pawnScene});
    Fake::AddRegion(pawnAddress + 0x3ef, 1); Fake::Put(pawnAddress + 0x3ef, uint8_t{2});
    Fake::AddRegion(pawnAddress + 0x1050, 4); Fake::Put(pawnAddress + 0x1050, uint32_t{42});
    Fake::AddRegion(pawnScene + 0xc8, 60); Fake::Put(pawnScene + 0xc8, Vector3{10.0f, 20.0f, 30.0f});
    Fake::Put(pawnScene + 0x103, uint8_t{0});
    {
        ScatterBatch batch(vmm, 10);
        CCitadelPlayerPawn pawn{pawnAddress};
        pawn.m_BoneArrayAddress = bonesAddress; // Unmapped stale optional pointer must never be consulted.
        pawn.m_BonePositions[0] = Vector3{7.0f, 8.0f, 9.0f};
        pawn.m_Velocity = Vector3{70.0f, 80.0f, 90.0f};
        pawn.m_TotalUnspentSouls = 777; pawn.m_UnsecuredSouls = 888;
        const auto boneAttempts = Diagnostics::BoneReadRequests.load();
        pawn.PrepareRead_1(batch, false, false);
        Check(Fake::requests.size() == 3, "box pawn primary reads only scene team and controller");
        CheckRequest(pawnAddress + 0x330, 8, "box primary scene pointer required");
        CheckRequest(pawnAddress + 0x3ef, 1, "box primary team required");
        CheckRequest(pawnAddress + 0x1050, 4, "box primary controller handle required");
        Check(batch.Execute(), "box primary succeeds with all optional fields unmapped");
        Check(pawn.m_hController.Data == 42 && pawn.m_TeamNum == ETeam::HIDDEN_KING, "box identity fields updated");
        Check(SameVector(pawn.m_Velocity, Vector3{70.0f, 80.0f, 90.0f}) &&
            pawn.m_TotalUnspentSouls == 777 && pawn.m_UnsecuredSouls == 888, "box primary preserves unread optional values");
        Check(batch.Clear(), "clear box primary");
        pawn.PrepareRead_2(batch, false);
        Check(Fake::requests.size() == 1, "box secondary uses one scene span and omits bone pointer");
        CheckRequest(pawnScene + 0xc8, 60, "box scene span covers position through dormancy");
        Check(batch.Execute() && SameVector(pawn.m_Position, Vector3{}), "box secondary stages scene without mutating published position");
        Check(pawn.CommitSceneRead() && SameVector(pawn.m_Position, Vector3{10.0f, 20.0f, 30.0f}), "complete box scene span publishes correct position");
        Check(!pawn.CommitSceneRead(), "scene span cannot be committed twice");
        Check(batch.Clear(), "clear box secondary");
        pawn.PrepareRead_3(batch, false);
        Check(Fake::requests.empty() && batch.Execute(), "box tertiary makes no DMA requests");
        Check(!pawn.CommitBoneRead(false), "box tertiary cannot publish bones");
        Check(batch.Clear(), "clear box tertiary");
        Fake::Put(pawnScene + 0xc8, Vector3{11.0f, 22.0f, 33.0f});
        Fake::Put(pawnScene + 0x103, uint8_t{1});
        pawn.QuickRead(batch, false, false);
        Check(Fake::requests.size() == 1, "box quick combines position and dormancy in one request");
        CheckRequest(pawnScene + 0xc8, 60, "box quick requests exactly sixty bytes");
        Check(batch.Execute(), "box quick succeeds with optional data unmapped");
        Check(SameVector(pawn.m_Position, Vector3{10.0f, 20.0f, 30.0f}) && !pawn.IsDormant(), "box quick keeps position and dormancy staged until commit");
        Check(pawn.CommitSceneRead() && SameVector(pawn.m_Position, Vector3{11.0f, 22.0f, 33.0f}) && pawn.IsDormant(), "box quick atomically publishes movement and dormancy");
        Check(!pawn.CommitBoneRead(false) && Diagnostics::BoneReadRequests.load() == boneAttempts, "box full and quick paths attempt zero bone spans");
        Check(batch.Clear(), "clear box quick");

        // Required reads still reject a partial result; a subsequent complete pass recovers.
        Fake::Put(pawnScene + 0xc8, Vector3{100.0f, 200.0f, 300.0f});
        Fake::Put(pawnScene + 0x103, uint8_t{0});
        Fake::shortAddress = pawnScene + 0xc8;
        pawn.QuickRead(batch, false, false);
        Check(!batch.Execute(), "box scene span short read rejects batch");
        Check(!pawn.CommitSceneRead() && SameVector(pawn.m_Position, Vector3{11.0f, 22.0f, 33.0f}) && pawn.IsDormant(), "short scene span preserves both last-good position and dormancy");
        Check(batch.Clear(), "clear box partial position");
        Fake::shortAddress = 0;
        Fake::Put(pawnScene + 0xc8, Vector3{12.0f, 24.0f, 36.0f});
        pawn.QuickRead(batch, false, false);
        Check(batch.Execute() && pawn.CommitSceneRead() && SameVector(pawn.m_Position, Vector3{12.0f, 24.0f, 36.0f}) && !pawn.IsDormant(), "box quick recovers both fields on next complete scene span");
        Check(batch.Clear(), "clear box recovery");

        // A copied staging object may retain an uncommitted bone read from the previous mode.
        Fake::AddRegion(bonesAddress, MAX_BONES * 0x20);
        Fake::Put(bonesAddress, Vector3{70.0f, 80.0f, 90.0f});
        pawn.PrepareRead_3(batch);
        Check(batch.Execute(), "seed pending bone read before mode switch");
        auto copiedPawn = pawn;
        Check(batch.Clear(), "clear pending bone request");
        copiedPawn.QuickRead(batch, false, false);
        Check(Fake::requests.size() == 1 && batch.Execute() && copiedPawn.CommitSceneRead(), "copied pawn enters box quick span path");
        Check(!copiedPawn.CommitBoneRead() && SameVector(copiedPawn.m_BonePositions[0], Vector3{7.0f, 8.0f, 9.0f}), "box quick discards copied pending bones");
        Check(!pawn.CommitBoneRead(false) && !pawn.CommitBoneRead(), "disabled commit consumes pending staging without exposing it later");
        Check(batch.Clear(), "clear copied box quick");

        // A copied complete scene span is discarded by the next read mode, not replayed over newer fields.
        pawn.QuickRead(batch, false, false);
        Check(batch.Execute(), "seed pending scene span before mode switch");
        auto copiedScenePawn = pawn;
        Check(batch.Clear(), "clear pending scene span");
        Fake::Put(pawnScene + 0xc8, Vector3{21.0f, 42.0f, 63.0f});
        Fake::Put(pawnScene + 0x103, uint8_t{1});
        Fake::AddRegion(pawnAddress + 0x400, 12); Fake::Put(pawnAddress + 0x400, Vector3{1.0f, 2.0f, 3.0f});
        copiedScenePawn.QuickRead(batch, false, true);
        Check(Fake::requests.size() == 3 && batch.Execute(), "velocity mode retains existing individual scene and velocity reads");
        Check(!copiedScenePawn.CommitSceneRead() && SameVector(copiedScenePawn.m_Position, Vector3{21.0f, 42.0f, 63.0f}) && copiedScenePawn.IsDormant(), "mode switch discards copied pending scene without overwriting fresh fields");
        Check(batch.Clear(), "clear scene mode switch");
        pawn.QuickRead(batch, false, false);
        Check(batch.Execute() && pawn.CommitSceneRead() && SameVector(pawn.m_Position, Vector3{21.0f, 42.0f, 63.0f}), "fresh box pass replaces original pending scene span");
        Check(batch.Clear(), "clear fresh scene span");

        CCitadelPlayerPawn invalidPawn{0};
        invalidPawn.PrepareRead_1(batch, false, false);
        Check(invalidPawn.IsInvalid() && Fake::requests.empty(), "null pawn produces no ancillary box requests");
        invalidPawn.QuickRead(batch, false, false);
        Check(Fake::requests.empty() && !invalidPawn.CommitBoneRead(false) && !invalidPawn.CommitSceneRead(), "invalid pawn quick path cannot prepare or commit data");
        CCitadelPlayerPawn nullScenePawn{pawnAddress};
        nullScenePawn.QuickRead(batch, false, false);
        Check(nullScenePawn.IsInvalid() && Fake::requests.empty(), "box quick never reads a null scene node");
        CCitadelPlayerPawn overflowScenePawn{pawnAddress};
        overflowScenePawn.m_GameSceneNodeAddress = (std::numeric_limits<uintptr_t>::max)() - 100;
        overflowScenePawn.QuickRead(batch, false, false);
        Check(overflowScenePawn.IsInvalid() && Fake::requests.empty(), "box quick rejects overflowing scene span");
    }
    Fake::remoteFixture = false; Fake::regions.clear();
}
void TestBoxOnlyControllerReads(VMM_HANDLE vmm) {
    constexpr ULONG64 controllerAddress = 0x900000;
    // Controllers can be nonspatial. Only identity/team/live health are mapped for boxes.
    Fake::remoteFixture = true;
    Fake::AddRegion(controllerAddress + 0x3ef, 1); Fake::Put(controllerAddress + 0x3ef, uint8_t{3});
    Fake::AddRegion(controllerAddress + 0x8c0, 4); Fake::Put(controllerAddress + 0x8c0, uint32_t{84});
    Fake::AddRegion(controllerAddress + 0x908 + 0x54, 4); Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{700});
    {
        ScatterBatch batch(vmm, 10);
        CCitadelPlayerController controller{controllerAddress};
        controller.m_CurrentLevel = 20; controller.m_MaxHealth = 1200;
        controller.m_TotalSouls = 999; controller.m_HeroID = HeroId::Haze;
        controller.PrepareRead_1(batch, true);
        Check(Fake::requests.size() == 3, "minimal controller reads only team pawn handle and health");
        CheckRequest(controllerAddress + 0x3ef, 1, "minimal controller team required");
        CheckRequest(controllerAddress + 0x8c0, 4, "minimal controller pawn identity required");
        CheckRequest(controllerAddress + 0x908 + 0x54, 4, "minimal controller live health required");
        Check(batch.Execute(), "minimal controller succeeds without mapped scene or optional player data");
        Check(!controller.IsInvalid() && !controller.IsDead() && controller.m_hPawn.Data == 84 &&
            controller.m_TeamNum == ETeam::ARCH_MOTHER, "minimal controller preserves alive identity without scene node");
        Check(controller.m_CurrentLevel == 20 && controller.m_MaxHealth == 1200 && controller.m_TotalSouls == 999 &&
            controller.m_HeroID == HeroId::Haze, "minimal controller leaves unused stats untouched");
        Check(batch.Clear(), "clear minimal controller primary");
        controller.PrepareRead_2(batch, false);
        Check(Fake::requests.empty() && batch.Execute() && !controller.IsInvalid(), "minimal controller skips spatial stage without invalidating nonspatial object");
        Check(controller.m_GameSceneNodeAddress == 0, "minimal controller never acquires scene pointer");
        Check(batch.Clear(), "clear minimal controller secondary");
        Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{0});
        controller.PrepareRead_1(batch, true);
        Check(batch.Execute() && controller.IsDead(), "minimal controller zero health hides dead hero");
        Check(batch.Clear(), "clear minimal controller zero health");
        Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{-5});
        controller.PrepareRead_1(batch, true);
        Check(batch.Execute() && controller.IsDead(), "minimal controller negative health remains dead");
        Check(batch.Clear(), "clear minimal controller negative health");
        Fake::Put(controllerAddress + 0x908 + 0x54, int32_t{500});
        Fake::shortAddress = controllerAddress + 0x908 + 0x54;
        controller.PrepareRead_1(batch, true);
        Check(!batch.Execute(), "minimal controller short health rejects publication batch");
        Check(batch.Clear(), "clear minimal controller short health");
        Fake::shortAddress = 0;
        controller.PrepareRead_1(batch, true);
        Check(batch.Execute() && !controller.IsDead() && controller.m_CurrentHealth == 500, "minimal controller health read recovers after short result");
        Check(batch.Clear(), "clear minimal controller health recovery");
        CCitadelPlayerController invalidController{0};
        invalidController.PrepareRead_1(batch, true);
        Check(invalidController.IsInvalid() && Fake::requests.empty(), "null minimal controller prepares no remote requests");
    }
    Fake::remoteFixture = false; Fake::regions.clear();
}
void TestCameraScatterReads(VMM_HANDLE vmm) {
    for (const bool resident : {false, true}) {
        RuntimeOptions::ResidentGameReads = resident;
        ScatterBatch policy(vmm, 10);
        const DWORD expectedFlags = VMMDLL_FLAG_NOCACHE | (resident ? VMMDLL_FLAG_NOPAGING : 0);
        Check(Fake::initializeFlags == expectedFlags, "scatter init applies current resident read policy");
        Check(policy.Clear() && Fake::clearFlags == expectedFlags, "scatter clear preserves resident read policy");
    }
    RuntimeOptions::ResidentGameReads = false;
    constexpr ULONG64 cameraAddress = 0xa00a60;
    using Camera = std::array<float, 16>;
    static_assert(sizeof(Camera) == 64);
    Camera expected{};
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = float(i + 1) / 4.0f;
    Fake::remoteFixture = true;
    Fake::AddRegion(cameraAddress, sizeof(Camera)); Fake::Put(cameraAddress, expected);
    Camera published{};
    const auto openedBefore = Fake::opened, closedBefore = Fake::closed;
    Check(ScatterBatch::ReadValue(vmm, 10, cameraAddress, published), "camera scatter reads exactly mapped 64 bytes");
    Check(published == expected, "camera scatter publishes complete matrix values");
    Check(Fake::opened == openedBefore + 1 && Fake::closed == closedBefore + 1 && Fake::requests.empty(), "camera scatter closes successful temporary batch");
    const auto previous = published;
    Fake::shortAddress = cameraAddress;
    Check(!ScatterBatch::ReadValue(vmm, 10, cameraAddress, published), "short camera scatter fails");
    Check(published == previous, "short camera scatter cannot partially mutate published matrix");
    Fake::shortAddress = 0;
    Fake::executeOk = false;
    Check(!ScatterBatch::ReadValue(vmm, 10, cameraAddress, published) && published == previous, "camera execution failure retains output");
    Fake::executeOk = true; Fake::prepareOk = false;
    Check(!ScatterBatch::ReadValue(vmm, 10, cameraAddress, published) && published == previous, "camera preparation failure retains output");
    Fake::prepareOk = true; Fake::initOk = false;
    Check(!ScatterBatch::ReadValue(vmm, 10, cameraAddress, published) && published == previous, "camera initialization failure retains output");
    Fake::initOk = true;
    Check(!ScatterBatch::ReadValue(vmm, 10, 0, published), "null camera address rejected");
    Check(!ScatterBatch::ReadValue(vmm, 0, cameraAddress, published), "zero camera pid rejected");
    Check(!ScatterBatch::ReadValue(nullptr, 10, cameraAddress, published), "missing camera connection rejected");
    Check(ScatterBatch::ReadValue(vmm, 10, cameraAddress, published) && published == expected, "camera recovers after failed reads");
    Fake::remoteFixture = false; Fake::regions.clear();
}
void TestBoundedScatterDiagnostics(VMM_HANDLE vmm) {
    // Mixed complete/partial/unmapped responses distinguish API success from byte completeness.
    Check(Fake::requests.empty(), "bounded diagnostics begins without pending native requests");
    Fake::initOk = Fake::prepareOk = Fake::executeOk = Fake::clearOk = true;
    Fake::partial = false; Fake::shortAddress = 0; Fake::regions.clear();
    Fake::remoteFixture = true;
    Fake::AddRegion(0xabc000, 4); Fake::AddRegion(0xabc100, 8);
    Fake::shortAddress = 0xabc100;
    const auto openedBefore = Fake::opened, closedBefore = Fake::closed;
    {
        ScatterBatch batch(vmm, 77);
        std::array<std::array<BYTE, 16>, 5> buffers{};
        std::array<DWORD, 5> actual{};
        const std::array<ULONG64, 5> addresses{0xabc000, 0xabc100, 0xabc200, 0xabc300, 0xabc400};
        const std::array<DWORD, 5> sizes{4, 8, 4, 16, 1};
        for (size_t i = 0; i < addresses.size(); ++i)
            Check(batch.Prepare(addresses[i], sizes[i], buffers[i].data(), &actual[i]), "bounded diagnostics fixture requests prepare");
        const auto before = Diagnostics::GetSnapshot();
        Check(!batch.Execute(), "successful scatter API with incomplete responses rejects batch");
        const auto after = Diagnostics::GetSnapshot();
        Check(actual == std::array<DWORD, 5>{4, 7, 0, 0, 0}, "partial diagnostics preserve each actual byte count");
        Check(after.requestedBytes - before.requestedBytes == 33 && after.readBytes - before.readBytes == 11 &&
            after.failedReads - before.failedReads == 4, "partial diagnostics counters distinguish four bad requests from five total");
        const auto& partialDetail = after.lastError;
        Check(partialDetail.find("scatter_incomplete: executed=true; prepared=true; pid=77; requests=5; bad_requests=4; expected_bytes=33; actual_bytes=11;") == 0,
            "bounded failure detail reports API result request totals and byte sums");
        Check(partialDetail.find("address=0xABC100,expected=8,actual=7,prepared=true") != std::string::npos &&
            partialDetail.find("address=0xABC200,expected=4,actual=0,prepared=true") != std::string::npos &&
            partialDetail.find("address=0xABC300,expected=16,actual=0,prepared=true") != std::string::npos,
            "bounded failure detail includes first three bad request samples");
        Check(partialDetail.find("0xABC000") == std::string::npos && partialDetail.find("0xABC400") == std::string::npos,
            "bounded failure detail omits complete and fourth failed request addresses");
        size_t addressSamples{}, offset{};
        while ((offset = partialDetail.find("address=", offset)) != std::string::npos) { ++addressSamples; offset += 8; }
        Check(addressSamples == 3, "failed batch address samples are capped at three");
        Fake::shortAddress = 0;
        Check(batch.Clear(), "clear bounded partial fixture");

        Check(batch.Prepare(0xdef100, 3, buffers[0].data()) && batch.Prepare(0xdef200, 7, buffers[1].data()), "API failure diagnostics prepare");
        Fake::executeOk = false;
        Check(!batch.Execute(), "API execution failure rejects diagnostics fixture");
        const auto executeDetail = Diagnostics::GetSnapshot().lastError;
        Check(executeDetail.find("scatter_incomplete: executed=false; prepared=true; pid=77; requests=2; bad_requests=2; expected_bytes=10; actual_bytes=0;") == 0,
            "execution failure detail remains current despite warning throttle");
        Check(executeDetail.find("address=0xDEF100,expected=3,actual=0,prepared=true") != std::string::npos &&
            executeDetail.find("address=0xDEF200,expected=7,actual=0,prepared=true") != std::string::npos,
            "API failure diagnostics mark every request bad even before reading bytes");
        Fake::executeOk = true;
        Check(batch.Clear(), "clear bounded API failure fixture");

        Check(!batch.Prepare(0xe11000, 0, buffers[0].data()), "zero-byte preparation rejected before native API");
        Check(!batch.Execute(), "rejected zero-byte preparation rejects otherwise empty native batch");
        const auto rejectedDetail = Diagnostics::GetSnapshot().lastError;
        Check(rejectedDetail.find("scatter_incomplete: executed=true; prepared=false; pid=77; requests=1; bad_requests=1; expected_bytes=0; actual_bytes=0;") == 0,
            "rejected zero-byte request counts bad despite equal zero byte totals");
        Check(rejectedDetail.find("address=0xE11000,expected=0,actual=0,prepared=false") != std::string::npos,
            "rejected preparation sample records prepared false");
        Check(Fake::requests.empty(), "rejected zero-byte request never reaches native prepare");
        Check(batch.Clear(), "clear bounded rejected preparation fixture");
    }
    Check(Fake::opened == openedBefore + 1 && Fake::closed == closedBefore + 1 && Fake::requests.empty(), "bounded diagnostics fixture balances its scatter handle");
    Fake::remoteFixture = false; Fake::regions.clear();
}
void TestProcessDiscoverySummary() {
    std::array<VMMDLL_PROCESS_INFORMATION, 8> table{};
    auto set = [&](size_t index, DWORD pid, DWORD state, const char* shortName, const char* longName) {
        auto& process = table[index];
        process.dwPID = pid; process.dwState = state;
        strcpy_s(process.szName, shortName); strcpy_s(process.szNameLong, longName);
    };
    set(0, 11, 0, "unrelated.exe", "unrelated.exe");
    set(1, 42, 0, "deadlock.exe", "Deadlock.EXE");
    auto summary = Process::SummarizeDiscovery(table.data(), 2, "deadlock.exe");
    Check(summary.exactActiveMatches == 1 && summary.uniqueCandidate == 42, "discovery selects only one exact active long name");
    Check(summary.activeShortNameMatches == 1 && summary.targetLikeCount == 1 && summary.sampleCount == 1,
        "discovery reports exact short name and omits unrelated processes");
    Check(summary.samples[0].pid == 42 && summary.samples[0].longName == "Deadlock.EXE", "target-only sample preserves bounded useful identity");

    set(2, 43, 0, "deadlock.exe", "");
    summary = Process::SummarizeDiscovery(table.data(), 3, "deadlock.exe");
    Check(summary.exactActiveMatches == 1 && summary.uniqueCandidate == 42 && summary.activeShortNameMatches == 2,
        "short-name-only candidate never relaxes existing selection policy");
    set(3, 44, 1, "deadlock.exe", "deadlock.exe");
    set(4, 45, 0, "citadel.exe", "citadel.exe");
    set(5, 46, 0, "prefix.exe", "other-deadlock.exe");
    set(6, 0, 0, "deadlock.exe", "deadlock.exe");
    set(7, 47, 0, "deadlock.exe", "deadlock.exe");
    summary = Process::SummarizeDiscovery(table.data(), table.size(), "deadlock.exe");
    Check(summary.exactActiveMatches == 2 && summary.uniqueCandidate == 0, "ambiguous active exact processes remain unselected");
    Check(summary.activeShortNameMatches == 3, "terminated and zero-PID entries do not count as active short-name matches");
    Check(summary.targetLikeCount == 7 && summary.sampleCount == 3, "target-like diagnostics are capped at three samples");
    Check(summary.samples[2].pid == 44 && summary.samples[2].state == 1, "target-only details explain terminated candidates");

    std::array<VMMDLL_PROCESS_INFORMATION, 2> malformed{};
    malformed[0].dwPID = 81;
    strcpy_s(malformed[0].szName, "deadlock.exe");
    memset(malformed[0].szNameLong, 'x', sizeof(malformed[0].szNameLong));
    malformed[1].dwPID = 82;
    memset(malformed[1].szName, 'x', sizeof(malformed[1].szName));
    strcpy_s(malformed[1].szNameLong, "CITADEL.exe");
    summary = Process::SummarizeDiscovery(malformed.data(), malformed.size(), "deadlock.exe");
    Check(summary.exactActiveMatches == 0 && summary.uniqueCandidate == 0 && summary.activeShortNameMatches == 1,
        "unterminated long name is never selected even when short name matches");
    Check(summary.targetLikeCount == 2 && summary.sampleCount == 2, "target-like matching is case insensitive and bounded");
    Check(summary.samples[0].longName == "<unterminated>" && summary.samples[1].shortName == "<unterminated>",
        "unterminated fields are diagnosed without copying non-string bytes");
    summary = Process::SummarizeDiscovery(nullptr, 100, "deadlock.exe");
    Check(summary.exactActiveMatches == 0 && summary.sampleCount == 0 && summary.uniqueCandidate == 0, "null native process table is safe");
    summary = Process::SummarizeDiscovery(table.data(), 0, "deadlock.exe");
    Check(summary.exactActiveMatches == 0 && summary.sampleCount == 0, "successful empty native process table is distinguishable and safe");
}
int main(int argc, char** argv) {
    try {
        Check(argc >= 2, "provide test output directory");
        auto directory = std::filesystem::absolute(argv[1]);
        Check(Diagnostics::Initialize(directory), "logger initialized");
        if (argc == 3 && std::string_view(argv[2]) == "crash") RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        TestProcessDiscoverySummary();
        VMMDLL_PROCESS_INFORMATION processFixture{};
        processFixture.dwPID = 42;
        strcpy_s(processFixture.szNameLong, "Deadlock.EXE");
        Check(Process::IsActiveExactProcess(processFixture, 42, "deadlock.exe"), "active exact process name is case insensitive");
        Check(!Process::IsActiveExactProcess(processFixture, 43, "deadlock.exe"), "another PID is not the attached target");
        processFixture.dwState = 1;
        Check(!Process::IsActiveExactProcess(processFixture, 42, "deadlock.exe"), "terminated process is not visible");
        processFixture.dwState = 0;
        strcpy_s(processFixture.szNameLong, "other-deadlock.exe");
        Check(!Process::IsActiveExactProcess(processFixture, 42, "deadlock.exe"), "substring process name is rejected");
        memset(processFixture.szNameLong, 'x', sizeof(processFixture.szNameLong));
        Check(!Process::IsActiveExactProcess(processFixture, 42, "deadlock.exe"), "unterminated process name is rejected safely");
        processFixture.dwPID = 0;
        strcpy_s(processFixture.szNameLong, "deadlock.exe");
        Check(!Process::IsActiveExactProcess(processFixture, 0, "deadlock.exe"), "zero target PID is not visible");
        Diagnostics::Snapshot freshness{};
        freshness.stage = "running"; freshness.dmaConnected = true; freshness.gameVisible = true; freshness.targetPid = 42;
        freshness.gameCheckAgeSeconds = 10;
        Check(Diagnostics::HasFreshGameConnection(freshness), "game probe freshness includes exact ten second boundary");
        freshness.gameCheckAgeSeconds = 10.001;
        Check(!Diagnostics::HasFreshGameConnection(freshness), "stale game probe hides cached target");
        freshness.gameCheckAgeSeconds = -1;
        Check(!Diagnostics::HasFreshGameConnection(freshness), "unprobed target is not visible");
        freshness.gameCheckAgeSeconds = 0; freshness.dmaConnected = false;
        Check(!Diagnostics::HasFreshGameConnection(freshness), "game cannot be visible without DMA transport");
        freshness.dmaConnected = true; freshness.stage = "failed";
        Check(!Diagnostics::HasFreshGameConnection(freshness), "terminal failure does not show old connection");
        Check(!Diagnostics::GetSnapshot().dmaConnected && !Diagnostics::GetSnapshot().gameVisible, "new session starts detached");
        using namespace MemoryPolicy;
        std::vector<uint8_t> data{0xaa, 0xbb, 0xcc};
        Check(FindPattern(data, "\xAA\xBB", "xx") == 0, "match at byte zero");
        Check(FindPattern(data, "\xBB\xCC", "xx") == 1, "last valid offset");
        Check(FindPattern(data, "\xAA\xBB\xCC", "xxx") == 0, "exact buffer length");
        Check(!FindPattern(data, "1234", "xxxx"), "too large pattern");
        Check(!FindPattern(data, "", ""), "empty pattern");
        Check(!FindPattern(data, "a", ""), "mask mismatch");
        Check(!FindPattern(data, "a", "z"), "invalid mask");
        Check(FindPattern(data, "\xAA!\xCC", "x?x") == 0, "wildcard");
        Check(!FindPattern({}, "a", "x"), "empty buffer");
        Check(RipRelative(100, 7, -50) == 57, "signed backward RIP");
        Check(RipRelative(100, 7, 50) == 157, "forward RIP");
        Check(!RipRelative(1, 1, -5), "negative target");
        for (uint32_t i = 0; i < 32768; ++i) Check(ValidEntityHandle(i, 16384) == (i < 16384), "entity boundary");
        Check(!ValidEntityHandle(UINT32_MAX, 16384), "invalid handle sentinel");
        std::mt19937 random(47);
        for (int iteration = 0; iteration < 2000; ++iteration) {
            std::vector<uint8_t> input(random() % 32);
            for (auto& b : input) b = random() % 4;
            std::string pattern(random() % 8, '\0'), mask(pattern.size(), 'x');
            for (size_t j = 0; j < pattern.size(); ++j) { pattern[j] = random() % 4; if (random() % 3 == 0) mask[j] = '?'; }
            std::optional<size_t> expected;
            if (!pattern.empty()) for (size_t i = 0; i < input.size(); ++i) {
                if (pattern.size() > input.size() - i) break;
                bool equal = true;
                for (size_t j = 0; j < pattern.size(); ++j) equal &= mask[j] == '?' || input[i+j] == uint8_t(pattern[j]);
                if (equal) { expected = i; break; }
            }
            Check(FindPattern(input, pattern, mask) == expected, "pattern oracle");
        }
        const auto vmm = reinterpret_cast<VMM_HANDLE>(1);
        { ScatterBatch batch(vmm, 10); std::vector<std::array<BYTE, 4>> buffers(512);
            for (auto& buffer : buffers) Check(batch.Prepare(0x1000, 4, buffer.data()), "scatter prepare");
            Check(batch.Execute(), "complete scatter"); Check(batch.Clear(), "scatter reuse");
            Fake::partial = true; Check(batch.Prepare(0x1000, 4, buffers[0].data()), "prepare partial");
            Check(!batch.Execute(), "short read even when ExecuteRead succeeds"); Fake::partial = false;
            Check(batch.Clear(), "clear partial"); Fake::prepareOk = false;
            Check(!batch.Prepare(0x1000, 4, buffers[0].data()), "prepare failure");
            Check(!batch.Execute(), "failed preparation rejects batch"); Fake::prepareOk = true;
            Check(batch.Clear(), "clear prepare failure");
            Check(batch.Prepare(0x1000, 4, buffers[0].data()), "prepare execution failure"); Fake::executeOk = false;
            Check(!batch.Execute(), "execution failure"); Fake::executeOk = true;
            Fake::clearOk = false; Check(!batch.Clear(), "clear failure"); Fake::clearOk = true;
        }
        Fake::initOk = false;
        { ScatterBatch batch(vmm, 10); BYTE buffer[4]{}; Check(!batch.Prepare(0x1000, 4, buffer), "null handle prepare"); Check(!batch.Execute(), "null handle execute"); }
        Fake::initOk = true;
        TestEntityListPageBoundary(vmm);
        TestBuild6766Layout(vmm);
        TestBoxOnlyPawnReads(vmm);
        TestBoxOnlyControllerReads(vmm);
        TestCameraScatterReads(vmm);
        TestBoundedScatterDiagnostics(vmm);
        Check(Fake::opened == Fake::closed, "all scatter handles closed");
        Check(Diagnostics::OpenScatter == 0, "open scatter gauge balanced");
        Diagnostics::Event("info", "escaping", "quotes\" newline\n UTF-8: тест");
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) threads.emplace_back([] { for (int i = 0; i < 100; ++i) Diagnostics::Warning("rate_test", "concurrent warning"); });
        for (auto& thread : threads) thread.join();
        Diagnostics::SetStage("connecting_dma");
        const auto stageStart = Diagnostics::GetSnapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Diagnostics::SetStage("connecting_dma"); // Re-publishing the same stage must not reset its age.
        const auto stageLater = Diagnostics::GetSnapshot();
        Check(stageStart.stageAgeSeconds >= 0 && stageLater.stageAgeSeconds > stageStart.stageAgeSeconds, "stage age advances");
        Diagnostics::SetStage("waiting_process");
        Check(Diagnostics::GetSnapshot().stageAgeSeconds < stageLater.stageAgeSeconds, "new stage resets its age");
        Diagnostics::SetStage("running", 42); Diagnostics::WorkerTick(75); Diagnostics::FrameTick(); Diagnostics::Entities(12,12,4,5,6);
        Diagnostics::SetGameVisible(true, 42);
        Check(!Diagnostics::GetSnapshot().gameVisible, "target confirmation cannot precede connected DMA");
        Diagnostics::SetDmaConnected(true); Diagnostics::SetGameVisible(true, 42);
        auto connected = Diagnostics::GetSnapshot();
        Check(connected.dmaConnected && connected.gameVisible && connected.gameCheckAgeSeconds >= 0, "fresh target probe is published independently from stage");
        Diagnostics::SetGameVisible(false, 42);
        Check(!Diagnostics::GetSnapshot().gameVisible && Diagnostics::GetSnapshot().dmaConnected, "lost game retains connected transport");
        Diagnostics::SetGameVisible(true, 42); Diagnostics::SetDmaConnected(false);
        Check(!Diagnostics::GetSnapshot().gameVisible && Diagnostics::GetSnapshot().gameCheckAgeSeconds == -1, "disconnect clears stale game confirmation");
        Diagnostics::SetDmaConnected(true); Diagnostics::SetGameVisible(true, 42);
        Diagnostics::PublishHeartbeat();
        auto s = Diagnostics::GetSnapshot(); Check(s.slowLoops == 1 && s.targetPid == 42, "snapshot state");
        std::ifstream status(directory / (s.session + ".status.json")); auto json = nlohmann::json::parse(status); status.close();
        Check(json["failed_reads"].get<uint64_t>() >= 4 && json["open_scatter"] == 0 && json["pawns"] == 12, "heartbeat counters");
        Check(json["stage_age_s"].get<double>() >= 0, "heartbeat publishes stage age");
        Check(json["dma_connected"] == true && json["game_visible"] == true && json["game_check_age_s"].get<double>() >= 0, "heartbeat records separate transport and target status");
        int rateRecords{};
        for (const auto& file : std::filesystem::directory_iterator(directory)) if (file.path().extension() == ".jsonl") {
            std::ifstream input(file.path()); std::string line;
            while (std::getline(input, line)) { auto record = nlohmann::json::parse(line); if (record["event"] == "rate_test") ++rateRecords; }
        }
        Check(rateRecords == 1, "concurrent warning rate limit");
        // Force rotation, verify per-session bound and readable records.
        std::string large(1024 * 1024, 'z');
        for (int i = 0; i < 30; ++i) Diagnostics::Event("info", "rotation_test", large);
        int files{};
        for (const auto& file : std::filesystem::directory_iterator(directory)) if (file.path().extension() == ".jsonl") {
            ++files; Check(file.file_size() <= 4 * 1024 * 1024, "segment bounded");
            std::ifstream input(file.path()); std::string line;
            while (std::getline(input, line)) Check(nlohmann::json::parse(line).contains("utc"), "rotated JSONL valid");
        }
        Check(files == 3, "three segments");
        Diagnostics::SetStage("stopped"); Diagnostics::Shutdown();
        std::cout << "PASS: " << checks << " offline checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
