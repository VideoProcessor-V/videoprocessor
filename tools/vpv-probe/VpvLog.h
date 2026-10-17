#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * VPV probe: line-buffered log to the console and a file. Every NVAPI call
 * goes through NV_LOGGED so its name, status code, message and duration are
 * recorded, flushed immediately (the process may be killed mid-sequence).
 */
#include <Windows.h>
#include <nvapi.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vpv
{
	class Log
	{
	public:
		static Log& Get() { static Log log; return log; }

		void Open(const char* path)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_file)
				fclose(m_file);
			m_file = path && *path ? _fsopen(path, "a", _SH_DENYNO) : nullptr;
		}

		void Write(const char* format, ...)
		{
			char text[2048];
			va_list args;
			va_start(args, format);
			vsnprintf(text, sizeof(text), format, args);
			va_end(args);
			SYSTEMTIME t;
			GetLocalTime(&t);
			char line[2200];
			snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [pid %lu tid %lu] %s\n",
				t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
				GetCurrentProcessId(), GetCurrentThreadId(), text);
			std::lock_guard<std::mutex> lock(m_mutex);
			fputs(line, stdout);
			fflush(stdout);
			if (m_file)
			{
				fputs(line, m_file);
				fflush(m_file);
			}
		}

	private:
		std::mutex m_mutex;
		FILE* m_file = nullptr;
	};

	inline NvAPI_Status LogStatus(const char* call, NvAPI_Status status, double ms)
	{
		NvAPI_ShortString message{};
		if (NvAPI_GetErrorMessage(status, message) != NVAPI_OK)
			snprintf(message, sizeof(message), "(no message)");
		Log::Get().Write("NVAPI %-48s -> %d %s (%.1f ms)", call, static_cast<int>(status), message, ms);
		return status;
	}
}

#define VPV_LOG(...) ::vpv::Log::Get().Write(__VA_ARGS__)

// Evaluates an NVAPI call once, logs it, yields its NvAPI_Status.
#define NV_LOGGED(label, expr) ([&]() -> NvAPI_Status { \
		const auto vpvStart = std::chrono::steady_clock::now(); \
		const NvAPI_Status vpvStatus = (expr); \
		const double vpvMs = std::chrono::duration<double, std::milli>( \
			std::chrono::steady_clock::now() - vpvStart).count(); \
		return ::vpv::LogStatus(label, vpvStatus, vpvMs); }())
