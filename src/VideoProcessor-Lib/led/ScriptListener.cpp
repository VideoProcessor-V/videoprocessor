#include <pch.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "ScriptListener.h"
#include "../DebugLog.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")

namespace LedDrive
{
	bool ParseScriptMessage(const std::string& text, ScriptMessage& out)
	{
		std::istringstream in(text);
		std::string tag;
		ScriptMessage message;
		if (!(in >> tag) || tag != "VP1" || !(in >> message.kind) || !(in >> message.value))
			return false;
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
			if (end == value.c_str())
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

	std::vector<ScriptMessage> ScriptListener::Receive(int waitMs)
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
			timeval timeout{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
			if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
				break;
			char buffer[512];
			const int received = recv(s, buffer, sizeof(buffer) - 1, 0);
			if (received <= 0)
				break;
			buffer[received] = '\0';
			ScriptMessage message;
			if (ParseScriptMessage(buffer, message))
				messages.push_back(message);
			timeoutMs = 0;   // drain what is already there, then return
		}
		return messages;
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
