// VP Renderer filter (2026-10-17): FileHost - where VideoProcessor.cfg is
// looked for (registry InstallDir, the filter's folder, %LOCALAPPDATA%, with
// the registry and paths injected), the change debounce, the filter's own
// [general] settings, and the profile runtime it owns: rules see source=file,
// snapshots reach the renderer through ApplyApplicationState, and an edited
// file is re-applied once.
#include "pch.h"
#include "CppUnitTest.h"
#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <filesource/FileHost.h>
#include <Windows.h>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		std::wstring FileHostWide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

		FileSource::FileHostEnvironment FakeEnvironment(const std::wstring* installDir, const std::wstring& module,
			const std::wstring& localAppData, const std::set<std::wstring>& existing)
		{
			FileSource::FileHostEnvironment environment;
			const bool hasInstall = installDir != nullptr;
			const std::wstring install = hasInstall ? *installDir : std::wstring();
			environment.installDir = [hasInstall, install](std::wstring& directory)
			{
				directory = install;
				return hasInstall;
			};
			environment.moduleDirectory = [module]() { return module; };
			environment.localAppData = [localAppData]() { return localAppData; };
			environment.fileExists = [existing](const std::wstring& path) { return existing.count(path) != 0; };
			return environment;
		}

		struct FileHostTempDirectory
		{
			std::string path;
			FileHostTempDirectory()
			{
				char directory[MAX_PATH]{};
				Assert::IsTrue(GetTempPathA(MAX_PATH, directory) != 0);
				path = std::string(directory) + "vpfilehost_" + std::to_string(GetCurrentProcessId()) + "_" +
					std::to_string(GetTickCount64());
				Assert::IsTrue(CreateDirectoryA(path.c_str(), nullptr) != 0);
			}
			~FileHostTempDirectory()
			{
				DeleteFileA(ConfigPath().c_str());
				RemoveDirectoryA(path.c_str());
			}
			std::string ConfigPath() const { return path + "\\VideoProcessor.cfg"; }
			void Write(const std::string& text) const
			{
				std::ofstream out(ConfigPath(), std::ios::trunc | std::ios::binary);
				out << text;
			}
		};

		class FileHostFakeRenderer : public IVideoRenderer
		{
		public:
			size_t GetConvertedQueueSize() override { return 0; }
			bool OnVideoState(VideoStateComPtr&) override { return true; }
			void OnVideoFrame(VideoFrame&) override {}
			HRESULT OnWindowsEvent(LONG_PTR, LONG_PTR) override { return S_OK; }
			void Build() override {}
			void Start() override {}
			void Stop() override {}
			void Reset() override {}
			void OnSize() override {}
			void OnPaint() override {}
			void SetFrameQueueMaxSize(size_t) override {}
			void SetSceneAwareTimingCorrection(bool) override {}
			size_t GetFrameQueueSize() override { return 0; }
			double EntryLatencyMs() const override { return 0; }
			double ExitLatencyMs() const override { return 0; }
			uint64_t DroppedFrameCount() const override { return 0; }

			bool ApplyApplicationState(const UnifiedProfileRuntime::Snapshot& snapshot, CString& activeState,
				bool& rendererRestartRequired, bool& liveResetRequired) override
			{
				activeState = CString(TEXT("fake"));
				rendererRestartRequired = false;
				liveResetRequired = false;
				++applies;
				lastGeneration = snapshot.generation;
				std::string value;
				lastSource = snapshot.LookupVariable("source", value) ? value : std::string();
				const auto display = snapshot.effectiveSelections.find("display");
				lastDisplay = display != snapshot.effectiveSelections.end() && !display->second.empty() ?
					display->second.front() : std::string();
				std::string offset;
				lastOffset = snapshot.configuration &&
					snapshot.configuration->TryGetString("general", "file_av_offset_ms", offset) ? offset : std::string();
				return true;
			}

			int applies = 0;
			uint64_t lastGeneration = 0;
			std::string lastSource;
			std::string lastDisplay;
			std::string lastOffset;
		};

		const char* const kFileHostConfig =
			"[general]\npersist_profile_selection: false\nfile_av_offset_ms: -120\nfile_led_holdover_ms: 1500\n"
			"[vprenderer.everyday]\nquality: high\n"
			"[vprenderer.files]\nwhen: $source==\"file\"\nsdr_black_nits: 0.05\n"
			"[vprenderer.live]\nwhen: $source==\"capture\"\nsdr_black_nits: 0.1\n";
	}

	TEST_CLASS(FileHostTests)
	{
	public:
		TEST_METHOD(CandidatesFollowRegistryModuleThenLocalAppData)
		{
			const std::wstring install = L"C:\\Program Files\\VideoProcessor";
			const auto environment = FakeEnvironment(&install, L"D:\\filters\\", L"C:\\Users\\me\\AppData\\Local", {});
			const std::vector<std::wstring> candidates = FileSource::ConfigurationCandidates(environment);
			Assert::AreEqual(static_cast<size_t>(3), candidates.size());
			Assert::AreEqual(std::wstring(L"C:\\Program Files\\VideoProcessor\\VideoProcessor.cfg"), candidates[0]);
			Assert::AreEqual(std::wstring(L"D:\\filters\\VideoProcessor.cfg"), candidates[1]);
			Assert::AreEqual(std::wstring(L"C:\\Users\\me\\AppData\\Local\\VideoProcessor\\VideoProcessor.cfg"),
				candidates[2]);
		}

		TEST_METHOD(LocateTakesTheFirstThatExists)
		{
			const std::wstring install = L"C:\\VP";
			const std::wstring installCfg = L"C:\\VP\\VideoProcessor.cfg";
			const std::wstring moduleCfg = L"D:\\ax\\VideoProcessor.cfg";
			const std::wstring localCfg = L"E:\\local\\VideoProcessor\\VideoProcessor.cfg";
			std::wstring path;
			Assert::IsTrue(FileSource::LocateConfiguration(
				FakeEnvironment(&install, L"D:\\ax", L"E:\\local", { installCfg, moduleCfg, localCfg }), path));
			Assert::AreEqual(installCfg, path, L"the registry's install folder first");
			Assert::IsTrue(FileSource::LocateConfiguration(
				FakeEnvironment(&install, L"D:\\ax", L"E:\\local", { moduleCfg, localCfg }), path));
			Assert::AreEqual(moduleCfg, path, L"then the filter's own folder");
			Assert::IsTrue(FileSource::LocateConfiguration(
				FakeEnvironment(&install, L"D:\\ax", L"E:\\local", { localCfg }), path));
			Assert::AreEqual(localCfg, path, L"then %LOCALAPPDATA%\\VideoProcessor");
			Assert::IsTrue(FileSource::LocateConfiguration(
				FakeEnvironment(nullptr, L"D:\\ax", L"E:\\local", { installCfg, moduleCfg }), path));
			Assert::AreEqual(moduleCfg, path, L"no registry value: the install folder is not guessed");
			Assert::IsFalse(FileSource::LocateConfiguration(
				FakeEnvironment(&install, L"D:\\ax", L"E:\\local", {}), path));
			Assert::IsTrue(path.empty());
		}

		TEST_METHOD(SameFolderIsListedOnceAndEmptySourcesAreSkipped)
		{
			const std::wstring install = L"D:\\ax\\";
			const auto candidates = FileSource::ConfigurationCandidates(FakeEnvironment(&install, L"d:\\AX", L"", {}));
			Assert::AreEqual(static_cast<size_t>(1), candidates.size(), L"the install folder is the module folder");
			Assert::AreEqual(std::wstring(L"D:\\ax\\VideoProcessor.cfg"), candidates[0]);
		}

		TEST_METHOD(DebounceCoalescesABurst)
		{
			FileSource::ChangeDebouncer debouncer(300, 2000);
			Assert::AreEqual(FileSource::ChangeDebouncer::NoWait(), debouncer.WaitMs(0), L"idle");
			Assert::IsFalse(debouncer.Due(0));
			debouncer.OnChange(1000);
			debouncer.OnChange(1100);
			debouncer.OnChange(1200);
			Assert::IsFalse(debouncer.Due(1400), L"still inside the quiet period");
			Assert::AreEqual(100u, debouncer.WaitMs(1400));
			Assert::IsTrue(debouncer.Due(1500), L"quiet for 300 ms: due");
			Assert::IsFalse(debouncer.Due(1600), L"once per burst");
			Assert::IsFalse(debouncer.Pending());
		}

		TEST_METHOD(DebounceHasAMaximumDelay)
		{
			FileSource::ChangeDebouncer debouncer(300, 2000);
			uint64_t now = 0;
			bool due = false;
			for (; now <= 3000 && !due; now += 100)
			{
				debouncer.OnChange(now);
				due = debouncer.Due(now);
			}
			Assert::IsTrue(due, L"a file that keeps changing is still reloaded");
			Assert::AreEqual(static_cast<uint64_t>(2100), now, L"at the maximum delay after the first change");
		}

		TEST_METHOD(SettingsReadValidateAndDefault)
		{
			FileHostTempDirectory directory;
			directory.Write("[general]\nfile_av_offset_ms: -250\nfile_led_holdover_ms: 0\n");
			ConfigFile config;
			Assert::IsTrue(config.Load(directory.ConfigPath(), ConfigFile::ReadPolicy::Fresh));
			FileSource::FileHostSettings settings = FileSource::ReadFileHostSettings(config);
			Assert::AreEqual(-250, settings.avOffsetMs);
			Assert::AreEqual(0u, settings.ledHoldoverMs);

			directory.Write("[general]\nrenderer: VP Renderer\n");
			Assert::IsTrue(config.Load(directory.ConfigPath(), ConfigFile::ReadPolicy::Fresh));
			settings = FileSource::ReadFileHostSettings(config);
			Assert::AreEqual(0, settings.avOffsetMs, L"default offset");
			Assert::AreEqual(3000u, settings.ledHoldoverMs, L"default hold-over");

			directory.Write("[general]\nfile_av_offset_ms: 2001\nfile_led_holdover_ms: soon\n[vprenderer.a]\nquality: high\n");
			Assert::IsTrue(config.Load(directory.ConfigPath(), ConfigFile::ReadPolicy::Fresh));
			std::vector<std::string> problems;
			settings = FileSource::ReadFileHostSettings(config, &problems);
			Assert::AreEqual(0, settings.avOffsetMs, L"out of range keeps the default");
			Assert::AreEqual(3000u, settings.ledHoldoverMs);
			Assert::AreEqual(static_cast<size_t>(2), problems.size());
			std::string error;
			Assert::IsFalse(MainConfigSchema::Validate(config, error), L"the schema refuses them (Config shows why)");

			directory.Write("[general]\nfile_av_offset_ms: -2000\nfile_led_holdover_ms: 30000\n[vprenderer.a]\nquality: high\n");
			Assert::IsTrue(config.Load(directory.ConfigPath(), ConfigFile::ReadPolicy::Fresh));
			Assert::IsTrue(MainConfigSchema::Validate(config, error), FileHostWide(error).c_str());
		}

		TEST_METHOD(HostResolvesFileRulesAndAppliesToTheRenderer)
		{
			FileHostTempDirectory directory;
			directory.Write(kFileHostConfig);
			const std::wstring module = FileHostWide(directory.path);
			FileSource::FileHostEnvironment environment = FileSource::FileHostEnvironment::System();
			environment.installDir = [](std::wstring&) { return false; };
			environment.moduleDirectory = [module]() { return module; };
			environment.localAppData = []() { return std::wstring(); };
			FileSource::FileHost host(environment);
			std::string error;
			Assert::IsTrue(host.Start(error, false), FileHostWide(error).c_str());
			Assert::AreEqual(FileHostWide(directory.ConfigPath()), host.ConfigurationPath());
			Assert::AreEqual(-120, host.AvOffsetMs());
			Assert::AreEqual(1500u, host.LedHoldoverMs());
			std::string source;
			Assert::IsTrue(host.RuleLookup()("source", source));
			Assert::AreEqual(std::string("file"), source, L"the filter's rules see source=file");

			FileHostFakeRenderer renderer;
			int handled = 0;
			host.SetApplyHandler([&handled](const FileSource::FileHost::ApplyOutcome& outcome)
			{
				if (outcome.applied) ++handled;
			});
			host.SetRenderer(&renderer);
			Assert::AreEqual(1, renderer.applies, L"attaching the renderer applies the current snapshot");
			Assert::AreEqual(1, handled);
			Assert::AreEqual(std::string("file"), renderer.lastSource);
			Assert::AreEqual(std::string("files"), renderer.lastDisplay, L"the source==file profile is selected");

			Assert::IsFalse(host.ReloadIfChanged(error), L"unchanged content is not re-applied");
			Assert::AreEqual(1, renderer.applies);

			std::string edited(kFileHostConfig);
			edited.replace(edited.find("file_av_offset_ms: -120"), 23, "file_av_offset_ms: 40");
			directory.Write(edited);
			Assert::IsTrue(host.ReloadIfChanged(error), FileHostWide(error).c_str());
			Assert::AreEqual(40, host.AvOffsetMs(), L"the edited offset is taken");
			Assert::AreEqual(2, renderer.applies, L"the edit is applied once");
			Assert::AreEqual(std::string("40"), renderer.lastOffset, L"the renderer gets the new configuration");
			Assert::IsFalse(host.ReloadIfChanged(error));
			Assert::AreEqual(2, renderer.applies);

			// A rejected edit keeps the last profiles and does not reach the renderer.
			directory.Write(std::string(kFileHostConfig) + "[vprenderer.broken]\nwhen: $nonsense==\"x\"\n");
			Assert::IsFalse(host.ReloadIfChanged(error));
			Assert::IsFalse(error.empty());
			Assert::AreEqual(2, renderer.applies);

			host.SetRenderer(nullptr);
			directory.Write(kFileHostConfig);
			Assert::IsTrue(host.ReloadIfChanged(error), FileHostWide(error).c_str());
			Assert::AreEqual(2, renderer.applies, L"no renderer attached: nothing applied");
			host.Stop();
		}

		TEST_METHOD(MissingConfigurationKeepsDefaults)
		{
			FileSource::FileHost host(FakeEnvironment(nullptr, L"Z:\\nowhere", L"", {}));
			std::string error;
			Assert::IsFalse(host.Start(error, false));
			Assert::IsTrue(error.find("not found") != std::string::npos, FileHostWide(error).c_str());
			Assert::AreEqual(0, host.AvOffsetMs());
			Assert::AreEqual(3000u, host.LedHoldoverMs());
			Assert::IsTrue(host.Snapshot() == nullptr);
			FileHostFakeRenderer renderer;
			host.SetRenderer(&renderer);
			Assert::AreEqual(0, renderer.applies, L"no snapshot: nothing applied");
			host.SetRenderer(nullptr);
		}

		TEST_METHOD(WatcherReappliesAnEditedFile)
		{
			FileHostTempDirectory directory;
			directory.Write(kFileHostConfig);
			const std::wstring module = FileHostWide(directory.path);
			FileSource::FileHostEnvironment environment = FileSource::FileHostEnvironment::System();
			environment.installDir = [](std::wstring&) { return false; };
			environment.moduleDirectory = [module]() { return module; };
			FileSource::FileHost host(environment);
			std::string error;
			Assert::IsTrue(host.Start(error, true), FileHostWide(error).c_str());
			std::string edited(kFileHostConfig);
			edited.replace(edited.find("file_av_offset_ms: -120"), 23, "file_av_offset_ms: 75");
			directory.Write(edited);
			const ULONGLONG deadline = GetTickCount64() + 10000;
			while (host.AvOffsetMs() != 75 && GetTickCount64() < deadline)
				Sleep(20);
			Assert::AreEqual(75, host.AvOffsetMs(), L"the watcher picked up the edit after the debounce");
			host.Stop();
		}
	};
}
