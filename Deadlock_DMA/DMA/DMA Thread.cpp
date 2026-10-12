#include "pch.h"

#include "DMA Thread.h"
#include "Input Manager.h"
#include "Deadlock/Deadlock.h"
#include "Deadlock/Entity List/EntityList.h"

#include "GUI/Keybinds/Keybinds.h"
#include "GUI/Query.h"
#include "Diagnostics/RuntimeOptions.h"
#include "Timing/ShortWait.h"
#include "DMA/RefreshExperiment.h"
#include "DMA/ManagedRefresh.h"
#include "DMA/TargetLiveness.h"

extern std::atomic<bool> bRunning;

static void RunWorker()
{
	std::println("[DMA Thread] DMA Thread started.");

	DMA_Connection* Conn = DMA_Connection::GetInstance();

	if (RuntimeOptions::EnableMakcu) c_keys::InitKeyboard(Conn);
	auto close = [](DMA_Connection* connection) { connection->EndConnection(); };
	std::unique_ptr<DMA_Connection, decltype(close)> guard(Conn, close);

#ifdef TRACY_ENABLE
	tracy::SetThreadName("DMA Thread");
#endif

	if (!Deadlock::Initialize(Conn))
	{
		std::println("[DMA Thread] Deadlock Initialization failed, requesting exit.");
		if (bRunning) { RuntimeOptions::ExitCode = 1; Diagnostics::Event("error", "initialization_failed", "Process/module/offset initialization failed"); Diagnostics::SetStage("failed"); }
		bRunning = false;
		return;
	}

	auto Deadlock = &Deadlock::Proc();
	RuntimeOptions::ResidentGameReads = RuntimeOptions::ManagedRefresh;
	TargetLiveness targetLiveness;
	if (RuntimeOptions::ManagedRefresh) {
		Diagnostics::SetGameVisible(false, Deadlock->GetPID());
		Diagnostics::ScopedOperation operation("target_liveness_initialize");
		if (!targetLiveness.Initialize(Conn->GetHandle(), Deadlock->GetPID()))
			throw std::runtime_error(std::format("Target liveness validation unavailable: {}; use --background-refresh for the legacy policy", targetLiveness.Reason()));
	}
	bool gameVisible{};
	auto probe = [&] {
		if (!RuntimeOptions::ManagedRefresh) { gameVisible = Deadlock->ProbeCurrentProcess(Conn); return; }
		const auto result = targetLiveness.Probe();
		gameVisible = result == TargetLiveness::Result::Alive;
		Diagnostics::SetGameVisible(gameVisible, Deadlock->GetPID());
		if (!gameVisible) Diagnostics::Warning("target_liveness_lost", targetLiveness.Reason());
	};
	probe();
	CTimer GameVisibilityTimer(std::chrono::seconds(2), probe, "process_probe");
	ManagedRefresh managedRefresh(Conn->GetHandle(), RuntimeOptions::ManagedRefresh);

	CTimer ViewMatrixTimer(std::chrono::milliseconds(1), [&Conn] {Deadlock::UpdateViewMatrix(Conn); }, "camera");
	CTimer YawTimer(std::chrono::milliseconds(10), [&Conn] {Deadlock::UpdateClientYaw(Conn); }, "yaw");
	CTimer ServerTimeTimer(std::chrono::milliseconds(1000), [&Conn] {Deadlock::UpdateServerTime(Conn); }, "server_time");
	CTimer LocalControllerAddressTime(std::chrono::seconds(15), [&Conn] {Deadlock::UpdateLocalPlayerAddresses(Conn); }, "local_addresses");

	CTimer FullTrooperTimer(std::chrono::seconds(3), [&Conn, &Deadlock] {EntityList::FullTrooperRefresh(Conn, Deadlock); }, "troopers_full");
	CTimer QuickTrooperTimer(std::chrono::milliseconds(100), [&Conn, &Deadlock] {EntityList::QuickTrooperRefresh(Conn, Deadlock); }, "troopers_quick");

	CTimer FullPawnTimer(std::chrono::seconds(2), [&Conn, &Deadlock] {EntityList::FullPawnRefresh_lk(Conn, Deadlock); }, "pawns_full");
	CTimer QuickPawnTimer(std::chrono::milliseconds(5), [&Conn, &Deadlock] {EntityList::QuickPawnRefresh(Conn, Deadlock); }, "pawns_quick");

	CTimer FullMonsterCampTimer(std::chrono::seconds(2), [&Conn, &Deadlock] {EntityList::FullMonsterCampRefresh(Conn, Deadlock); }, "camps_full");
	CTimer QuickMonsterCampTimer(std::chrono::milliseconds(100), [&Conn, &Deadlock] {EntityList::QuickMonsterCampRefresh(Conn, Deadlock); }, "camps_quick");

	CTimer FullControllerTimer(std::chrono::milliseconds(50), [&Conn, &Deadlock] { EntityList::FullControllerRefresh_lk(Conn, Deadlock); }, "controllers_full");

	CTimer FullSinnerTimer(std::chrono::milliseconds(1000), [&Conn, &Deadlock] {EntityList::FullSinnerRefresh(Conn, Deadlock); }, "sinners");
	CTimer FullUpdateTimer(std::chrono::seconds(5), [&Conn, &Deadlock] {EntityList::FullUpdate(Conn, Deadlock); }, "discovery");

	CTimer Keybinds(std::chrono::milliseconds(50), [&Conn]() { Keybinds::OnDMAFrame(Conn); }, "keys");

	Diagnostics::SetStage("running", Deadlock::Proc().GetPID());
	Diagnostics::WorkerTick(0);
	ShortWait workerWait;
	RefreshExperiment refreshExperiment(Conn->GetHandle(), RuntimeOptions::DiagnoseRefresh);
	while (bRunning)
	{
		refreshExperiment.Tick();
		auto TimeNow = std::chrono::steady_clock::now();
		if (!managedRefresh.Tick(gameVisible)) throw std::runtime_error("Managed refresh failed; stopping stale session");
		GameVisibilityTimer.Tick(TimeNow);
		// A lost PID keeps the application responsive and hides cached ESP; reattach is a separate action.
		if (!gameVisible) {
			Diagnostics::WorkerTick(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - TimeNow).count());
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}
		ViewMatrixTimer.Tick(TimeNow);
		if (Query::IsUsingPlayers() || Query::IsUsingTroopers()) YawTimer.Tick(TimeNow);
		if (Query::IsUsingServerTime()) ServerTimeTimer.Tick(TimeNow);
		LocalControllerAddressTime.Tick(TimeNow);
		FullTrooperTimer.Tick(TimeNow);
		QuickTrooperTimer.Tick(TimeNow);
		FullPawnTimer.Tick(TimeNow);
		QuickPawnTimer.Tick(TimeNow);
		FullMonsterCampTimer.Tick(TimeNow);
		QuickMonsterCampTimer.Tick(TimeNow);
		FullControllerTimer.Tick(TimeNow);
		FullSinnerTimer.Tick(TimeNow);
		FullUpdateTimer.Tick(TimeNow);
		if (RuntimeOptions::EnableMakcu) Keybinds.Tick(TimeNow);
		Diagnostics::WorkerTick(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - TimeNow).count());
		workerWait.WaitFor(std::chrono::milliseconds(1));
	}


}
void DMA_Thread_Main() {
    try { RunWorker(); }
    catch (const std::exception& e) { RuntimeOptions::ExitCode = 1; Diagnostics::Event("error", "worker_exception", e.what()); Diagnostics::SetStage("failed"); bRunning = false; }
    catch (...) { RuntimeOptions::ExitCode = 1; Diagnostics::Event("error", "worker_exception", "Unknown exception"); Diagnostics::SetStage("failed"); bRunning = false; }
}
