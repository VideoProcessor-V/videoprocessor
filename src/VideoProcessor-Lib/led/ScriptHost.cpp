#include <pch.h>

#include "ScriptHost.h"
#include "ScriptListener.h"
#include "../DebugLog.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <vector>

namespace LedDrive
{
	namespace
	{
		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return std::wstring();
			const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
			std::wstring out(n > 0 ? n - 1 : 0, L'\0');
			if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], n);
			return out;
		}

		std::string Folder(const std::string& path)
		{
			const size_t slash = path.find_last_of("\\/");
			return slash == std::string::npos ? std::string() : path.substr(0, slash);
		}

		// The logs folder (where led-listen-now.txt goes).
		std::string LogFolder(const std::string& logDirectory)
		{
			return Folder(ListenNowPath(logDirectory));
		}

		std::string Stamp()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			char text[32];
			std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay,
				t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			return text;
		}
	}

	ScriptHost::~ScriptHost()
	{
		Stop();
	}

	int ScriptHost::SecondsToRestart() const
	{
		if (state != State::Waiting) return 0;
		const auto left = std::chrono::duration_cast<std::chrono::seconds>(restartAt - std::chrono::steady_clock::now()).count();
		return static_cast<int>(std::max<long long>(0, left));
	}

	bool ScriptHost::ConfigurePort(const std::string& port, int baud, std::string& error)
	{
		const std::string path = "\\\\.\\" + port;
		HANDLE opened = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (opened == INVALID_HANDLE_VALUE)
		{
			error = "cannot open " + port + " (Windows error " + std::to_string(GetLastError()) + ")";
			return false;
		}
		DCB dcb{};
		dcb.DCBlength = sizeof(dcb);
		bool ok = GetCommState(opened, &dcb) != FALSE;
		if (ok)
		{
			// As Win32SerialPort (the projector driver) and Tera Term's setup.
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
			ok = SetCommState(opened, &dcb) != FALSE;
		}
		if (!ok)
			error = "cannot set " + port + " (Windows error " + std::to_string(GetLastError()) + ")";
		CloseHandle(opened);
		return ok;
	}

	void ScriptHost::Update(const std::string& script, const std::string& python, const std::string& logDirectory,
		const std::string& port, int baud, bool relay)
	{
		nowPath = LogFolder(logDirectory) + "\\led-script-now.txt";
		// A new script or interpreter, or relay switched: start again now.
		if (script != configuredScript || python != configuredPython || relay != configuredRelay)
		{
			Stop();
			configuredScript = script;
			configuredPython = python;
			configuredRelay = relay;
			backoffSeconds = 2;
			restartAt = std::chrono::steady_clock::now();
		}
		if (script.empty())
		{
			Stop();
			return;
		}
		if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
			Reap();
		if (!process && std::chrono::steady_clock::now() >= restartAt)
		{
			// The port's line settings first (the script's `echo ... > com1`
			// uses whatever the port was left at).
			portError.clear();
			portSetup = PortSetup::Skipped;
			if (!port.empty())
			{
				if (ConfigurePort(port, baud, portError))
				{
					portSetup = PortSetup::Applied;
					DebugLog::Log("LED drive: listen script: %s set to %d 8N1", port.c_str(), baud);
				}
				else
				{
					portSetup = PortSetup::Failed;
					DebugLog::Log("LED drive: listen script: %s not set (%s); starting the script anyway", port.c_str(), portError.c_str());
				}
			}
			if (!Start(script, python, logDirectory))
			{
				state = State::Waiting;
				restartAt = std::chrono::steady_clock::now() + std::chrono::seconds(backoffSeconds);
				DebugLog::Log("LED drive: listen script not started (%s); again in %d s", lastError.c_str(), backoffSeconds);
				backoffSeconds = std::min(60, backoffSeconds * 2);
			}
		}
		WriteNow(logDirectory);
	}

	bool ScriptHost::Start(const std::string& script, const std::string& python, const std::string& logDirectory)
	{
		if (GetFileAttributesW(Wide(script).c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			lastError = "script not found: " + script;
			return false;
		}
		// The script's output: logs\led-script.log, the previous kept past 10 MB.
		const std::string folder = LogFolder(logDirectory);
		CreateDirectoryW(Wide(folder).c_str(), nullptr);
		const std::string logPath = folder + "\\led-script.log";
		WIN32_FILE_ATTRIBUTE_DATA info{};
		if (GetFileAttributesExW(Wide(logPath).c_str(), GetFileExInfoStandard, &info) &&
			(info.nFileSizeHigh > 0 || info.nFileSizeLow > 10u * 1024u * 1024u))
			MoveFileExW(Wide(logPath).c_str(), Wide(folder + "\\led-script.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);

		SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
		HANDLE outputWrite = nullptr;
		if (!CreatePipe(&outputRead, &outputWrite, &inherit, 0))
		{
			lastError = "no pipe";
			return false;
		}
		SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
		HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = input;
		startup.hStdOutput = outputWrite;
		startup.hStdError = outputWrite;
		const std::string interpreter = python.empty() ? std::string("python") : python;
		std::wstring command = L"\"" + Wide(interpreter) + L"\" -u \"" + Wide(script) + L"\"";
		const std::wstring directory = Wide(Folder(script));
		// VP's environment, with VP_LED_RELAY=1 when VP relays (and never
		// otherwise, even if VP itself was started with it).
		std::wstring environment;
		if (const LPWCH block = GetEnvironmentStringsW())
		{
			for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1)
				if (_wcsnicmp(entry, L"VP_LED_RELAY=", 13) != 0)
					environment.append(entry).push_back(L'\0');
			FreeEnvironmentStringsW(block);
		}
		if (configuredRelay)
			environment.append(L"VP_LED_RELAY=1").push_back(L'\0');
		environment.push_back(L'\0');
		PROCESS_INFORMATION started{};
		const BOOL ok = CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, &environment[0],
			directory.empty() ? nullptr : directory.c_str(), &startup, &started);
		const DWORD createError = ok ? 0 : GetLastError();
		CloseHandle(outputWrite);
		if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
		if (!ok)
		{
			CloseHandle(outputRead);
			outputRead = nullptr;
			lastError = "cannot run " + interpreter + " (error " + std::to_string(createError) + ")";
			return false;
		}
		// The job ends the script with VP (even after a crash).
		job = CreateJobObjectW(nullptr, nullptr);
		if (job)
		{
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
			if (!AssignProcessToJobObject(job, started.hProcess))
			{
				// Without the job only the process itself can be ended (audit
				// 2026-10-14): Stop() and Reap() then terminate it directly.
				DebugLog::Log("LED drive: listen script not in a job object (error %lu); it is ended directly",
					static_cast<unsigned long>(GetLastError()));
				CloseHandle(job);
				job = nullptr;
			}
		}
		ResumeThread(started.hThread);
		CloseHandle(started.hThread);
		process = started.hProcess;
		pid = started.dwProcessId;
		state = State::Running;
		startedAt = std::chrono::steady_clock::now();
		++starts;
		lastError.clear();
		// Each line of output, time-stamped, into the log.
		HANDLE pipe = outputRead;
		const DWORD childPid = pid;
		reader = std::thread([pipe, logPath, childPid, script]()
		{
			std::ofstream log(Wide(logPath), std::ios::out | std::ios::app);
			if (log)
				log << Stamp() << " VP: started " << script << " (pid " << childPid << ")\n" << std::flush;
			std::string line;
			char buffer[4096];
			DWORD got = 0;
			while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
			{
				for (DWORD i = 0; i < got; ++i)
				{
					const char c = buffer[i];
					if (c == '\r') continue;
					if (c == '\n')
					{
						if (log) log << Stamp() << " " << line << "\n";
						line.clear();
					}
					else
						line += c;
				}
				if (log) log.flush();
			}
			if (log)
			{
				if (!line.empty()) log << Stamp() << " " << line << "\n";
				log << Stamp() << " VP: output ended (pid " << childPid << ")\n";
			}
		});
		DebugLog::Log("LED drive: listen script started: %s (pid %lu, start %d)", script.c_str(),
			static_cast<unsigned long>(pid), starts);
		return true;
	}

	void ScriptHost::Reap()
	{
		DWORD code = 0;
		GetExitCodeProcess(process, &code);
		const auto ran = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt).count();
		if (ran >= 60)
			backoffSeconds = 2;   // it ran: a fresh fault starts the back-off again
		// Anything the script started holds the output pipe open: end the job.
		if (job) TerminateJobObject(job, code);
		CloseHandle(process);
		process = nullptr;
		if (reader.joinable()) reader.join();   // the pipe closed with the process
		if (outputRead) { CloseHandle(outputRead); outputRead = nullptr; }
		if (job) { CloseHandle(job); job = nullptr; }
		state = State::Waiting;
		restartAt = std::chrono::steady_clock::now() + std::chrono::seconds(backoffSeconds);
		DebugLog::Log("LED drive: listen script exited (code %lu after %lld s); again in %d s",
			static_cast<unsigned long>(code), static_cast<long long>(ran), backoffSeconds);
		lastError = "exited with code " + std::to_string(code);
		backoffSeconds = std::min(60, backoffSeconds * 2);
		pid = 0;
	}

	void ScriptHost::Stop()
	{
		if (process)
		{
			// The job ends it and anything it started; the serial port closes
			// with it. The projector keeps the script's last values until VP's
			// next mode drives it.
			if (job) TerminateJobObject(job, 1);
			else TerminateProcess(process, 1);
			WaitForSingleObject(process, 5000);
			CloseHandle(process);
			process = nullptr;
			DebugLog::Log("LED drive: listen script stopped (pid %lu)", static_cast<unsigned long>(pid));
		}
		if (reader.joinable()) reader.join();
		if (outputRead) { CloseHandle(outputRead); outputRead = nullptr; }
		if (job) { CloseHandle(job); job = nullptr; }
		pid = 0;
		const bool wasOn = state != State::Off;
		state = State::Off;
		if (wasOn && !nowPath.empty())
		{
			std::ofstream now(Wide(nowPath), std::ios::out | std::ios::trunc);
			if (now) now << "state=stopped starts=" << starts << "\n";
			writtenNow.clear();
		}
	}

	void ScriptHost::WriteNow(const std::string& /*logDirectory*/)
	{
		std::string text = state == State::Running ? "state=running pid=" + std::to_string(pid) :
			state == State::Waiting ? "state=waiting restart_s=" + std::to_string(SecondsToRestart()) : std::string("state=stopped");
		text += " starts=" + std::to_string(starts);
		if (!lastError.empty())
		{
			std::string error = lastError;
			std::replace(error.begin(), error.end(), ' ', '_');
			text += " last=" + error;
		}
		if (portSetup != PortSetup::None)
			text += std::string(" port=") + (portSetup == PortSetup::Applied ? "applied" : portSetup == PortSetup::Skipped ? "skipped" : "failed");
		if (!portError.empty())
		{
			std::string error = portError;
			std::replace(error.begin(), error.end(), ' ', '_');
			text += " port_error=" + error;
		}
		if (text == writtenNow) return;
		std::ofstream now(Wide(nowPath), std::ios::out | std::ios::trunc);
		if (now)
		{
			now << text << "\n";
			writtenNow = text;
		}
	}
}
