#include <pch.h>
#include "FileHost.h"

#include <DebugLog.h>
#include <SourceRuleVariable.h>
#include <StateVariables.h>

#include <locale>
#include <sstream>

namespace FileSource
{
	namespace
	{
		const wchar_t* const kConfigFileName = L"VideoProcessor.cfg";
		// Any object in this module: GetModuleHandleEx finds the module containing it.
		const int kModuleAnchor = 0;

		std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
		{
			if (directory.empty())
				return std::wstring();
			const wchar_t last = directory.back();
			return last == L'\\' || last == L'/' ? directory + name : directory + L"\\" + name;
		}

		std::wstring ParentDirectory(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
		}

		// ConfigFile opens narrow (ANSI) paths.
		std::string ToAnsi(const std::wstring& text)
		{
			if (text.empty())
				return std::string();
			const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
				nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return std::string();
			std::string result(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), &result[0], size,
				nullptr, nullptr);
			return result;
		}

		std::string ToLog(const std::wstring& text)
		{
			return ToAnsi(text);
		}
	}


	FileHostEnvironment FileHostEnvironment::System(HMODULE module)
	{
		FileHostEnvironment environment;
		environment.installDir = [](std::wstring& directory)
		{
			directory.clear();
			wchar_t buffer[4096] = {};
			DWORD size = sizeof(buffer);
			const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\VideoProcessor", L"InstallDir",
				RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer, &size);
			if (status != ERROR_SUCCESS)
				return false;
			directory = buffer;
			return !directory.empty();
		};
		environment.moduleDirectory = [module]()
		{
			HMODULE owner = module;
			if (!owner && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&kModuleAnchor), &owner))
				return std::wstring();
			std::wstring path(32768, L'\0');
			const DWORD length = GetModuleFileNameW(owner, &path[0], static_cast<DWORD>(path.size()));
			if (length == 0 || length >= path.size())
				return std::wstring();
			path.resize(length);
			return ParentDirectory(path);
		};
		environment.localAppData = []()
		{
			wchar_t buffer[4096] = {};
			const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 4096);
			return length == 0 || length >= 4096 ? std::wstring() : std::wstring(buffer, length);
		};
		environment.fileExists = [](const std::wstring& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
		};
		return environment;
	}


	std::vector<std::wstring> ConfigurationCandidates(const FileHostEnvironment& environment)
	{
		std::vector<std::wstring> candidates;
		auto add = [&candidates](const std::wstring& path)
		{
			if (path.empty())
				return;
			for (const std::wstring& existing : candidates)
				if (_wcsicmp(existing.c_str(), path.c_str()) == 0)
					return;
			candidates.push_back(path);
		};
		std::wstring installDir;
		if (environment.installDir && environment.installDir(installDir))
			add(JoinPath(installDir, kConfigFileName));
		if (environment.moduleDirectory)
			add(JoinPath(environment.moduleDirectory(), kConfigFileName));
		if (environment.localAppData)
		{
			const std::wstring localAppData = environment.localAppData();
			if (!localAppData.empty())
				add(JoinPath(JoinPath(localAppData, L"VideoProcessor"), kConfigFileName));
		}
		return candidates;
	}


	bool LocateConfiguration(const FileHostEnvironment& environment, std::wstring& path)
	{
		path.clear();
		if (!environment.fileExists)
			return false;
		for (const std::wstring& candidate : ConfigurationCandidates(environment))
			if (environment.fileExists(candidate))
			{
				path = candidate;
				return true;
			}
		return false;
	}


	FileHost::FileHost(FileHostEnvironment environmentIn)
		: environment(std::move(environmentIn))
	{
	}


	FileHost::~FileHost()
	{
		Stop();
	}


	bool FileHost::Start(std::string& error, bool watch)
	{
		error.clear();
		std::wstring path;
		if (!LocateConfiguration(environment, path))
		{
			error = "VideoProcessor.cfg not found (HKCU\\Software\\VideoProcessor\\InstallDir, the filter's folder, "
				"%LOCALAPPDATA%\\VideoProcessor); file playback uses the defaults";
			DebugLog::Log("File host: %s", error.c_str());
			return false;
		}
		// The change notification is registered before the file is read, so an
		// edit made at any moment after Start returns (or during it) is seen.
		HANDLE change = INVALID_HANDLE_VALUE;
		if (watch && !watcher.joinable())
		{
			change = FindFirstChangeNotificationW(ParentDirectory(path).c_str(), FALSE,
				FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
			if (change == INVALID_HANDLE_VALUE)
				DebugLog::Log("File host: cannot watch %s (error %lu); configuration edits apply on the next file",
					ToLog(ParentDirectory(path)).c_str(), GetLastError());
		}
		std::shared_ptr<const UnifiedProfileRuntime::Snapshot> snapshot;
		bool initialized = false;
		{
			std::lock_guard<std::mutex> guard(mutex);
			configPath = path;
			ConfigFile config;
			if (LoadLocked(path, config, error))
			{
				configLoaded = true;
				configIdentity = config.GetContentIdentity();
				TakeSettingsLocked(config);
				initialized = runtime.IsInitialized() || runtime.Initialize(config, RuleLookupLocked(), error);
				if (initialized)
					snapshot = runtime.GetSnapshot();
			}
		}
		DebugLog::Log("File host: configuration %s (%s)", ToLog(path).c_str(),
			initialized ? "profiles resolved, rules see source=file" : error.c_str());
		if (change != INVALID_HANDLE_VALUE)
		{
			stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (stopEvent)
				watcher = std::thread(&FileHost::WatchLoop, this, change, ParentDirectory(path));
			else
			{
				DebugLog::Log("File host: cannot watch the configuration (CreateEvent error %lu)", GetLastError());
				FindCloseChangeNotification(change);
			}
		}
		if (snapshot)
			Apply(snapshot, "start");
		return initialized;
	}


	void FileHost::Stop()
	{
		if (stopEvent)
			SetEvent(stopEvent);
		if (watcher.joinable())
			watcher.join();
		if (stopEvent)
		{
			CloseHandle(stopEvent);
			stopEvent = nullptr;
		}
	}


	void FileHost::SetRenderer(IVideoRenderer* rendererIn)
	{
		{
			std::lock_guard<std::mutex> guard(applyMutex);
			renderer = rendererIn;
			lastAppliedGeneration = 0;
		}
		if (rendererIn)
			Apply(runtime.GetSnapshot(), "renderer");
	}


	void FileHost::SetApplyHandler(ApplyHandler handler)
	{
		std::lock_guard<std::mutex> guard(mutex);
		applyHandler = std::move(handler);
	}


	void FileHost::UpdateVideoState(const VideoStateComPtr& state)
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			videoState = state;
		}
		Refresh("video-state");
	}


	void FileHost::SetRendererName(const std::string& name)
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (rendererName == name)
				return;
			rendererName = name;
		}
		Refresh("renderer-name");
	}


	void FileHost::SetActualRefreshRate(double hz)
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (actualRefreshHz == hz)
				return;
			actualRefreshHz = hz;
		}
		Refresh("display-refresh");
	}


	bool FileHost::ReloadIfChanged(std::string& error)
	{
		error.clear();
		std::shared_ptr<const UnifiedProfileRuntime::Snapshot> snapshot;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (configPath.empty())
			{
				error = "no configuration located";
				return false;
			}
			ConfigFile config;
			if (!LoadLocked(configPath, config, error))
			{
				DebugLog::Log("File host: reload of %s failed (%s); keeping the last configuration",
					ToLog(configPath).c_str(), error.c_str());
				return false;
			}
			if (configLoaded && config.GetContentIdentity() == configIdentity)
				return false;   // another file in the folder changed, or the same content was saved
			configLoaded = true;
			configIdentity = config.GetContentIdentity();
			TakeSettingsLocked(config);
			if (runtime.IsInitialized())
			{
				UnifiedProfileRuntime::RefreshResult result;
				if (!runtime.Reload(config, RuleLookupLocked(), result, error))
				{
					DebugLog::Log("File host: configuration edit rejected (%s); keeping the last profiles",
						error.c_str());
					return false;
				}
				snapshot = result.snapshot;
			}
			else if (runtime.Initialize(config, RuleLookupLocked(), error))
				snapshot = runtime.GetSnapshot();
			else
			{
				DebugLog::Log("File host: configuration edit still rejected (%s)", error.c_str());
				return false;
			}
		}
		DebugLog::Log("File host: configuration reloaded (av offset %d ms, LED hold-over %u ms)",
			AvOffsetMs(), LedHoldoverMs());
		Apply(snapshot, "configuration");
		return true;
	}


	FileHostSettings FileHost::Settings() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return settings;
	}


	std::shared_ptr<const UnifiedProfileRuntime::Snapshot> FileHost::Snapshot() const
	{
		return runtime.GetSnapshot();
	}


	std::wstring FileHost::ConfigurationPath() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return configPath;
	}


	DisplayRuleExpression::ValueLookup FileHost::RuleLookup() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return RuleLookupLocked();
	}


	DisplayRuleExpression::ValueLookup FileHost::RuleLookupLocked() const
	{
		const DisplayRuleExpression::ValueLookup stateValues = videoState ?
			StateVariables::VideoStateLookup(videoState) : DisplayRuleExpression::ValueLookup();
		const std::string name = rendererName;
		const double refresh = actualRefreshHz;
		const DisplayRuleExpression::ValueLookup base =
			[stateValues, name, refresh](const std::string& key, std::string& value)
		{
			if (key == "renderer")
			{
				if (name.empty()) return false;
				value = name;
				return true;
			}
			if (key == "actual_refresh")
			{
				if (refresh <= 0.0) return false;
				std::ostringstream text;
				text.imbue(std::locale::classic());
				text.precision(17);
				text << refresh;
				value = text.str();
				return true;
			}
			return stateValues ? stateValues(key, value) : false;
		};
		return SourceRuleVariable::WithSource(base, SourceRuleVariable::kFile);
	}


	bool FileHost::LoadLocked(const std::wstring& path, ConfigFile& config, std::string& error) const
	{
		if (!config.Load(ToAnsi(path), ConfigFile::ReadPolicy::Fresh))
		{
			error = "cannot read " + ToLog(path);
			for (const std::string& warning : config.GetWarnings())
				error += "; " + warning;
			return false;
		}
		return true;
	}


	void FileHost::TakeSettingsLocked(const ConfigFile& config)
	{
		std::vector<std::string> problems;
		settings = ReadFileHostSettings(config, &problems);
		for (const std::string& problem : problems)
			DebugLog::Log("File host: %s", problem.c_str());
		avOffsetMs.store(settings.avOffsetMs, std::memory_order_release);
		ledHoldoverMs.store(settings.ledHoldoverMs, std::memory_order_release);
	}


	void FileHost::Refresh(const char* reason)
	{
		std::shared_ptr<const UnifiedProfileRuntime::Snapshot> snapshot;
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (!runtime.IsInitialized())
				return;
			UnifiedProfileRuntime::RefreshResult result;
			std::string error;
			const bool sourceContextAvailable = videoState && videoState->valid;
			if (!runtime.Refresh(RuleLookupLocked(), sourceContextAvailable, result, error))
			{
				DebugLog::Log("File host: rule refresh failed (%s): %s", reason, error.c_str());
				return;
			}
			if (result.deferred || !result.changed)
				return;
			snapshot = result.snapshot;
		}
		Apply(snapshot, reason);
	}


	void FileHost::Apply(const std::shared_ptr<const UnifiedProfileRuntime::Snapshot>& snapshot,
		const char* reason)
	{
		if (!snapshot)
			return;
		ApplyOutcome outcome;
		outcome.reason = reason ? reason : "";
		{
			std::lock_guard<std::mutex> applyGuard(applyMutex);
			if (!renderer)
				return;
			// Two threads (the watcher, the filter's) may race; never step back
			// to an older snapshot.
			if (snapshot->generation < lastAppliedGeneration)
				return;
			CString activeState;
			try
			{
				outcome.applied = renderer->ApplyApplicationState(*snapshot, activeState,
					outcome.rendererRestartRequired, outcome.liveResetRequired);
			}
			catch (const std::exception& e)
			{
				DebugLog::Log("File host: applying the profile state failed: %s", e.what());
				outcome.applied = false;
			}
			outcome.activeState = CStringA(activeState).GetString();
			if (outcome.applied)
				lastAppliedGeneration = snapshot->generation;
		}
		DebugLog::Log("File host: profile state %s (%s, generation %llu): %s%s%s",
			outcome.applied ? "applied" : "not applied", outcome.reason.c_str(),
			static_cast<unsigned long long>(snapshot->generation), outcome.activeState.c_str(),
			outcome.rendererRestartRequired ? "; renderer restart requested" : "",
			outcome.liveResetRequired ? "; live reset requested" : "");
		ApplyHandler handler;
		{
			std::lock_guard<std::mutex> guard(mutex);
			handler = applyHandler;
		}
		if (handler)
			handler(outcome);
	}


	void FileHost::WatchLoop(HANDLE change, std::wstring directory)
	{
		// `change` was registered by Start before the configuration was read.
		ChangeDebouncer debouncer;
		for (;;)
		{
			const uint32_t wait = debouncer.WaitMs(GetTickCount64());
			HANDLE handles[2] = { stopEvent, change };
			const DWORD signalled = WaitForMultipleObjects(2, handles, FALSE,
				wait == ChangeDebouncer::NoWait() ? INFINITE : static_cast<DWORD>(wait));
			if (signalled == WAIT_OBJECT_0)
				break;
			if (signalled == WAIT_OBJECT_0 + 1)
			{
				debouncer.OnChange(GetTickCount64());
				if (!FindNextChangeNotification(change))
				{
					DebugLog::Log("File host: watching %s stopped (error %lu)", ToLog(directory).c_str(),
						GetLastError());
					break;
				}
				continue;
			}
			if (signalled != WAIT_TIMEOUT)
			{
				DebugLog::Log("File host: watch wait failed (error %lu)", GetLastError());
				break;
			}
			if (debouncer.Due(GetTickCount64()))
			{
				std::string error;
				ReloadIfChanged(error);
			}
		}
		FindCloseChangeNotification(change);
	}
}
