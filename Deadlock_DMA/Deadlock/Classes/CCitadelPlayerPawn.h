#pragma once
#include <array>
#include <cstddef>
#include <limits>
#include "Deadlock/Engine/CHandle.h"
#include "CBaseEntity.h"
#include "CCitadelPlayerController.h"

constexpr int MAX_BONES = 70;

class CCitadelPlayerPawn : public CBaseEntity
{
public:
	explicit CCitadelPlayerPawn(uintptr_t address) : CBaseEntity(address) {}
	Vector3 m_BonePositions[MAX_BONES]{ 0.0f };
	Vector3 m_Velocity{ 0.0f };
	uintptr_t m_BoneArrayAddress{ 0 };
	CHandle m_hController{ 0 };
	int32_t m_TotalUnspentSouls{ 0 };
	int32_t m_UnsecuredSouls{ 0 };

private:
	// One contiguous request keeps position/dormancy eligible for a tiny read.
	// Multiple requests on the same virtual page can cause a full-page read.
	struct SceneReadRecord
	{
		Vector3 position;
		std::array<uint8_t, Offsets::SceneNode::Dormant - Offsets::SceneNode::Position - sizeof(Vector3)> padding;
		uint8_t dormant;
	};
	static_assert(sizeof(SceneReadRecord) == 60);
	static_assert(offsetof(SceneReadRecord, position) == 0);
	static_assert(offsetof(SceneReadRecord, dormant) == Offsets::SceneNode::Dormant - Offsets::SceneNode::Position);
	SceneReadRecord m_SceneReadBuffer{};
	DWORD m_SceneReadBytes{};
	bool m_SceneReadPrepared{};

	void ResetSceneRead()
	{
		m_SceneReadBytes = 0;
		m_SceneReadPrepared = false;
	}

	void PrepareSceneRead(ScatterBatch& vmsh)
	{
		ResetSceneRead();
		constexpr uintptr_t readEnd = Offsets::SceneNode::Position + sizeof(SceneReadRecord) - 1;
		if (!m_GameSceneNodeAddress || m_GameSceneNodeAddress > (std::numeric_limits<uintptr_t>::max)() - readEnd)
		{
			SetInvalid();
			return;
		}
		m_SceneReadPrepared = vmsh.Prepare(m_GameSceneNodeAddress + Offsets::SceneNode::Position,
			sizeof(SceneReadRecord), reinterpret_cast<BYTE*>(&m_SceneReadBuffer), &m_SceneReadBytes);
	}

	// The remote transform stride is 0x20; only its first Vector3 is rendered.
	// Read the contiguous span once, then publish positions after byte validation.
	struct BoneRecord
	{
		Vector3 position;
		std::array<uint8_t, 0x20 - sizeof(Vector3)> padding;
	};
	static_assert(sizeof(Vector3) == 12 && sizeof(BoneRecord) == 0x20);
	static_assert(offsetof(BoneRecord, position) == 0);
	static constexpr DWORD BoneReadSize = (MAX_BONES - 1) * sizeof(BoneRecord) + sizeof(Vector3);
	std::array<BoneRecord, MAX_BONES> m_BoneReadBuffer{};
	DWORD m_BoneReadBytes{};
	bool m_BoneReadPrepared{};

	void ResetBoneRead()
	{
		m_BoneReadBytes = 0;
		m_BoneReadPrepared = false;
	}

	void PrepareBoneRead(ScatterBatch& vmsh)
	{
		ResetBoneRead();
		if (!m_BoneArrayAddress || m_BoneArrayAddress > (std::numeric_limits<uintptr_t>::max)() - (BoneReadSize - 1))
			return;
		++Diagnostics::BoneReadRequests;
		m_BoneReadPrepared = vmsh.Prepare(m_BoneArrayAddress, BoneReadSize,
			reinterpret_cast<BYTE*>(m_BoneReadBuffer.data()), &m_BoneReadBytes);
	}

public:
	bool CommitSceneRead()
	{
		const bool complete = m_SceneReadPrepared && m_SceneReadBytes == sizeof(SceneReadRecord);
		ResetSceneRead();
		if (!complete) return false;
		m_Position = m_SceneReadBuffer.position;
		m_Dormant = m_SceneReadBuffer.dormant;
		return true;
	}

	bool CommitBoneRead(bool readBones = true)
	{
		const bool complete = readBones && m_BoneReadPrepared && m_BoneReadBytes == BoneReadSize;
		ResetBoneRead();
		if (!complete) return false;
		for (int i = 0; i < MAX_BONES; ++i)
			m_BonePositions[i] = m_BoneReadBuffer[i].position;
		return true;
	}

	void PrepareRead_1(ScatterBatch& vmsh, bool readVelocity = true, bool readSouls = true)
	{
		ResetSceneRead();
		ResetBoneRead();
		CBaseEntity::PrepareRead_1(vmsh, false);
		if (IsInvalid())
			return;

		uintptr_t hControllerPtr = m_EntityAddress + Offsets::Pawn::hController;
		vmsh.Prepare(hControllerPtr, sizeof(uint32_t), reinterpret_cast<BYTE*>(&m_hController.Data), nullptr);

		if (readSouls)
		{
			uintptr_t UnsecuredSoulsPtr = m_EntityAddress + Offsets::Pawn::UnsecuredSouls;
			vmsh.Prepare(UnsecuredSoulsPtr, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_UnsecuredSouls), nullptr);

			uintptr_t TotalUnspentSoulsPtr = m_EntityAddress + Offsets::Pawn::TotalUnspentSouls;
			vmsh.Prepare(TotalUnspentSoulsPtr, sizeof(int32_t), reinterpret_cast<BYTE*>(&m_TotalUnspentSouls), nullptr);
		}

		if (readVelocity)
		{
			uintptr_t VelocityPtr = m_EntityAddress + Offsets::Pawn::Velocity;
			vmsh.Prepare(VelocityPtr, sizeof(Vector3), reinterpret_cast<BYTE*>(&m_Velocity), nullptr);
		}
	}

	void PrepareRead_2(ScatterBatch& vmsh, bool readBones = true)
	{
		ResetSceneRead();
		ResetBoneRead();
		if (!m_GameSceneNodeAddress)
			SetInvalid();
		if (IsInvalid())
			return;
		if (!readBones)
		{
			PrepareSceneRead(vmsh);
			return;
		}

		CBaseEntity::PrepareRead_2(vmsh);

		uintptr_t BoneArrayPtrAddress = m_GameSceneNodeAddress + Offsets::SceneNode::ModelState + Offsets::ModelState::BoneArrayPtr;
		vmsh.Prepare(BoneArrayPtrAddress, sizeof(uintptr_t), reinterpret_cast<BYTE*>(&m_BoneArrayAddress), nullptr);
	}

	void PrepareRead_3(ScatterBatch& vmsh, bool readBones = true)
	{
		ResetBoneRead();
		if (IsInvalid() || !readBones)
			return;

		PrepareBoneRead(vmsh);
	}
	void QuickRead(ScatterBatch& vmsh, bool readBones = true, bool readVelocity = true)
	{
		ResetSceneRead();
		ResetBoneRead();
		if (!m_GameSceneNodeAddress)
			SetInvalid();
		if (IsInvalid())
			return;
		if (!readBones && !readVelocity)
		{
			PrepareSceneRead(vmsh);
			return;
		}

		CBaseEntity::QuickRead(vmsh, false);

		if (readVelocity)
		{
			uintptr_t VelocityPtr = m_EntityAddress + Offsets::Pawn::Velocity;
			vmsh.Prepare(VelocityPtr, sizeof(Vector3), reinterpret_cast<BYTE*>(&m_Velocity), nullptr);
		}

		if (readBones)
			PrepareBoneRead(vmsh);
	}
};
