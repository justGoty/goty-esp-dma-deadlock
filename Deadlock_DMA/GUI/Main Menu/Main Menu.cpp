#include "pch.h"
#include "Main Menu.h"
#include "GUI/Color Picker/Color Picker.h"
#include "GUI/Fuser/Fuser.h"
#include "GUI/Fuser/ESP/ESP.h"
#include "GUI/Fuser/Status Bars/Status Bars.h"
#include "GUI/Radar/Radar.h"
#include "GUI/Aimbot/Aimbot.h"
#include "GUI/Keybinds/Keybinds.h"
#include "GUI/Debug GUI/Player List/Player List.h"
#include "GUI/Debug GUI/Class List/Class List.h"
#include "GUI/Debug GUI/Trooper List/Trooper List.h"
#include "GUI/Main Window/Main Window.h"
#include "Diagnostics/Panel.h"
#include "Diagnostics/RuntimeOptions.h"

void MainMenu::InitializeCompactLayout()
{
	// Config loading can restore old settings visibility. Close only tool windows;
	// preserve every feature, color, binding and saved configuration value.
	Fuser::bSettings = false;
	Aimbot::bSettings = false;
	Keybinds::bSettings = false;
	ColorPicker::bMasterToggle = false;
	PlayerList::bSettings = false;
	ClassList::bSettings = false;
	TrooperList::bSettings = false;
	bShowESPSettings = bShowConfig = bShowDiagnostics = bShowRadar = false;
	bControlsVisible = true;
}

void MainMenu::Render()
{
	// The output viewport is independent, so hiding the controls never minimizes
	// the Fuser. Poll the local key even while the controls HWND is hidden.
	static bool insertWasDown = false;
	const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
	if (insertDown && !insertWasDown) {
		bControlsVisible = !bControlsVisible;
		ShowWindow(MainWindow::g_hWnd, bControlsVisible ? SW_SHOWNORMAL : SW_HIDE);
	}
	insertWasDown = insertDown;
	if (!bControlsVisible) return;

	auto* viewport = ImGui::GetMainViewport();
	// Host bounds override old imgui.ini docking/size entries on every launch.
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
	if (!ImGui::Begin("Controls", nullptr, flags)) { ImGui::End(); return; }

	ImGui::TextUnformatted("GOTY Deadlock DMA");
	ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 116.0f);
	if (ImGui::SmallButton("Hide [Insert]")) {
		bControlsVisible = false;
		ShowWindow(MainWindow::g_hWnd, SW_HIDE);
	}
	ImGui::Spacing();
	const auto snapshot = Diagnostics::GetSnapshot();
	DiagnosticsPanel::RenderConnectionStatus(snapshot);
	ImGui::Separator();
	ImGui::Checkbox("Fuser output", &Fuser::bMasterToggle);
	ImGui::SameLine();
	ImGui::Checkbox("ESP", &ESP::bMasterToggle);
	ImGui::SameLine();
	ImGui::TextDisabled(ESP::bPlayersOnly ? "Characters only" : "All enabled categories");
	ImGui::Spacing();

	if (ImGui::BeginTabBar("ControlSections")) {
		if (ImGui::BeginTabItem("Output")) {
			ImGui::BeginChild("OutputContent", ImVec2(0, 0));
			Fuser::RenderSettingsContent();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Settings")) {
			ImGui::SeparatorText("Feature windows");
			ImGui::Checkbox("ESP settings", &bShowESPSettings);
			ImGui::Checkbox("Configuration manager", &bShowConfig);
			ImGui::Checkbox("Colors", &ColorPicker::bMasterToggle);
			ImGui::Checkbox("Keybinds", &Keybinds::bSettings);
			ImGui::BeginDisabled(!RuntimeOptions::EnableMakcu);
			ImGui::Checkbox("Aimbot settings", &Aimbot::bSettings);
			ImGui::EndDisabled();
			if (!RuntimeOptions::EnableMakcu) ImGui::TextDisabled("MAKCU input is disabled for this session.");
			ImGui::SeparatorText("Performance");
			ImGui::Checkbox("VSync", &bVSync);
			ImGui::SetNextItemWidth(200.0f);
			ImGui::SliderInt("Target FPS", &iTargetFPS, 60, 240);
			ImGui::TextDisabled("%.1f FPS | %.2f ms / frame", ImGui::GetIO().Framerate,
				ImGui::GetIO().Framerate > 0.0f ? 1000.0f / ImGui::GetIO().Framerate : 0.0f);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Tools")) {
			ImGui::SeparatorText("Optional windows");
			if (!ESP::bPlayersOnly) {
				ImGui::Checkbox("Radar window", &bShowRadar);
				if (bShowRadar) ImGui::Checkbox("Enable radar", &Radar::bMasterToggle);
			}
			ImGui::Checkbox("Player list", &PlayerList::bSettings);
			ImGui::Checkbox("Class list", &ClassList::bSettings);
			if (!ESP::bPlayersOnly) ImGui::Checkbox("Trooper list", &TrooperList::bSettings);
			ImGui::Spacing();
			ImGui::SeparatorText("Diagnostics");
			ImGui::Checkbox("Show detailed diagnostics", &bShowDiagnostics);
			if (bShowDiagnostics) {
				ImGui::BeginChild("DiagnosticDetails", ImVec2(0, 0));
				DiagnosticsPanel::RenderDetails(snapshot);
				ImGui::EndChild();
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}
