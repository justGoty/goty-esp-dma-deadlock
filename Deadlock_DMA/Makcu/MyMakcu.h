#pragma once
#include "makcu/makcu.h"

class MyMakcu
{
public:
	static bool Initialize();
	static makcu::Device& GetDevice();
};