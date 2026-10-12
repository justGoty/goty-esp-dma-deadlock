#pragma once
#include "FieldOffsets.h"
#include "DMA/MultiScan.h"

namespace Offsets
{
	bool ResolveOffsets(DMA_Connection* Conn);
	bool ResolveGameEntitySystem(MultiScan& ms, DMA_Connection* Conn);
	bool ResolveLocalController(MultiScan& ms, DMA_Connection* Conn);
	bool ResolveViewMatrix(MultiScan& ms, DMA_Connection* Conn);
	bool ResolvePredictionInstance(MultiScan& ms, DMA_Connection* Conn);
	void LogLayout();

	inline std::ptrdiff_t GameEntitySystem = 0;
	inline std::ptrdiff_t LocalController = 0;
	inline std::ptrdiff_t ViewMatrix = 0;
	inline std::ptrdiff_t PredictionInstance = 0; // LEA target: CPrediction object RVA, not a pointer slot.
}
