#include "pch.h"
#include "Offsets.h"
#include "Deadlock.h"
#include "DMA/MemoryPolicy.h"
#include "Diagnostics/Diagnostics.h"
#include "GlobalPatterns.h"
namespace {
bool Resolve(MultiScan& scan, DMA_Connection* conn, const char* name, const PatternInfo& pattern,
    size_t displacementOffset, size_t instructionLength, ptrdiff_t& result) {
    result = 0; auto instruction = scan.ScanOffset(pattern);
    if (!instruction) { Diagnostics::Event("error", "offset_pattern_failed", name); return false; }
    int32_t displacement{};
    if (!Deadlock::Proc().TryRead(conn, scan.GetModuleBase() + instruction + displacementOffset, displacement)) return false;
    auto target = MemoryPolicy::RipRelative(instruction, instructionLength, displacement);
    if (!target) { Diagnostics::Event("error", "offset_target_invalid", name); return false; }
    result = *target; Diagnostics::Event("info", "offset_resolved", std::format("{}=0x{:X} address=0x{:X} instruction_rva=0x{:X}", name, result, scan.GetModuleBase() + result, instruction)); return true;
}
}
void Offsets::LogLayout() {
    Diagnostics::Event("info", "offset_layout", std::format(
        "profile={} target_build={} source_revision={} schema_date={} live_verified=false",
        LayoutTag, TargetClientVersion, SourceRevision, SchemaDate));
    Diagnostics::Event("info", "offset_fields", std::format(
        "hero_pawn=0x{:X} player_data=0x{:X} pawn_controller=0x{:X} team=0x{:X} model_state=0x{:X} health=0x{:X} net_worth=0x{:X} abs_velocity=0x{:X} gold[0]=0x{:X} death_penalty_gold[3]=0x{:X}",
        Controller::m_hPawn, Controller::PlayerData, Pawn::hController, BaseEntity::TeamNum,
        SceneNode::ModelState, Controller::PlayerDataOffsets::CurrentHealth, Controller::PlayerDataOffsets::TotalSouls,
        Pawn::Velocity, Pawn::TotalUnspentSouls, Pawn::UnsecuredSouls));
    Diagnostics::Event("info", "offset_manual_fields", std::format(
        "first_entity_list=0x{:X} bone_array_ptr=0x{:X} prediction_server_time=0x{:X}; not schema-verified",
        FirstEntityList, ModelState::BoneArrayPtr, Prediction::ServerTime));
    Diagnostics::Event("info", "offset_patterns", "ViewMatrix and PredictionInstance: e127620 / 2026-10-02 LEA object targets; unique match required; no RVA fallback");
}
bool Offsets::ResolveOffsets(DMA_Connection* conn) {
    GameEntitySystem = LocalController = ViewMatrix = PredictionInstance = 0;
    Diagnostics::SetStage("resolving_offsets", Deadlock::Proc().GetPID()); MultiScan scan;
    if (!scan.Initialize(conn->GetHandle(), Deadlock::Proc().GetPID(), "client.dll")) return false;
    bool ok = ResolveLocalController(scan, conn); ok = ResolveGameEntitySystem(scan, conn) && ok;
    ok = ResolveViewMatrix(scan, conn) && ok; ok = ResolvePredictionInstance(scan, conn) && ok; scan.Close();
    if (!ok) { GameEntitySystem = LocalController = ViewMatrix = PredictionInstance = 0;
        Diagnostics::Event("error", "offset_resolution_failed", "Initialization stopped; no fallback offsets used"); }
    return ok;
}
bool Offsets::ResolveGameEntitySystem(MultiScan& scan, DMA_Connection* conn) {
    return Resolve(scan, conn, "GameEntitySystem", {"\x48\x8B\x0D????\x8B\xFD", "xxx????xx", "client.dll"}, 3, 7, GameEntitySystem);
}
bool Offsets::ResolveLocalController(MultiScan& scan, DMA_Connection* conn) {
    // iqsell/deadlock-dumper d129f6f, src/analysis.cpp, build 6701 / Sep 28 dump.
    // MOV loads from a global controller-pointer slot, matching UpdateLocalPlayerAddresses.
    // Candidate for build 6766: the scanner requires a unique match; no RVA fallback.
    return Resolve(scan, conn, "LocalController", {"\x48\x8B\x05????\x89\x9E", "xxx????xx", "client.dll"}, 3, 7, LocalController);
}
bool Offsets::ResolveViewMatrix(MultiScan& scan, DMA_Connection* conn) {
    const auto& pattern = GlobalPatterns::ViewMatrix;
    return Resolve(scan, conn, "ViewMatrix", {std::string(pattern.bytes), std::string(pattern.mask), "client.dll"},
        pattern.displacementOffset, pattern.instructionLength, ViewMatrix);
}
bool Offsets::ResolvePredictionInstance(MultiScan& scan, DMA_Connection* conn) {
    const auto& pattern = GlobalPatterns::PredictionInstance;
    return Resolve(scan, conn, "PredictionInstance", {std::string(pattern.bytes), std::string(pattern.mask), "client.dll"},
        pattern.displacementOffset, pattern.instructionLength, PredictionInstance);
}
