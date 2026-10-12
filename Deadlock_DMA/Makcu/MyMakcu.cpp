#include "pch.h"

#include "MyMakcu.h"

bool MyMakcu::Initialize()
{
	std::println("[Makcu] Initializing Makcu Device...");

	if (!GetDevice().connect())
	{
		std::println("[Makcu] Failed to connect to Makcu");
		return false;
	}

	auto DeviceInfo = MyMakcu::GetDevice().getDeviceInfo();
	std::println("[Makcu] Connected to Makcu Device on port: {}", DeviceInfo.port);

	return true;
}
makcu::Device& MyMakcu::GetDevice() { static makcu::Device device; return device; }
