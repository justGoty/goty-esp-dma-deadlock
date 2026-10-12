#pragma once
#include "Deadlock/Classes/Classes.h"
#include "Deadlock/Engine/Matrix.h"

class Draw_Players
{
public:
	static void operator()();

public:
	static inline bool bMasterToggle{ true };
	static inline bool bHideFriendly{ false };
	static inline bool bBoxOnly{ true };
	static inline bool bDrawBox{ true };
	static inline float fBoxHeight{ 72.f };
	static inline float fBoxWidthRatio{ 0.45f };
	static inline float fBoxThickness{ 2.f };
	static inline bool bDrawBones{ true };
	static inline float fBonesThickness{ 3.f };
	static inline bool bDrawHead{ true };
	static inline bool bDrawVelocityVector{ false };
	static inline bool bDrawUnsecuredSouls{ false };
	static inline int32_t UnsecuredSoulsMinimumThreshold{ 1 };
	static inline int32_t UnsecuredSoulsHighlightThreshold{ 400 };
	static inline bool bBoneNumbers{ false };
	static inline bool bDrawHealthBar{ true };
	static inline bool bHideLocalPlayer{ true };
	static inline bool bShowDistance{ true };

private:
	static void DrawPlayer(const CCitadelPlayerController& PC, const CCitadelPlayerPawn& Pawn, const Matrix44& camera);
	static void DrawBox(const CCitadelPlayerPawn& Pawn, ImDrawList* DrawList, const ImVec2& WindowPos, const Matrix44& camera);
	static void DrawHealthBar(const CCitadelPlayerController& PC, const CCitadelPlayerPawn& Pawn, const ImVec2& PawnScreenPos, ImDrawList* DrawList, int& LineNumber);
	static void DrawSkeleton(const CCitadelPlayerController& PC, const CCitadelPlayerPawn& Pawn, ImDrawList* DrawList, const ImVec2& WindowPos);
	static void DrawHeadCircle(const CCitadelPlayerController& PC, const CCitadelPlayerPawn& Pawn, ImDrawList* DrawList, const ImVec2& WindowPos);
	static void DrawVelocityVector(const CCitadelPlayerPawn& Pawn, ImDrawList* DrawList, const ImVec2& WindowPos);
	static void DrawBoneNumbers(const CCitadelPlayerPawn& Pawn);
	static void DrawNameTag(const CCitadelPlayerController& PC, const CCitadelPlayerPawn& Pawn, ImDrawList* DrawList, const ImVec2& WindowPos, int& LineNumber);
	static void DrawUnsecuredSouls(const CCitadelPlayerPawn& Pawn, const Vector2& ScreenPos, int& LineNumber);
};
