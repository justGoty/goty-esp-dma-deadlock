#pragma once
#include <cstddef>
#include <cstdint>

// Windows client schema: build 6766, SourceRevision 11106493, Oct 08 2026.
// Provenance and remaining manual fields: OFFSETS_RESEARCH.md.
// This describes the expected layout; it does not identify the attached game's build.
namespace Offsets {
inline constexpr uint32_t TargetClientVersion = 6766;
inline constexpr uint32_t SourceRevision = 11106493;
inline constexpr char LayoutTag[] = "windows-6766-rev11106493";
inline constexpr char SchemaDate[] = "2026-10-08";

inline constexpr std::ptrdiff_t FirstEntityList = 0x10; // Manual, not a schema field.
namespace Prediction {
inline constexpr std::ptrdiff_t ServerTime = 0x68; // Manual, not a schema field.
}
namespace SceneNode {
inline constexpr std::ptrdiff_t Position = 0xC8; // CGameSceneNode.m_vecAbsOrigin, VectorWS.
inline constexpr std::ptrdiff_t Dormant = 0x103;
inline constexpr std::ptrdiff_t ModelState = 0x140; // CSkeletonInstance.m_modelState.
}
namespace ModelState {
inline constexpr std::ptrdiff_t BoneArrayPtr = 0x80; // Manual, not a schema field.
}
enum class Currency : size_t {
    Gold = 0, AbilityPoints = 1, AbilityUnlocks = 2, DeathPenaltyGold = 3,
    ItemDraftRerolls = 4, ItemEnhancements = 5, Count = 6
};
namespace Pawn {
// Read the ordinary 12-byte absolute Vector. m_vecVelocity at 0x438 is a 40-byte wrapper.
inline constexpr std::ptrdiff_t Velocity = 0x400; // C_BaseEntity.m_vecAbsVelocity.
inline constexpr std::ptrdiff_t hController = 0x1050; // C_BasePlayerPawn.m_hController.
inline constexpr std::ptrdiff_t Currencies = 0x1234; // C_CitadelPlayerPawn.m_nCurrencies: int32[6].
inline constexpr size_t CurrencyCount = static_cast<size_t>(Currency::Count);
// Retain existing UI/config names; these mappings still need comparison with the game HUD.
inline constexpr std::ptrdiff_t TotalUnspentSouls = Currencies + static_cast<std::ptrdiff_t>(Currency::Gold) * sizeof(int32_t);
inline constexpr std::ptrdiff_t UnsecuredSouls = Currencies + static_cast<std::ptrdiff_t>(Currency::DeathPenaltyGold) * sizeof(int32_t);
}
namespace BaseEntity {
inline constexpr std::ptrdiff_t GameSceneNode = 0x330;
inline constexpr std::ptrdiff_t CurrentHealth = 0x354;
inline constexpr std::ptrdiff_t TeamNum = 0x3EF; // uint8.
}
namespace Controller {
inline constexpr std::ptrdiff_t m_hPawn = 0x8C0; // CCitadelPlayerController.m_hHeroPawn, CHandle (4 bytes).
inline constexpr std::ptrdiff_t PlayerData = 0x908; // Embedded PlayerDataGlobal_t, not a pointer.
namespace PlayerDataOffsets {
inline constexpr std::ptrdiff_t Level = 0x8;
inline constexpr std::ptrdiff_t MaxHealth = 0x10;
inline constexpr std::ptrdiff_t HeroID = 0x1C;
inline constexpr std::ptrdiff_t TotalSouls = 0x2C; // m_iGoldNetWorth; HUD semantics need live validation.
inline constexpr std::ptrdiff_t CurrentHealth = 0x54;
}
}
}
