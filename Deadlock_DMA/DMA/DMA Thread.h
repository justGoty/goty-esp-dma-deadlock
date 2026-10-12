#pragma once

#include "DMA/DMA.h"
#include "Diagnostics/Diagnostics.h"

template <typename T, typename F>
class CTimer
{
public:
	CTimer(T _Interval, F _Function, std::string_view lane = {}) : m_Interval(_Interval), m_Function(_Function), m_Lane(lane) {}

	void Tick(std::chrono::steady_clock::time_point CurrentTime)
	{
		if (std::chrono::duration_cast<T>(CurrentTime - m_LastExecutionTime) >= m_Interval)
		{
			const auto started = std::chrono::steady_clock::now();
			const auto failed = Diagnostics::FailedReads.load();
			m_Function();
			if (!m_Lane.empty()) Diagnostics::LaneTick(m_Lane,
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
				Diagnostics::FailedReads.load() == failed);
			m_LastExecutionTime = CurrentTime;
		}
	}
private:
	T m_Interval{};
	F m_Function{};
	std::string_view m_Lane;
	std::chrono::steady_clock::time_point m_LastExecutionTime{};
};

void DMA_Thread_Main();
