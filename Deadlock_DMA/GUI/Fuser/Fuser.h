#pragma once

class Fuser
{
public:
	static void Render();
	static void RenderSettings();
	static void RenderSettingsContent();
	static int GetOutputMonitorIndex();
	static void RenderSoulsPerMinute();

public:
	static inline bool bSettings{ true };
	static inline bool bMasterToggle{ true };
	static inline bool bFullscreen{ true };
	static inline int MonitorIndex{ -1 }; // -1 selects a suitable output monitor automatically.
	static inline bool bDrawSoulsPerMinute{ false };
	static inline ImVec2 m_ScreenSize{ 2560.0f,1440.0f }; // Actual output pixels, used by projection.
	static inline ImVec2 m_WindowedSize{ 2560.0f,1440.0f };
};
