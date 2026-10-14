#include <pch.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "ScriptListener.h"
#include "../DebugLog.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <windows.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

namespace LedDrive
{
	bool ParseScriptMessage(const std::string& text, ScriptMessage& out)
	{
		std::istringstream in(text);
		std::string tag, valueText;
		ScriptMessage message;
		if (!(in >> tag) || tag != "VP1" || !(in >> message.kind) || !(in >> valueText))
			return false;
		// LED power and contrast are whole numbers (owner 2026-10-14): the
		// value must be an integer token ("55.5" or "55x" is not a message).
		{
			char* end = nullptr;
			const long parsed = std::strtol(valueText.c_str(), &end, 10);
			if (end == valueText.c_str() || *end != '\0' || parsed < -1000000 || parsed > 1000000)
				return false;
			message.value = static_cast<int>(parsed);
		}
		if (message.kind != "LPW1" && message.kind != "CNTR" && message.kind != "TICK")
			return false;
		for (std::string field; in >> field;)
		{
			const size_t equals = field.find('=');
			if (equals == std::string::npos)
				continue;
			const std::string key = field.substr(0, equals);
			const std::string value = field.substr(equals + 1);
			char* end = nullptr;
			const double number = std::strtod(value.c_str(), &end);
			// Any program on this PC can send here (audit 2026-10-14): nothing
			// non-finite or out of int range reaches a cast.
			if (end == value.c_str() || !std::isfinite(number) || std::fabs(number) > 1.0e6)
				continue;
			// The integer fields (means, codes, targets) must be whole numbers;
			// only the script's clock (t) may have a fraction.
			if (key != "t" && (*end != '\0' || number != std::floor(number)))
				continue;
			if (key == "t") message.scriptSeconds = number;
			else if (key == "mean") message.mean = static_cast<int>(number);
			else if (key == "max") message.maximum = static_cast<int>(number);
			else if (key == "hl") message.highlight = static_cast<int>(number);
			else if (key == "tp") message.targetPower = static_cast<int>(number);
			else if (key == "tc") message.targetContrast = static_cast<int>(number);
		}
		out = message;
		return true;
	}

	ScriptListener::~ScriptListener()
	{
		Close();
	}

	bool ScriptListener::Open(int port, std::string& error)
	{
		Close();
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
		{
			error = "Winsock did not start";
			return false;
		}
		winsockStarted = true;
		const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET)
		{
			error = "no UDP socket";
			Close();
			return false;
		}
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<u_short>(port));
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // this PC only
		if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
		{
			closesocket(s);
			error = "UDP port " + std::to_string(port) + " is in use";
			Close();
			return false;
		}
		socketHandle = static_cast<uintptr_t>(s);
		boundPort = port;
		return true;
	}

	void ScriptListener::Close()
	{
		if (socketHandle != invalidSocket)
			closesocket(static_cast<SOCKET>(socketHandle));
		socketHandle = invalidSocket;
		boundPort = 0;
		if (winsockStarted)
			WSACleanup();
		winsockStarted = false;
	}

	std::vector<ScriptMessage> ScriptListener::Receive(int waitMs, const std::function<bool()>& interrupted)
	{
		std::vector<ScriptMessage> messages;
		if (socketHandle == invalidSocket)
			return messages;
		const SOCKET s = static_cast<SOCKET>(socketHandle);
		int timeoutMs = waitMs;
		for (;;)
		{
			fd_set readable;
			FD_ZERO(&readable);
			FD_SET(s, &readable);
			const int sliceMs = messages.empty() ? (timeoutMs < 50 ? timeoutMs : 50) : 0;
			timeval timeout{ sliceMs / 1000, (sliceMs % 1000) * 1000 };
			const int ready = select(0, &readable, nullptr, nullptr, &timeout);
			if (ready < 0)
				break;
			if (ready == 0)
			{
				// Nothing yet: the next slice, unless the wait is over, the
				// drain is done, or the caller wants out.
				timeoutMs -= sliceMs;
				if (!messages.empty() || timeoutMs <= 0 || (interrupted && interrupted()))
					break;
				continue;
			}
			char buffer[512];
			sockaddr_in from{};
			int fromLength = sizeof(from);
			const int received = recvfrom(s, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
			if (received <= 0)
				break;
			buffer[received] = '\0';
			ScriptMessage message;
			if (ParseScriptMessage(buffer, message))
			{
				message.senderPort = ntohs(from.sin_port);
				messages.push_back(message);
			}
			timeoutMs = 0;   // drain what is already there, then return
		}
		return messages;
	}

	unsigned long UdpPortOwner(unsigned short port)
	{
		ULONG size = 0;
		GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
		for (int attempt = 0; attempt < 3 && size > 0; ++attempt)
		{
			std::vector<unsigned char> table(size);
			const DWORD result = GetExtendedUdpTable(table.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
			if (result == ERROR_INSUFFICIENT_BUFFER)
				continue;
			if (result != NO_ERROR)
				return 0;
			const auto* rows = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(table.data());
			for (DWORD i = 0; i < rows->dwNumEntries; ++i)
			{
				const MIB_UDPROW_OWNER_PID& row = rows->table[i];
				const unsigned long address = ntohl(row.dwLocalAddr);
				if (ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
					(address == INADDR_LOOPBACK || address == INADDR_ANY))
					return row.dwOwningPid;
			}
			return 0;
		}
		return 0;
	}

	namespace
	{
		std::string ListenDirectory(const std::string& directory)
		{
			if (!directory.empty())
				return directory.back() == '\\' || directory.back() == '/' ? directory : directory + "\\";
			const std::string log = DebugLog::GetExecutableLogFilePath();
			const size_t slash = log.find_last_of("\\/");
			return slash == std::string::npos ? std::string() : log.substr(0, slash + 1);
		}
	}

	std::string ListenCsvPath(const std::string& directory)
	{
		return ListenDirectory(directory) + "led-listen.csv";
	}

	std::string ListenNowPath(const std::string& directory)
	{
		return ListenDirectory(directory) + "led-listen-now.txt";
	}

	long long OpenListenCsv(std::ofstream& csv, const std::string& path, const std::string& first)
	{
		std::string header;
		{
			std::ifstream existing(path);
			std::getline(existing, header);
		}
		WIN32_FILE_ATTRIBUTE_DATA info{};
		const bool exists = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info) != 0;
		const long long size = exists ? (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow : 0;
		if (exists && (size > kListenCsvCapBytes || header != kListenCsvHeader))
			MoveFileExA(path.c_str(), (path.substr(0, path.size() - 4) + ".prev.csv").c_str(), MOVEFILE_REPLACE_EXISTING);
		const bool fresh = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
		csv.open(path, std::ios::out | std::ios::app);
		if (!csv)
			return 0;
		if (fresh)
			csv << kListenCsvHeader << "\n";
		csv << first << "\n";
		csv.flush();
		if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info))
			return 0;
		return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	}

	bool SendScriptMessageForTest(int port, const std::string& text)
	{
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			return false;
		const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		bool ok = false;
		if (s != INVALID_SOCKET)
		{
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_port = htons(static_cast<u_short>(port));
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			ok = sendto(s, text.c_str(), static_cast<int>(text.size()), 0,
				reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == static_cast<int>(text.size());
			closesocket(s);
		}
		WSACleanup();
		return ok;
	}
}
