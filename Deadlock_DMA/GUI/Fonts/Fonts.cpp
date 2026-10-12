#include "pch.h"
#include "Fonts.h"

void Fonts::Initialize(ImGuiIO& io)
{
	// The public source-only snapshot does not redistribute embedded font data.
	ImFontConfig config;
	config.SizePixels = 16.0f;
	m_IBMPlexMonoSemiBold = io.Fonts->AddFontDefault(&config);
	IM_ASSERT(m_IBMPlexMonoSemiBold != nullptr);
}
