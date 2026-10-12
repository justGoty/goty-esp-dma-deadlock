#include "pch.h"

#include "ESP.h"

#include "Deadlock/Entity List/EntityList.h"

#include "GUI/Color Picker/Color Picker.h"

#include "Draw/Players.h"
#include "Draw/Troopers.h"
#include "Draw/Camps.h"
#include "Draw/Sinners.h"

void ESP::OnFrame()
{
	if (!bMasterToggle)
		return;

	ZoneScoped;

	auto DrawList = ImGui::GetWindowDrawList();
	auto WindowPos = ImGui::GetWindowPos();

	ImGui::PushFont(nullptr, 16.0f);

	if (Draw_Players::bMasterToggle)
		Draw_Players::operator()();

	if (!bPlayersOnly && Draw_Troopers::bMasterToggle)
		Draw_Troopers::operator()();

	if (!bPlayersOnly && Draw_Camps::bMasterToggle)
		Draw_Camps::operator()();

	if (!bPlayersOnly && Draw_Sinners::bMasterToggle)
		Draw_Sinners::operator()();

	ImGui::PopFont();
}

void ESP::RenderSettings()
{
	ImGui::Begin("ESP Settings");

	ImGui::Checkbox("Enable ESP", &bMasterToggle);
	ImGui::Checkbox("Characters only", &bPlayersOnly);
	if (bPlayersOnly) ImGui::TextDisabled("NPCs, camps, world markers, radar and team HUD are disabled.");

	if (ImGui::CollapsingHeader("Players", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Indent();
		ImGui::Checkbox("Master Toggle", &Draw_Players::bMasterToggle);
		ImGui::Checkbox("Hide Friendly", &Draw_Players::bHideFriendly);
		ImGui::Checkbox("Boxes only", &Draw_Players::bBoxOnly);
		ImGui::Checkbox("Boxes", &Draw_Players::bDrawBox);
		if (Draw_Players::bDrawBox)
		{
			ImGui::Indent();
			ImGui::SliderFloat("Box Height", &Draw_Players::fBoxHeight, 24.f, 160.f, "%.0f units");
			ImGui::SliderFloat("Box Width", &Draw_Players::fBoxWidthRatio, .2f, 1.f, "%.2f");
			ImGui::SliderFloat("Box Thickness", &Draw_Players::fBoxThickness, .5f, 5.f, "%.1f");
			ImGui::Unindent();
		}
		if (Draw_Players::bBoxOnly) ImGui::TextDisabled("Bones, text and health bars are disabled. Box height is approximate.");
		ImGui::BeginDisabled(Draw_Players::bBoxOnly);

		ImGui::Checkbox("Bones", &Draw_Players::bDrawBones);
		if (Draw_Players::bDrawBones)
		{
			ImGui::Indent();
			ImGui::SliderFloat("Bones Thickness", &Draw_Players::fBonesThickness, 0.1f, 5.0f, "%.1f");
			ImGui::Unindent();
		};

		ImGui::Checkbox("Head Circle", &Draw_Players::bDrawHead);
		ImGui::Checkbox("Velocity Vector", &Draw_Players::bDrawVelocityVector);
		ImGui::Checkbox("Health Bars", &Draw_Players::bDrawHealthBar);
		if (!bPlayersOnly) {
			ImGui::Checkbox("Unsecured Souls", &Draw_Players::bDrawUnsecuredSouls);
			ImGui::Indent();
			ImGui::SetNextItemWidth(50.0f);
			ImGui::InputScalarN("Minimum Threshold", ImGuiDataType_S32, &Draw_Players::UnsecuredSoulsMinimumThreshold, 1);
			ImGui::SetNextItemWidth(50.0f);
			ImGui::InputScalarN("Highlight Threshold", ImGuiDataType_S32, &Draw_Players::UnsecuredSoulsHighlightThreshold, 1);
			ImGui::Unindent();
		}
		ImGui::Checkbox("Show Distance", &Draw_Players::bShowDistance);
		ImGui::Checkbox("Bone Numbers", &Draw_Players::bBoneNumbers);
		ImGui::EndDisabled();
		ImGui::Checkbox("Hide Local", &Draw_Players::bHideLocalPlayer);
		ImGui::Unindent();
	}

	if (!bPlayersOnly) {
		ImGui::Checkbox("Draw Troopers", &Draw_Troopers::bMasterToggle);
		ImGui::Checkbox("Hide Friendly Troopers", &Draw_Troopers::bHideFriendly);

		ImGui::Checkbox("Draw Monster Camps", &Draw_Camps::bMasterToggle);

		ImGui::Checkbox("Draw Sinners", &Draw_Sinners::bMasterToggle);
	}

	ImGui::End();
}
