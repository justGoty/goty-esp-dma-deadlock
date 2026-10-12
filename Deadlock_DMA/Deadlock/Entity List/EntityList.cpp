#include "pch.h"

#include "EntityList.h"
#include "GUI/Fuser/ESP/ESP.h"

#include "GUI/Query.h"

void EntityList::FullUpdate(DMA_Connection* Conn, Process* Proc)
{
	ZoneScoped;

	if (!UpdateCrucialInformation(Conn, Proc)) return;
	if (!UpdateEntityMap(Conn, Proc)) return;
	if (!UpdateEntityClassMap(Conn, Proc)) return;
	SortEntityList();
	Diagnostics::Entities(m_PlayerPawn_Addresses.size(), m_PlayerController_Addresses.size(), m_TrooperAddresses.size(), m_MonsterCampAddresses.size(), m_SinnersAddresses.size());
}

bool EntityList::UpdateCrucialInformation(DMA_Connection* Conn, Process* Proc)
{
	return GetEntitySystemAddress(Conn, Proc) && GetEntityListAddresses(Conn, Proc);
}

bool EntityList::GetEntitySystemAddress(DMA_Connection* Conn, Process* Proc)
{
	uintptr_t EntitySystemPointer = Proc->GetModuleAddress("client.dll") + Offsets::GameEntitySystem;
	uintptr_t LatestAddr{};
	if (!Proc->TryRead(Conn, EntitySystemPointer, LatestAddr)) return false;
	if (!LatestAddr) {
		Diagnostics::Warning("entity_system_missing", "Entity system is unavailable; retaining prior discovery cache");
		return false;
	}

	if (LatestAddr == m_EntitySystem_Address) return true;

	m_EntitySystem_Address = LatestAddr;

	std::println("Entity System Address: 0x{:X}", m_EntitySystem_Address);
	return true;
}

bool EntityList::GetEntityListAddresses(DMA_Connection* Conn, Process* Proc)
{
	if (!m_EntitySystem_Address) { Diagnostics::Warning("entity_system_missing", "Menu, unreadable pointer or incompatible game layout"); return false; }
	decltype(m_EntityList_Addresses) next{};

	uintptr_t StartEntityListArray = m_EntitySystem_Address + 0x10;

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (int i = 0; i < MAX_ENTITY_LISTS; i++)
	{
		auto& WriteAddr = next[i];
		auto Addr = StartEntityListArray + (i * sizeof(uintptr_t));
		vmsh.Prepare(Addr, sizeof(uintptr_t), reinterpret_cast<BYTE*>(&WriteAddr), nullptr);
	}

	if (!vmsh.Execute()) return false;

	// ScatterBatch closes the handle on every return path.
	if (std::none_of(next.begin(), next.end(), [](uintptr_t address) { return address != 0; })) {
		Diagnostics::Warning("entity_lists_missing", "No active entity lists; retaining prior discovery cache");
		return false;
	}
	m_EntityList_Addresses = next;
	return true;
}

bool EntityList::UpdateEntityMap(DMA_Connection* Conn, Process* Proc)
{
	if (std::none_of(m_EntityList_Addresses.begin(), m_EntityList_Addresses.end(), [](uintptr_t address) { return address != 0; }))
		return false;
	// Heap storage avoids a large stack allocation and retains the last complete handle map.
	auto next = std::make_unique<decltype(m_CompleteEntityList)>();

	static_assert(EntityListLayout::ReadBytes(MAX_ENTITIES) > 0 && EntityListLayout::ReadBytes(MAX_ENTITIES) <= MAXDWORD);
	constexpr DWORD EntityListSize = static_cast<DWORD>(EntityListLayout::ReadBytes(MAX_ENTITIES));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (int i = 0; i < MAX_ENTITY_LISTS; i++)
	{
		auto& Addr = m_EntityList_Addresses[i];
		auto& WriteAddr = (*next)[i][0];

		if (Addr == 0) continue;

		vmsh.Prepare(Addr, EntityListSize, reinterpret_cast<BYTE*>(&WriteAddr), nullptr);
	}

	if (!vmsh.Execute()) return false;

	// ScatterBatch closes the handle on every return path.

	{
		std::scoped_lock publish(m_PawnMutex, m_ControllerMutex);
		m_CompleteEntityList.swap(*next);
	}
	DbgPrintln("Entity Map Updated.");
	return true;
}

void EntityList::SortEntityList()
{
	std::scoped_lock classLock(m_ClassMapMutex);
	m_TrooperAddresses.clear();
	m_PlayerPawn_Addresses.clear();
	m_PlayerController_Addresses.clear();
	m_MonsterCampAddresses.clear();
	m_SinnersAddresses.clear();

	uintptr_t PlayerControllerClassPtr = m_EntityClassMap["citadel_player_controller"];
	uintptr_t PlayerPawnClassPtr = m_EntityClassMap["player"];

	uintptr_t TrooperClassPtr = 0;
	if (m_EntityClassMap.find("npc_trooper") != m_EntityClassMap.end())
		TrooperClassPtr = m_EntityClassMap["npc_trooper"];

	uintptr_t MonsterCampClassPtr = 0;
	if (m_EntityClassMap.find("npc_trooper_neutral") != m_EntityClassMap.end())
		MonsterCampClassPtr = m_EntityClassMap["npc_trooper_neutral"];

	uintptr_t SinnerClassPtr = 0;
	if (m_EntityClassMap.find("npc_neutral_sinners_sacrifice") != m_EntityClassMap.end())
		SinnerClassPtr = m_EntityClassMap["npc_neutral_sinners_sacrifice"];

	for (auto& List : m_CompleteEntityList)
	{
		for (auto& Entry : List)
		{
			if (!Entry.pEnt || !Entry.pName) continue;
			if (TrooperClassPtr && Entry.pName == TrooperClassPtr) m_TrooperAddresses.push_back(Entry.pEnt);
			else if (Entry.pName == PlayerControllerClassPtr) m_PlayerController_Addresses.push_back(Entry.pEnt);
			else if (Entry.pName == PlayerPawnClassPtr) m_PlayerPawn_Addresses.push_back(Entry.pEnt);
			else if (MonsterCampClassPtr && Entry.pName == MonsterCampClassPtr) m_MonsterCampAddresses.push_back(Entry.pEnt);
			else if (SinnerClassPtr && Entry.pName == SinnerClassPtr) m_SinnersAddresses.push_back(Entry.pEnt);
			else continue;
		}
	}
}

void EntityList::FullControllerRefresh_lk(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingPlayers() == false) return;

	ZoneScoped;

	FullControllerRefresh(Conn, Proc);
}

void EntityList::FullControllerRefresh(DMA_Connection* Conn, Process* Proc)
{
	const bool minimal = Query::IsUsingMinimalControllers();
	int32_t localIndex = -1;
	std::vector<CCitadelPlayerController> next;
	next.reserve(m_PlayerController_Addresses.size());

	for (auto& addr : m_PlayerController_Addresses)
		next.emplace_back(CCitadelPlayerController(addr));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& PC : next)
		PC.PrepareRead_1(vmsh, minimal);

	if (!vmsh.Execute()) return;

	if (!minimal) {
		if (!vmsh.Clear()) return;
		for (auto& PC : next) PC.PrepareRead_2(vmsh);
		if (!vmsh.Execute()) return;
	}

	// ScatterBatch closes the handle on every return path.

	for (int i = 0; i < next.size(); i++)
	{
		if (next[i].IsLocalPlayer())
		{
			localIndex = i;

			break;
		}
		else if (i == next.size() - 1)
		{
			localIndex = -1;
		}
	}
	{
		std::scoped_lock publish(m_ControllerMutex);
		m_PlayerControllers.swap(next);
		m_LocalControllerIndex = localIndex;
	}

}

void EntityList::QuickControllerRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingPlayers() == false) return;

	ZoneScoped;

	std::vector<CCitadelPlayerController> next;
	{
		std::scoped_lock snapshot(m_ControllerMutex);
		next = m_PlayerControllers;
	}

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& PC : next)
		PC.QuickRead(vmsh);

	if (!vmsh.Execute()) return;

	// ScatterBatch closes the handle on every return path.
	{
		std::scoped_lock publish(m_ControllerMutex);
		m_PlayerControllers.swap(next);
	}

}

void EntityList::FullPawnRefresh_lk(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingPlayers() == false) return;

	ZoneScoped;

	FullPawnRefresh(Conn, Proc);
}

void EntityList::FullPawnRefresh(DMA_Connection* Conn, Process* Proc)
{
	ZoneScoped;

	const bool readBones = Query::IsUsingBones();
	const bool readVelocity = Query::IsUsingVelocity();
	const bool readSouls = Query::IsUsingPlayerSouls();
	int32_t localIndex = -1;
	std::vector<CCitadelPlayerPawn> next;
	next.reserve(m_PlayerPawn_Addresses.size());

	for (auto& addr : m_PlayerPawn_Addresses)
		next.emplace_back(CCitadelPlayerPawn(addr));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Pawn : next)
		Pawn.PrepareRead_1(vmsh, readVelocity, readSouls);

	if (!vmsh.Execute()) return;

	if (!vmsh.Clear()) return;

	for (auto& Pawn : next)
		Pawn.PrepareRead_2(vmsh, readBones);

	if (!vmsh.Execute()) return;
	for (auto& pawn : next) pawn.CommitSceneRead();

	if (readBones) {
		if (!vmsh.Clear()) return;
		for (auto& Pawn : next) Pawn.PrepareRead_3(vmsh, true);
		if (!vmsh.Execute()) return;
	}
	for (auto& pawn : next) pawn.CommitBoneRead(readBones);

	// ScatterBatch closes the handle on every return path.

	for (int i = 0; i < next.size(); i++)
	{
		if (next[i].IsLocalPlayer())
		{
			localIndex = i;
			break;
		}
		else if (i == next.size() - 1)
		{
			localIndex = -1;
		}
	}
	{
		std::scoped_lock publish(m_PawnMutex);
		m_PlayerPawns.swap(next);
		m_LocalPawnIndex = localIndex;
	}
	Diagnostics::PawnPublished();

}

void EntityList::QuickPawnRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingPlayers() == false) return;

	ZoneScoped;

	const bool readBones = Query::IsUsingBones();
	const bool readVelocity = Query::IsUsingVelocity();
	std::vector<CCitadelPlayerPawn> next;
	{
		std::scoped_lock snapshot(m_PawnMutex);
		next = m_PlayerPawns;
	}

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Pawn : next)
		Pawn.QuickRead(vmsh, readBones, readVelocity);

	if (!vmsh.Execute()) return;

	for (auto& pawn : next) {
		pawn.CommitSceneRead();
		pawn.CommitBoneRead(readBones);
	}
	{
		std::scoped_lock publish(m_PawnMutex);
		m_PlayerPawns.swap(next);
	}
	Diagnostics::PawnPublished();

}

void EntityList::FullMonsterCampRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingCamps() == false) return;

	ZoneScoped;

	std::scoped_lock Lock(m_MonsterCampMutex);

	m_MonsterCamps.clear();

	for (auto& addr : m_MonsterCampAddresses)
		m_MonsterCamps.emplace_back(CBaseEntity(addr));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Camp : m_MonsterCamps)
		Camp.PrepareRead_1(vmsh);

	if (!vmsh.Execute()) return;

	if (!vmsh.Clear()) return;

	for (auto& Camp : m_MonsterCamps)
		Camp.PrepareRead_2(vmsh);

	if (!vmsh.Execute()) return;

	// ScatterBatch closes the handle on every return path.
}

void EntityList::QuickMonsterCampRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingCamps() == false) return;

	ZoneScoped;

	std::scoped_lock Lock(m_MonsterCampMutex);

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Camp : m_MonsterCamps)
		Camp.QuickRead(vmsh);

	if (!vmsh.Execute()) return;

	if (!vmsh.Clear()) return;
}

void EntityList::FullTrooperRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingTroopers() == false) return;

	ZoneScoped;

	std::scoped_lock Lock(m_TrooperMutex);

	m_Troopers.clear();

	for (auto& addr : m_TrooperAddresses)
		m_Troopers.emplace_back(CTrooperEntity(addr));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Trooper : m_Troopers)
		Trooper.PrepareRead_1(vmsh);

	if (!vmsh.Execute()) return;

	if (!vmsh.Clear()) return;

	for (auto& Trooper : m_Troopers)
		Trooper.PrepareRead_2(vmsh);

	if (!vmsh.Execute()) return;

	// ScatterBatch closes the handle on every return path.

	DbgPrintln("Trooper List Refreshed. Count: {}", m_Troopers.size());
}

void EntityList::QuickTrooperRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingTroopers() == false) return;

	ZoneScoped;

	std::scoped_lock Lock(m_TrooperMutex);

	if (m_Troopers.empty()) return;

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Trooper : m_Troopers)
		Trooper.QuickRead(vmsh);

	if (!vmsh.Execute()) return;

	// ScatterBatch closes the handle on every return path.
}

void EntityList::FullSinnerRefresh(DMA_Connection* Conn, Process* Proc)
{
	if (Query::IsUsingSinners() == false) return;

	ZoneScoped;

	std::scoped_lock Lock(m_SinnerMutex);

	m_Sinners.clear();

	for (auto& addr : m_SinnersAddresses)
		m_Sinners.emplace_back(CBaseEntity(addr));

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (auto& Sinner : m_Sinners)
		Sinner.PrepareRead_1(vmsh);

	if (!vmsh.Execute()) return;

	if (!vmsh.Clear()) return;

	for (auto& Sinner : m_Sinners)
		Sinner.PrepareRead_2(vmsh);

	if (!vmsh.Execute()) return;

	// ScatterBatch closes the handle on every return path.
}

uintptr_t EntityList::GetEntityAddressFromHandle(CHandle Handle)
{
	if (!Handle.IsValid()) return 0;

	auto ListIndex = Handle.GetEntityListIndex();
	auto EntityIndex = Handle.GetEntityEntryIndex();

	return m_CompleteEntityList[ListIndex][EntityIndex].pEnt;
}

bool EntityList::UpdateEntityClassMap(DMA_Connection* Conn, Process* Proc)
{
	std::vector<uintptr_t> UniqueClassNames{};

	for (auto& List : m_CompleteEntityList)
	{
		for (auto& Entry : List)
		{
			if (Entry.pEnt == 0 || Entry.pName == 0) continue;

			if (std::find(UniqueClassNames.begin(), UniqueClassNames.end(), Entry.pName) != UniqueClassNames.end())
				continue;

			UniqueClassNames.push_back(Entry.pName);
		}
	}

	if (UniqueClassNames.empty()) {
		Diagnostics::Warning("entity_class_names_missing", "No entity class names; retaining prior class discovery cache");
		return false;
	}
	struct NameBuffer
	{
		char Name[64]{ 0 };
	};

	auto Buffer = std::make_unique<NameBuffer[]>(UniqueClassNames.size());

	ScatterBatch vmsh(Conn->GetHandle(), Proc->GetPID());

	for (int i = 0; i < UniqueClassNames.size(); i++)
	{
		auto& NamePtr = UniqueClassNames[i];
		vmsh.Prepare(NamePtr, sizeof(NameBuffer), reinterpret_cast<BYTE*>(&Buffer.get()[i]), nullptr);
	}

	if (!vmsh.Execute()) return false;

	// ScatterBatch closes the handle on every return path.

	decltype(m_EntityClassMap) next;
	for (int i = 0; i < UniqueClassNames.size(); i++)
	{
		auto& NamePtr = UniqueClassNames[i];

		auto& rawName = Buffer.get()[i].Name;
		auto length = strnlen_s(rawName, sizeof(rawName));
		if (length == sizeof(rawName)) { Diagnostics::Warning("class_name_unterminated", "Skipped 64-byte class name"); continue; }
		std::string Name(rawName, length);

		if (Name.empty()) continue;

		next[Name] = NamePtr;
	}

	if (next.empty()) {
		Diagnostics::Warning("entity_class_names_missing", "No valid entity class names; retaining prior class discovery cache");
		return false;
	}
	{
		std::scoped_lock publish(m_ClassMapMutex);
		m_EntityClassMap.swap(next);
	}
	DbgPrintln("Entity Class Map Updated.");
	return true;
}

void EntityList::PrintPlayerControllerAddresses()
{
	for (auto& Addr : m_PlayerController_Addresses)
		std::println("PlayerController: 0x{:X}", Addr);
}

void EntityList::PrintPlayerControllers()
{
	if (m_PlayerControllers.empty())
	{
		std::println("No PlayerControllers found.");
		return;
	}


	for (auto& PC : m_PlayerControllers)
	{
		if (PC.IsInvalid()) continue;

		auto PawnIt = std::find(m_PlayerPawns.begin(), m_PlayerPawns.end(), EntityList::GetEntityAddressFromHandle(PC.m_hPawn));

		if (PawnIt == std::end(m_PlayerPawns)) continue;

		if (PawnIt->IsInvalid()) continue;

		std::println("Player found! hPawn: {0:d}, hController {1:d}\n  Pawn @ {2:.0f} with {3:d} hp", PC.m_hPawn.GetEntityEntryIndex(), PawnIt->m_hController.GetEntityEntryIndex(), PawnIt->m_BonePositions[0].z, PC.m_CurrentHealth);
	}
}

void EntityList::PrintPlayerPawns()
{
	for (auto& Pawn : m_PlayerPawns)
		std::println("PlayerPawn: 0x{0:X} | GameSceneNode: {1:X}", Pawn.m_EntityAddress, Pawn.m_GameSceneNodeAddress);
}

void EntityList::PrintClassMap()
{
	for (auto& [name, addr] : m_EntityClassMap)
		std::println("Class: {} | Address: 0x{:X}", name, addr);
}

ETeam EntityList::GetLocalPlayerTeam()
{
	auto Return = ETeam::UNKNOWN;

	std::scoped_lock Lock(m_PawnMutex);

	if (m_LocalPawnIndex < 0 || static_cast<size_t>(m_LocalPawnIndex) >= m_PlayerPawns.size())
		return Return;

	return m_PlayerPawns[m_LocalPawnIndex].m_TeamNum;
}

Vector3 EntityList::GetLocalPawnPosition()
{
	auto Return = Vector3();

	std::scoped_lock Lock(m_PawnMutex);

	if (m_LocalPawnIndex < 0 || static_cast<size_t>(m_LocalPawnIndex) >= m_PlayerPawns.size())
		return Return;

	return m_PlayerPawns[m_LocalPawnIndex].m_Position;
}

/* m_ControllerMutex and m_PawnMutex must be locked (the latter also protects the handle map). */
CCitadelPlayerController* EntityList::GetAssociatedPC(const CCitadelPlayerPawn& Pawn)
{
	auto PCAddress = EntityList::GetEntityAddressFromHandle(Pawn.m_hController);

	for (auto& PC : m_PlayerControllers)
	{
		if (PC.m_EntityAddress == PCAddress)
			return &PC;
	}

	return nullptr;
}
