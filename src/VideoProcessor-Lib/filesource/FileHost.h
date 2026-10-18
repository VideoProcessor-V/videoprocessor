#pragma once

#include "FileHostSettings.h"

#include <ConfigFile.h>
#include <DisplayRuleExpression.h>
#include <IRenderer.h>
#include <UnifiedProfileRuntime.h>
#include <VideoState.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// VP Renderer DirectShow filter (2026-10-17): the filter's equivalent of the
// capture app's profile ownership. FileHost finds and loads VideoProcessor.cfg,
// owns a UnifiedProfileRuntime whose rules see source=file, watches the file
// for edits (Config's Apply / Save, hand edits) and re-applies the resolved
// snapshot through IVideoRenderer::ApplyApplicationState, the same entry point
// VideoProcessor.exe uses. It also reads the filter's own [general] keys
// (file_av_offset_ms, file_led_holdover_ms; FileHostSettings.h).
namespace FileSource
{
	// Where VideoProcessor.cfg may live, in order; injectable for tests.
	struct FileHostEnvironment
	{
		// HKCU\Software\VideoProcessor\InstallDir (false when absent).
		std::function<bool(std::wstring&)> installDir;
		// The directory of the module FileHost is linked into (the filter .ax).
		std::function<std::wstring()> moduleDirectory;
		// %LOCALAPPDATA% (empty when unset).
		std::function<std::wstring()> localAppData;
		std::function<bool(const std::wstring&)> fileExists;

		// The real registry, module path, environment and file system. `module`
		// null: the module that contains this code.
		static FileHostEnvironment System(HMODULE module = nullptr);
	};

	// <InstallDir>\VideoProcessor.cfg, <module dir>\VideoProcessor.cfg,
	// %LOCALAPPDATA%\VideoProcessor\VideoProcessor.cfg (missing sources skipped).
	std::vector<std::wstring> ConfigurationCandidates(const FileHostEnvironment& environment);
	// The first candidate that exists.
	bool LocateConfiguration(const FileHostEnvironment& environment, std::wstring& path);

	// Coalesces a burst of change notifications (an editor's write, rename and
	// attribute changes) into one reload: due once the file has been quiet for
	// quietMs, or at the latest maxDelayMs after the first change of a burst.
	class ChangeDebouncer
	{
	public:
		static uint32_t NoWait() { return 0xFFFFFFFFu; }

		explicit ChangeDebouncer(uint32_t quietMs = 300, uint32_t maxDelayMs = 2000)
			: quiet(quietMs), maxDelay(maxDelayMs < quietMs ? quietMs : maxDelayMs)
		{
		}

		void OnChange(uint64_t nowMs)
		{
			if (!pending)
				firstMs = nowMs;
			pending = true;
			lastMs = nowMs;
		}

		// True once per burst, when the reload is due.
		bool Due(uint64_t nowMs)
		{
			if (!pending || (nowMs - lastMs < quiet && nowMs - firstMs < maxDelay))
				return false;
			pending = false;
			return true;
		}

		// How long to wait before Due() can become true (NoWait() when idle).
		uint32_t WaitMs(uint64_t nowMs) const
		{
			if (!pending)
				return NoWait();
			const uint64_t sinceLast = nowMs - lastMs;
			const uint64_t sinceFirst = nowMs - firstMs;
			if (sinceLast >= quiet || sinceFirst >= maxDelay)
				return 0;
			const uint64_t untilQuiet = quiet - sinceLast;
			const uint64_t untilMax = maxDelay - sinceFirst;
			return static_cast<uint32_t>(untilQuiet < untilMax ? untilQuiet : untilMax);
		}

		bool Pending() const { return pending; }

	private:
		uint32_t quiet;
		uint32_t maxDelay;
		bool pending = false;
		uint64_t firstMs = 0;
		uint64_t lastMs = 0;
	};

	class FileHost
	{
	public:
		struct ApplyOutcome
		{
			bool applied = false;
			bool rendererRestartRequired = false;
			bool liveResetRequired = false;
			std::string activeState;
			std::string reason;
		};
		// Told after every application to the renderer, outside FileHost's
		// locks; the filter decides what a restart or reset request means.
		using ApplyHandler = std::function<void(const ApplyOutcome&)>;

		explicit FileHost(FileHostEnvironment environment = FileHostEnvironment::System());
		~FileHost();
		FileHost(const FileHost&) = delete;
		FileHost& operator=(const FileHost&) = delete;

		// Locates and loads the configuration, initializes the profile runtime
		// and starts watching the file. False (with error) when no configuration
		// was found or its profiles were rejected; the settings then keep their
		// defaults (or the file's values) and a later valid edit is picked up.
		bool Start(std::string& error, bool watch = true);
		void Stop();

		// The renderer the snapshot is applied to (null: none). Applies the
		// current snapshot at once. Call with null before destroying the renderer.
		void SetRenderer(IVideoRenderer* renderer);
		void SetApplyHandler(ApplyHandler handler);
		// Rule context: the source state (the file's format), the renderer's
		// name and the display's measured refresh rate (0 = unknown).
		void UpdateVideoState(const VideoStateComPtr& state);
		void SetRendererName(const std::string& name);
		void SetActualRefreshRate(double hz);

		// Reloads the configuration if its content changed (the watcher calls
		// this after the debounce). True when a new configuration was taken.
		bool ReloadIfChanged(std::string& error);

		FileHostSettings Settings() const;
		int AvOffsetMs() const { return avOffsetMs.load(std::memory_order_acquire); }
		uint32_t LedHoldoverMs() const { return ledHoldoverMs.load(std::memory_order_acquire); }
		std::shared_ptr<const UnifiedProfileRuntime::Snapshot> Snapshot() const;
		std::wstring ConfigurationPath() const;
		// The rules' values as FileHost answers them (source=file); tests.
		DisplayRuleExpression::ValueLookup RuleLookup() const;

	private:
		DisplayRuleExpression::ValueLookup RuleLookupLocked() const;
		bool LoadLocked(const std::wstring& path, ConfigFile& config, std::string& error) const;
		void TakeSettingsLocked(const ConfigFile& config);
		void Apply(const std::shared_ptr<const UnifiedProfileRuntime::Snapshot>& snapshot, const char* reason);
		void Refresh(const char* reason);
		void WatchLoop(HANDLE change, std::wstring directory);

		FileHostEnvironment environment;
		mutable std::mutex mutex;            // state below
		std::mutex applyMutex;               // the renderer and its application
		UnifiedProfileRuntime::Runtime runtime;
		std::wstring configPath;
		uint64_t configIdentity = 0;
		bool configLoaded = false;
		FileHostSettings settings;
		VideoStateComPtr videoState;
		std::string rendererName;
		double actualRefreshHz = 0.0;
		IVideoRenderer* renderer = nullptr;
		uint64_t lastAppliedGeneration = 0;   // under applyMutex
		ApplyHandler applyHandler;
		std::atomic<int> avOffsetMs{ MainConfigSchema::FileAvOffsetDefaultMs };
		std::atomic<uint32_t> ledHoldoverMs{ static_cast<uint32_t>(MainConfigSchema::FileLedHoldoverDefaultMs) };
		HANDLE stopEvent = nullptr;
		std::thread watcher;
	};
}
