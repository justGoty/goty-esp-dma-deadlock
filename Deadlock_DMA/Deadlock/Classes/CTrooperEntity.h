#pragma once
#include "CBaseEntity.h"

class CTrooperEntity : public CBaseEntity
{
public:

public:
	void PrepareRead_2(ScatterBatch& vmsh)
	{
		if (m_CurrentHealth < 1)
			SetInvalid();

		if (IsInvalid())
			return;

		CBaseEntity::PrepareRead_2(vmsh);
	}

	void QuickRead(ScatterBatch& vmsh)
	{
		if (m_CurrentHealth < 1)
			SetInvalid();

		if (IsInvalid())
			return;

		CBaseEntity::QuickRead(vmsh, true);
	}
};