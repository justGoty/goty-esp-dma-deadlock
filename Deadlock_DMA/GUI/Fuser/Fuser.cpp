#include "pch.h"
#include "Fuser.h"
#include "ESP/ESP.h"
#include "Deadlock/Entity List/EntityList.h"
#include "Status Bars/Status Bars.h"
#include "GUI/Aimbot/Aimbot.h"
#include "Diagnostics/Panel.h"
#include <cmath>

namespace
{
	ImGuiID outputViewportId{};
	int previousMonitor = -1;
	bool previousFullscreen = true;
	bool outputWasSubmitted = false;
	bool haveLoggedOutput = false;
	int loggedMonitor = -1;
	bool loggedFullscreen = true;
	ImVec2 loggedSize{};

	HMONITOR GetViewportMonitor(ImGuiViewport* viewport)
	{
		if (!viewport) return nullptr;
		auto hwnd = static_cast<HWND>(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
		return hwnd ? ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
	}

	int ResolveMonitor()
	{
		const auto& monitors = ImGui::GetPlatformIO().Monitors;
		if (monitors.empty()) return -1;
		if (Fuser::MonitorIndex >= 0 && Fuser::MonitorIndex < monitors.Size)
			return Fuser::MonitorIndex;

		const auto controlMonitor = GetViewportMonitor(ImGui::GetMainViewport());
		int matchingMonitor = -1;
		for (int i = 0; i < monitors.Size; ++i)
		{
			if (monitors[i].MainSize.x != 2560.0f || monitors[i].MainSize.y != 1440.0f) continue;
			if (monitors[i].PlatformHandle != controlMonitor) return i;
			matchingMonitor = i;
		}
		if (matchingMonitor >= 0) return matchingMonitor;

		const auto currentMonitor = GetViewportMonitor(ImGui::FindViewportByID(outputViewportId));
		for (int i = 0; currentMonitor && i < monitors.Size; ++i)
			if (monitors[i].PlatformHandle == currentMonitor) return i;
		return 0; // Win32 backend places the primary monitor first.
	}

	void ValidateWindowedSize()
	{
		if (!std::isfinite(Fuser::m_WindowedSize.x) || Fuser::m_WindowedSize.x < 1.0f)
			Fuser::m_WindowedSize.x = 2560.0f;
		if (!std::isfinite(Fuser::m_WindowedSize.y) || Fuser::m_WindowedSize.y < 1.0f)
			Fuser::m_WindowedSize.y = 1440.0f;
		Fuser::m_WindowedSize.x = std::clamp(std::round(Fuser::m_WindowedSize.x), 320.0f, 16384.0f);
		Fuser::m_WindowedSize.y = std::clamp(std::round(Fuser::m_WindowedSize.y), 240.0f, 16384.0f);
	}
}

void Fuser::Render()
{
	if (!bMasterToggle) { outputWasSubmitted = false; return; }

	ValidateWindowedSize();
	const auto monitorIndex = ResolveMonitor();
	const auto& monitors = ImGui::GetPlatformIO().Monitors;
	const auto monitorPosition = monitorIndex >= 0 ? monitors[monitorIndex].MainPos : ImGui::GetMainViewport()->Pos;
	const auto monitorSize = monitorIndex >= 0 ? monitors[monitorIndex].MainSize : ImGui::GetMainViewport()->Size;
	const bool fullscreen = bFullscreen;
	const auto outputSize = fullscreen ? monitorSize : m_WindowedSize;

	ImGuiWindowClass outputClass;
	outputClass.ClassId = 0x46555352; // Dedicated output; controls cannot dock into it.
	outputClass.ParentViewportId = 0; // Output remains independent when controls are hidden.
	outputClass.DockingAllowUnclassed = false;
	outputClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoDecoration |
		ImGuiViewportFlags_NoTaskBarIcon | ImGuiViewportFlags_NoFocusOnAppearing;
	ImGui::SetNextWindowClass(&outputClass);
	ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
	if (fullscreen)
		ImGui::SetNextWindowPos(monitorPosition, ImGuiCond_Always);
	else if (!outputWasSubmitted || previousFullscreen != fullscreen || previousMonitor != monitorIndex)
		ImGui::SetNextWindowPos(ImVec2(monitorPosition.x + 16.0f, monitorPosition.y + 16.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(outputSize, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
	auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (fullscreen) flags |= ImGuiWindowFlags_NoMove;
	const bool visible = ImGui::Begin("Fuser", nullptr, flags);
	outputViewportId = ImGui::GetWindowViewport()->ID;
	m_ScreenSize = ImGui::GetWindowSize();
	previousMonitor = monitorIndex;
	previousFullscreen = fullscreen;
	outputWasSubmitted = true;
	if (!haveLoggedOutput || loggedMonitor != monitorIndex || loggedFullscreen != fullscreen ||
		loggedSize.x != m_ScreenSize.x || loggedSize.y != m_ScreenSize.y)
	{
		Diagnostics::Event("info", "fuser_output_changed", std::format(
			"mode={} monitor={} pixels={:.0f}x{:.0f} origin={:.0f},{:.0f} target=2560x1440",
			fullscreen ? "fullscreen" : "windowed", monitorIndex, m_ScreenSize.x, m_ScreenSize.y,
			monitorPosition.x, monitorPosition.y));
		haveLoggedOutput = true;
		loggedMonitor = monitorIndex;
		loggedFullscreen = fullscreen;
		loggedSize = m_ScreenSize;
	}

	if (visible)
	{
		const auto snapshot = Diagnostics::GetSnapshot();
		if (snapshot.gameVisible && (!ESP::bPlayersOnly || Diagnostics::HasFreshEspData(snapshot)))
		{
			ESP::OnFrame();
			if (!ESP::bPlayersOnly) {
				Aimbot::RenderFOVCircle();
				RenderSoulsPerMinute();
				StatusBars::Render();
			}
		}
		const auto position = ImGui::GetWindowPos();
		DrawConnectionStatusBadge(ImVec2(position.x + 16.0f, position.y + 16.0f));
	}

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

int Fuser::GetOutputMonitorIndex()
{
	return previousMonitor;
}

void Fuser::RenderSettings()
{
	if (!bSettings) return;

	ImGui::Begin("Fuser Settings", &bSettings);
	RenderSettingsContent();
	ImGui::End();
}

void Fuser::RenderSettingsContent()
{

	ImGui::Checkbox("Enable Fuser", &bMasterToggle);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();


	ImGui::SeparatorText("Screen Configuration");
	ImGui::Checkbox("Fullscreen output", &bFullscreen);
	const auto& monitors = ImGui::GetPlatformIO().Monitors;
	const auto selection = MonitorIndex >= 0 && MonitorIndex < monitors.Size
		? std::format("Monitor {} ({:.0f}x{:.0f})", MonitorIndex + 1,
			monitors[MonitorIndex].MainSize.x, monitors[MonitorIndex].MainSize.y)
		: std::string("Auto (prefer 2560x1440)");
	if (ImGui::BeginCombo("Output monitor", selection.c_str()))
	{
		if (ImGui::Selectable("Auto (prefer 2560x1440)", MonitorIndex < 0)) MonitorIndex = -1;
		for (int i = 0; i < monitors.Size; ++i)
		{
			const auto label = std::format("Monitor {} ({:.0f}x{:.0f})", i + 1, monitors[i].MainSize.x, monitors[i].MainSize.y);
			if (ImGui::Selectable(label.c_str(), MonitorIndex == i)) MonitorIndex = i;
		}
		ImGui::EndCombo();
	}
	ImGui::Text("Output pixels: %.0f x %.0f", m_ScreenSize.x, m_ScreenSize.y);
	if (bFullscreen && (m_ScreenSize.x != 2560.0f || m_ScreenSize.y != 1440.0f))
		ImGui::TextWrapped("Selected display is not 2560x1440. Fullscreen uses its actual pixels. Set the fuser display to 2560x1440 in Windows for 2K output.");
	ImGui::Text("Windowed resolution");
	ImGui::BeginDisabled(bFullscreen);
	ImGui::SetNextItemWidth(150.0f);
	ImGui::InputFloat("Width##Screen", &m_WindowedSize.x, 0.0f, 0.0f, "%.0f");
	ImGui::SetNextItemWidth(150.0f);
	ImGui::InputFloat("Height##Screen", &m_WindowedSize.y, 0.0f, 0.0f, "%.0f");
	ImGui::EndDisabled();
	ValidateWindowedSize();
	ImGui::TextDisabled("(Match your game resolution)");

	ImGui::Spacing();

	ImGui::Checkbox("Characters only", &ESP::bPlayersOnly);
	if (ESP::bPlayersOnly) {
		ImGui::TextDisabled("Only player characters and connection status are shown.");
		return;
	}
	ImGui::SeparatorText("HUD Elements");
	ImGui::Checkbox("Souls Per Minute", &bDrawSoulsPerMinute);

	ImGui::Checkbox("Team Health Bar", &StatusBars::bRenderTeamHealthBar);
	ImGui::Checkbox("Team Souls Bar", &StatusBars::bRenderTeamSoulsBar);
	ImGui::Checkbox("Unspent Souls Bar", &StatusBars::bRenderUnspentSoulsBar);

}

void Fuser::RenderSoulsPerMinute()
{
	if (!bDrawSoulsPerMinute) return;

	ZoneScoped;

	std::scoped_lock PawnLock(EntityList::m_ControllerMutex);

	if (EntityList::m_LocalControllerIndex < 0 || static_cast<size_t>(EntityList::m_LocalControllerIndex) >= EntityList::m_PlayerControllers.size()) return;

	auto& LocalController = EntityList::m_PlayerControllers[EntityList::m_LocalControllerIndex];

	auto Souls = LocalController.m_TotalSouls;
	float SoulsPerSecond = 0.0f;

	{
		std::scoped_lock timeLock(Deadlock::m_ServerTimeMutex);
		if (!std::isfinite(Deadlock::m_ServerTime) || Deadlock::m_ServerTime <= 0) return;
		SoulsPerSecond = static_cast<float>(LocalController.m_TotalSouls) / Deadlock::m_ServerTime;
	}

	auto SoulsPerMinute = SoulsPerSecond * 60.0f;
	ImGui::PushFont(nullptr, 24.0f);
	ImGui::SetCursorPos({ 2.0f, m_ScreenSize.y - ImGui::GetTextLineHeight() });
	ImGui::Text("%.1f Souls/Min", SoulsPerMinute);
	ImGui::PopFont();
}
