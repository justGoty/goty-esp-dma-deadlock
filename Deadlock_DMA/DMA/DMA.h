#pragma once

class DMA_Connection
{
public: /* Singleton interface */
	static DMA_Connection* GetInstance();

private:


public:
	VMM_HANDLE GetHandle() const;
	bool EndConnection();

private:
	VMM_HANDLE m_VMMHandle = nullptr;

private:
	DMA_Connection();
	~DMA_Connection();
};