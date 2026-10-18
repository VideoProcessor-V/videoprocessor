#include <pch.h>

#include "VPRendererFilterLog.h"

#include <DebugLog.h>

#include <cstdio>
#include <mutex>
#include <share.h>


namespace VPRendererFilterLog
{
	namespace
	{
		// One log file per session; a previous one over this size is kept as .1.
		constexpr long long kRotateBytes = 64ll * 1024 * 1024;

		std::mutex& Mutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		FILE*& File()
		{
			static FILE* file = nullptr;
			return file;
		}

		std::wstring& Path()
		{
			static std::wstring path;
			return path;
		}

		std::wstring DefaultPath()
		{
			wchar_t buffer[MAX_PATH * 2] = {};
			DWORD length = GetEnvironmentVariableW(L"VP_RENDERER_FILTER_LOG", buffer,
				static_cast<DWORD>(_countof(buffer)));
			if (length > 0 && length < _countof(buffer))
				return buffer;
			length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
				static_cast<DWORD>(_countof(buffer)));
			if (length == 0 || length >= _countof(buffer))
				return std::wstring();
			return std::wstring(buffer) + L"\\VideoProcessor\\logs\\vprfilter.log";
		}

		void CreateParentDirectories(const std::wstring& path)
		{
			for (size_t i = path.find_first_of(L"\\/", 3); i != std::wstring::npos;
				i = path.find_first_of(L"\\/", i + 1))
				CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
		}

		void OpenLocked()
		{
			const std::wstring path = DefaultPath();
			if (path.empty())
				return;
			CreateParentDirectories(path);
			WIN32_FILE_ATTRIBUTE_DATA attributes = {};
			if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
			{
				const long long size = (static_cast<long long>(attributes.nFileSizeHigh) << 32) |
					attributes.nFileSizeLow;
				if (size > kRotateBytes)
					MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
			}
			// Bug hunt 3 (C11): shared for writing, so a second player (or a second
			// graph in another process) can log too instead of logging nowhere.
			File() = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
			if (File())
				Path() = path;
		}
	}


	void Initialize()
	{
		{
			std::lock_guard<std::mutex> lock(Mutex());
			if (File() || !Path().empty())
				return;
			Path() = L"(unavailable)";
			OpenLocked();
		}
		DebugLog::SetExternalSink(&Sink);
		DebugLog::Log("=== VP Renderer filter log session start (pid %lu) ===",
			GetCurrentProcessId());
	}


	void __cdecl Sink(const char* message)
	{
		if (!message)
			return;
		SYSTEMTIME now;
		GetLocalTime(&now);
		std::lock_guard<std::mutex> lock(Mutex());
		FILE* file = File();
		if (!file)
			return;
		fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u.%03u | %5lu | %s\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
			now.wMilliseconds, GetCurrentThreadId(), message);
		fflush(file);
	}


	std::wstring ActivePath()
	{
		std::lock_guard<std::mutex> lock(Mutex());
		return Path();
	}
}
