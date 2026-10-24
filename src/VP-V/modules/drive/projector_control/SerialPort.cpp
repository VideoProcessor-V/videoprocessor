#include <pch.h>
#include "SerialPort.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>

namespace LedDrive
{
	Win32SerialPort::Win32SerialPort() : handle(INVALID_HANDLE_VALUE)
	{
	}

	Win32SerialPort::~Win32SerialPort()
	{
		Close();
	}

	bool Win32SerialPort::Open(const std::string& port, int baud, std::string& error)
	{
		Close();
		const std::string path = "\\\\.\\" + port;
		HANDLE opened = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (opened == INVALID_HANDLE_VALUE)
		{
			error = "cannot open " + port + " (Windows error " +
				std::to_string(GetLastError()) + ")";
			return false;
		}
		DCB dcb{};
		dcb.DCBlength = sizeof(dcb);
		if (!GetCommState(opened, &dcb))
		{
			error = "cannot read " + port + " settings";
			CloseHandle(opened);
			return false;
		}
		dcb.BaudRate = static_cast<DWORD>(baud);
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		dcb.fParity = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDsrSensitivity = FALSE;
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		// audit 2026-10-16: a line error must not abort every later read
		// (ReadFile failing now closes the port).
		dcb.fAbortOnError = FALSE;
		COMMTIMEOUTS timeouts{};
		// Reads return as soon as a byte is available or after the per-call
		// total; ReadLine sets the total for each call.
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
		timeouts.ReadTotalTimeoutConstant = 50;
		timeouts.WriteTotalTimeoutConstant = 500;
		if (!SetCommState(opened, &dcb) || !SetCommTimeouts(opened, &timeouts))
		{
			error = "cannot configure " + port + " (" + std::to_string(baud) + " 8N1)";
			CloseHandle(opened);
			return false;
		}
		PurgeComm(opened, PURGE_RXCLEAR | PURGE_TXCLEAR);
		handle = opened;
		pending.clear();
		return true;
	}

	void Win32SerialPort::Close()
	{
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(static_cast<HANDLE>(handle));
		handle = INVALID_HANDLE_VALUE;
		pending.clear();
	}

	bool Win32SerialPort::IsOpen() const
	{
		return handle != INVALID_HANDLE_VALUE;
	}

	void Win32SerialPort::DiscardInput()
	{
		pending.clear();
		if (IsOpen())
			PurgeComm(static_cast<HANDLE>(handle), PURGE_RXCLEAR);
	}

	bool Win32SerialPort::Write(const std::string& bytes, std::string& error)
	{
		if (!IsOpen())
		{
			error = "port is not open";
			return false;
		}
		DWORD written = 0;
		if (!WriteFile(static_cast<HANDLE>(handle), bytes.data(),
			static_cast<DWORD>(bytes.size()), &written, nullptr) ||
			written != bytes.size())
		{
			error = "write failed (Windows error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		return true;
	}

	bool Win32SerialPort::ReadLine(int timeoutMs, std::string& line)
	{
		line.clear();
		if (!IsOpen()) return false;
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeoutMs);
		for (;;)
		{
			const size_t cr = pending.find('\r');
			if (cr != std::string::npos)
			{
				for (char c : pending.substr(0, cr))
					if (c != '\n') line += c;
				pending.erase(0, cr + 1);
				return true;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline) break;
			COMMTIMEOUTS timeouts{};
			timeouts.ReadIntervalTimeout = MAXDWORD;
			timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
			timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(std::max<long long>(1,
				std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
			timeouts.WriteTotalTimeoutConstant = 500;
			SetCommTimeouts(static_cast<HANDLE>(handle), &timeouts);
			char buffer[64];
			DWORD read = 0;
			if (!ReadFile(static_cast<HANDLE>(handle), buffer, sizeof(buffer), &read, nullptr))
			{
				// audit 2026-10-16: a failed read (device unplugged) closes the
				// port, so its owner reopens it with its back-off instead of
				// calling straight back into a failing read (100% CPU).
				Close();
				return false;
			}
			if (read == 0)
			{
				// audit 2026-10-16: some USB serial drivers return at once with
				// nothing after an unplug; a failing ClearCommError tells.
				DWORD errors = 0;
				COMSTAT status{};
				if (!ClearCommError(static_cast<HANDLE>(handle), &errors, &status))
				{
					Close();
					return false;
				}
			}
			pending.append(buffer, read);
			// Kept partial lines are bounded: noise without a CR never grows it.
			if (pending.size() > 4096) pending.erase(0, pending.size() - 4096);
		}
		// audit 2026-10-16: a timeout keeps the partial line in `pending` for
		// the next call (a reply split across the driver's short reads was
		// lost); only a complete line is returned.
		return false;
	}

	std::unique_ptr<ISerialPort> CreateWin32SerialPort()
	{
		return std::make_unique<Win32SerialPort>();
	}
}
