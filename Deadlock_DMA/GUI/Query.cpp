#include "pch.h"
#include "Query.h"
#include "GUI/Fuser/Fuser.h"
#include "GUI/Fuser/ESP/ESP.h"
#include "GUI/Fuser/Status Bars/Status Bars.h"
#include "GUI/Main Menu/Main Menu.h"
#include "GUI/Radar/Radar.h"
#include "GUI/Fuser/ESP/Draw/Players.h"
#include "GUI/Fuser/ESP/Draw/Troopers.h"
#include "GUI/Fuser/ESP/Draw/Camps.h"
#include "GUI/Fuser/ESP/Draw/Sinners.h"
#include "Diagnostics/RuntimeOptions.h"
#include "GUI/Debug GUI/Player List/Player List.h"
namespace { std::atomic<bool> players{true}, troopers{}, camps{}, sinners{}, serverTime{}, bones{}, velocity{}, playerSouls{}, minimalControllers{true}; }
void Query::Publish() {
    const bool useEsp = Fuser::bMasterToggle && ESP::bMasterToggle;
    const bool useExtras = !ESP::bPlayersOnly;
    const bool useRadar = useExtras && MainMenu::bControlsVisible && MainMenu::bShowRadar && Radar::bMasterToggle;
    const bool useTeamHud = useExtras && Fuser::bMasterToggle && (Fuser::bDrawSoulsPerMinute ||
        (StatusBars::bMasterToggle && (StatusBars::bRenderTeamHealthBar || StatusBars::bRenderTeamSoulsBar || StatusBars::bRenderUnspentSoulsBar)));
    players = useRadar || useTeamHud || (useEsp && Draw_Players::bMasterToggle);
    troopers = useExtras && useEsp && Draw_Troopers::bMasterToggle;
    camps = useExtras && useEsp && Draw_Camps::bMasterToggle;
    sinners = useExtras && useEsp && Draw_Sinners::bMasterToggle;
    serverTime = useExtras && Fuser::bMasterToggle && Fuser::bDrawSoulsPerMinute;
    const bool usePlayerEsp = useEsp && Draw_Players::bMasterToggle;
    bones = RuntimeOptions::EnableMakcu || (usePlayerEsp && !Draw_Players::bBoxOnly && (Draw_Players::bDrawBones || Draw_Players::bDrawHead || Draw_Players::bBoneNumbers));
    velocity = RuntimeOptions::EnableMakcu || (usePlayerEsp && !Draw_Players::bBoxOnly && Draw_Players::bDrawVelocityVector);
    playerSouls = (usePlayerEsp && !Draw_Players::bBoxOnly && useExtras && Draw_Players::bDrawUnsecuredSouls) || useTeamHud;
    minimalControllers = Draw_Players::bBoxOnly && !useTeamHud && !useRadar && !PlayerList::bSettings && !RuntimeOptions::EnableMakcu;
}
bool Query::IsUsingPlayers() { return players.load(); }
bool Query::IsUsingTroopers() { return troopers.load(); }
bool Query::IsUsingCamps() { return camps.load(); }
bool Query::IsUsingSinners() { return sinners.load(); }
bool Query::IsUsingServerTime() { return serverTime.load(); }
bool Query::IsUsingBones() { return bones.load(); }
bool Query::IsUsingVelocity() { return velocity.load(); }
bool Query::IsUsingPlayerSouls() { return playerSouls.load(); }
bool Query::IsUsingMinimalControllers() { return minimalControllers.load(); }
