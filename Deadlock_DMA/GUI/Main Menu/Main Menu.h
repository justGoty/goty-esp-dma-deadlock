#pragma once

class MainMenu
{
public:
	static void Render();
	static void InitializeCompactLayout();
	static inline bool bControlsVisible{ true };
	static inline bool bShowESPSettings{ false };
	static inline bool bShowConfig{ false };
	static inline bool bShowDiagnostics{ false };
	static inline bool bShowRadar{ false };
	static inline bool bVSync{ false };
	static inline int iTargetFPS{ 144 };

};
