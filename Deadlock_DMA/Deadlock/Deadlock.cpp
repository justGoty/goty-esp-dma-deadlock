#include "pch.h"

#include "Deadlock.h"

#include "Entity List/EntityList.h"

#include "GUI/Fuser/Fuser.h"

#include "DMA/Input Manager.h"
#include "GlobalPatterns.h"

bool Deadlock::Initialize(DMA_Connection* Conn)
{


	auto& Process = Deadlock::Proc();

	if (!Process.GetProcessInfo("deadlock.exe", Conn)) return false;

	if (!Offsets::ResolveOffsets(Conn)) return false;

	EntityList::FullUpdate(Conn, &Process);

	UpdateLocalPlayerAddresses(Conn);

	GetPredictionAddress();

	DbgPrintln("Deadlock Initialized.");

	return true;
}

Process& Deadlock::Proc()
{
	return m_DeadlockProc;
}

void Deadlock::UpdateViewMatrix(DMA_Connection* Conn)
{
	ZoneScoped;

	uintptr_t ViewMatrixAddress = Proc().GetClientBase() + Offsets::ViewMatrix;
	Matrix44 matrix{};
	// MemReadEx rounds this small hot read up to a full page. A single scatter
	// request permits MemProcFS's small-read path without disabling cache refresh.
	const bool read = Conn && ScatterBatch::ReadValue(Conn->GetHandle(), Proc().GetPID(), ViewMatrixAddress, matrix);
	// Hardware latency must never hold the renderer's camera lock.
	std::scoped_lock lock(ViewMatrixMutex);
	if (!read) { m_ViewMatrix = {}; return; }
	m_ViewMatrix = matrix;
}

Matrix44 Deadlock::GetViewMatrix()
{
	std::scoped_lock lock(ViewMatrixMutex);
	return m_ViewMatrix;
}

bool Deadlock::WorldToScreen(const Vector3& Pos, Vector2& ScreenPos)
{
	std::scoped_lock lock(ViewMatrixMutex);

	ScreenPos.x = m_ViewMatrix.m00 * Pos.x + m_ViewMatrix.m01 * Pos.y + m_ViewMatrix.m02 * Pos.z + m_ViewMatrix.m03;
	ScreenPos.y = m_ViewMatrix.m10 * Pos.x + m_ViewMatrix.m11 * Pos.y + m_ViewMatrix.m12 * Pos.z + m_ViewMatrix.m13;

	float w = m_ViewMatrix.m30 * Pos.x + m_ViewMatrix.m31 * Pos.y + m_ViewMatrix.m32 * Pos.z + m_ViewMatrix.m33;

	if (!std::isfinite(w) || !std::isfinite(ScreenPos.x) || !std::isfinite(ScreenPos.y) || w < 0.01f)
		return false;

	float inv_w = 1.f / w;
	ScreenPos.x *= inv_w;
	ScreenPos.y *= inv_w;

	float x = Fuser::m_ScreenSize.x * .5f;
	float y = Fuser::m_ScreenSize.y * .5f;

	x += 0.5f * ScreenPos.x * Fuser::m_ScreenSize.x + 0.5f;
	y -= 0.5f * ScreenPos.y * Fuser::m_ScreenSize.y + 0.5f;

	ScreenPos.x = x;
	ScreenPos.y = y;

	return true;
}

bool Deadlock::UpdateLocalPlayerAddresses(DMA_Connection* Conn)
{
	ZoneScoped;

	uintptr_t LocalPlayerControllerAddress = Proc().GetClientBase() + Offsets::LocalController;
	const auto controller = Proc().ReadMem<uintptr_t>(Conn, LocalPlayerControllerAddress);
	uintptr_t pawn{};
	{
		std::scoped_lock entities(EntityList::m_PawnMutex, EntityList::m_ControllerMutex);
		auto it = std::find(EntityList::m_PlayerControllers.begin(), EntityList::m_PlayerControllers.end(), controller);
		if (it != EntityList::m_PlayerControllers.end()) pawn = EntityList::GetEntityAddressFromHandle(it->m_hPawn);
	}
	{
		std::scoped_lock addresses(m_LocalAddressMutex);
		m_LocalPlayerControllerAddress = controller;
		m_LocalPlayerPawnAddress = pawn;
	}
	DbgPrintln("Local Player Controller Address: 0x{:X}; Pawn: 0x{:X}", controller, pawn);
	return pawn != 0;
}

void Deadlock::GetPredictionAddress()
{
	// The LEA signature resolves the instance itself. Reading its first word as a pointer would
	// accidentally use an object member/vtable address instead of CPrediction.
	m_PredictionAddress = GlobalPatterns::PredictionInstanceAddress(Proc().GetClientBase(), Offsets::PredictionInstance);
	Diagnostics::Event("info", "prediction_instance", std::format("address=0x{:X} source=LEA direct_object", m_PredictionAddress));

	DbgPrintln("Prediction Address: 0x{:X}", m_PredictionAddress);
}

void Deadlock::UpdateServerTime(DMA_Connection* Conn)
{
	if (!m_PredictionAddress) return;

	ZoneScoped;

	uintptr_t ServerTimeAddress = m_PredictionAddress + Offsets::Prediction::ServerTime;

	std::scoped_lock lock(m_ServerTimeMutex);
	m_ServerTime = Proc().ReadMem<float>(Conn, ServerTimeAddress);
}

void Deadlock::UpdateClientYaw(DMA_Connection* Conn)
{
	ZoneScoped;

	auto Mat = GetViewMatrix();

	SetClientYaw(atan2(Mat.m01, Mat.m00));
}

void Deadlock::SetClientYaw(float NewYaw)
{
	std::scoped_lock Lock(m_ClientYawMutex);
	m_ClientYaw = NewYaw;
}

float Deadlock::GetClientYaw()
{
	std::scoped_lock Lock(m_ClientYawMutex);
	return m_ClientYaw;
}

float Deadlock::GetClientYawDegrees()
{
	std::scoped_lock Lock(m_ClientYawMutex);

	return m_ClientYaw * (180.0 / std::numbers::pi);
}
