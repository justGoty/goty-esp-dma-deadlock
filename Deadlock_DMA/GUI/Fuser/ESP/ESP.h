#pragma once

#include "Deadlock/Classes/Classes.h"

class ESP
{
public:
	static inline bool bMasterToggle{ true };
	// Hides auxiliary categories/HUD and keeps their DMA lanes disabled.
	static inline bool bPlayersOnly{ true };

public:
	static void OnFrame();
	static void RenderSettings();
};
