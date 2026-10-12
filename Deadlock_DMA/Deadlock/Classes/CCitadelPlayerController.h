#pragma once
#include "CBaseEntity.h"
#include "Deadlock/Engine/CHandle.h"
#include "Deadlock/Const/HeroMap.hpp"
#include "Deadlock/Const/HeroEnum.hpp"

class CCitadelPlayerController : public CBaseEntity
{
public:
	CHandle m_hPawn{ 0 };
	int32_t m_CurrentLevel{ 0 };
	int32_t m_MaxHealth{ 0 };
	int32_t m_TotalSouls{ 0 };
	HeroId m_HeroID{ 0 };

public:
	const bool IsDead() const { return m_CurrentHealth <= 0; }
	const std::string_view GetHeroName() const
	{
		auto it = HeroNames::HeroNameMap.find(m_HeroID);

		if (it != HeroNames::HeroNameMap.end())
			return it->second;

		return "Unknown";
	}

public:
	void PrepareRead_1(ScatterBatch& vmsh, bool minimal = false)
	{
		if (minimal) {
			if (!m_EntityAddress) SetInvalid();
			if (IsInvalid()) return;
			vmsh.Prepare(m_EntityAddress + Offsets::BaseEntity::TeamNum, sizeof(uint8_t), reinterpret_cast<BYTE*>(&m_TeamNum));
			vmsh.Prepare(m_EntityAddress + Offsets::Controller::m_hPawn, sizeof(CHandle), reinterpret_cast<BYTE*>(&m_hPawn));
			vmsh.Prepare(m_EntityAddress + Offsets::Controller::PlayerData + Offsets::Controller::PlayerDataOffsets::CurrentHealth,
				sizeof(int32_t), reinterpret_cast<BYTE*>(&m_CurrentHealth));
			return;
		}
		CBaseEntity::PrepareRead_1(vmsh, false);

		if (IsInvalid())
			return;

		uintptr_t PawnHandleAddress = m_EntityAddress + Offsets::Controller::m_hPawn;
		vmsh.Prepare(PawnHandleAddress, sizeof(CHandle), reinterpret_cast<BYTE*>(&m_hPawn), nullptr);

		uintptr_t PlayerDataAddress = m_EntityAddress + Offsets::Controller::PlayerData;

		uintptr_t LevelAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::Level;
		vmsh.Prepare(LevelAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_CurrentLevel), nullptr);

		uintptr_t MaxHealthAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::MaxHealth;
		vmsh.Prepare(MaxHealthAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_MaxHealth), nullptr);

		uintptr_t CurrentHealthAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::CurrentHealth;
		vmsh.Prepare(CurrentHealthAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_CurrentHealth), nullptr);

		uintptr_t HeroIDAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::HeroID;
		vmsh.Prepare(HeroIDAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_HeroID), nullptr);

		uintptr_t TotalSoulsAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::TotalSouls;
		vmsh.Prepare(TotalSoulsAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_TotalSouls), nullptr);
	}
	void PrepareRead_2(ScatterBatch& vmsh, bool readSpatial = true)
	{
		if (readSpatial) CBaseEntity::PrepareRead_2(vmsh);
	}
	void QuickRead(ScatterBatch& vmsh)
	{
		if (IsInvalid())
			return;

		CBaseEntity::QuickRead(vmsh, false);

		uintptr_t PlayerDataAddress = m_EntityAddress + Offsets::Controller::PlayerData;

		uintptr_t CurrentHealthAddress = PlayerDataAddress + Offsets::Controller::PlayerDataOffsets::CurrentHealth;
		vmsh.Prepare(CurrentHealthAddress, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_CurrentHealth), nullptr);
	}
private:

};
