#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/control/diagnostics/DiagnosticsAdapter.h>
#include <VP-V/modules/control/diagnostics/DiagnosticsReportRun.h>
#include <VP-V/modules/input/signal/DvCompareCaptureSink.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// WP-17: the diagnostics adapter must write exactly what the host wrote before the extraction.
// Each "Old..." function below is a verbatim copy of the host code it replaced
// (LibplaceboVideoRenderer.cpp / VideoProcessorDlg.cpp at chore/early-migrations ac49a1de), with
// members turned into parameters and GetTickCount64()/Clock::now() made injectable.
namespace
{
	namespace DH = DiagnosticHistory;

	// The recorder is 64-byte aligned (seqlock slots): allocate it aligned, as DiagnosticHistoryTests does.
	struct RecorderDeleter
	{
		void operator()(DH::Recorder* r) const
		{
			if (r)
			{
				r->~Recorder();
				_aligned_free(r);
			}
		}
	};
	using RecorderPtr = std::unique_ptr<DH::Recorder, RecorderDeleter>;
	RecorderPtr NewRecorder()
	{
		void* memory = _aligned_malloc(sizeof(DH::Recorder), 64);
		if (!memory)
			throw std::bad_alloc();
		return RecorderPtr(new (memory) DH::Recorder());
	}

	// ---- log capture (DebugLog's external sink) ----
	std::mutex g_logMutex;
	std::vector<std::string> g_log;
	void __cdecl CollectLog(const char* line)
	{
		std::lock_guard<std::mutex> lock(g_logMutex);
		g_log.push_back(line ? line : "");
	}
	struct LogCapture
	{
		LogCapture() { Take(); DebugLog::SetExternalSink(CollectLog); }
		~LogCapture() { DebugLog::SetExternalSink(nullptr); }
		static std::vector<std::string> Take()
		{
			std::lock_guard<std::mutex> lock(g_logMutex);
			std::vector<std::string> out;
			out.swap(g_log);
			return out;
		}
		// Only the lines under test (a background thread of an earlier test may still log).
		static std::vector<std::string> Take(const char* prefix)
		{
			std::vector<std::string> out;
			for (std::string& line : Take())
				if (line.compare(0, std::strlen(prefix), prefix) == 0)
					out.push_back(std::move(line));
			return out;
		}
	};

	// Saves an environment variable (OS and CRT copies) and restores it at scope exit: a GPU run
	// sets VP_TEST_ADAPTER for the whole test process, later tests must still see it.
	struct EnvRestore
	{
		explicit EnvRestore(const char* name) : name(name)
		{
			char value[32768] = {};
			const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
			wasSet = length > 0 && length < sizeof(value);
			if (wasSet)
				saved.assign(value, length);
		}
		~EnvRestore()
		{
			SetEnvironmentVariableA(name, wasSet ? saved.c_str() : nullptr);
			_putenv_s(name, wasSet ? saved.c_str() : "");
		}
		const char* name;
		bool wasSet = false;
		std::string saved;
	};

	// ---- ring comparison (the stamped qpc is the only field that differs between two runs) ----
	std::vector<DH::FrameRecord> Frames(const DH::Recorder& r)
	{
		std::vector<DH::FrameRecord> out;
		for (const auto& e : r.frames.Snapshot())
			out.push_back(e.record);
		return out;
	}
	std::vector<DH::EventRecord> Decisions(const DH::Recorder& r)
	{
		std::vector<DH::EventRecord> out;
		for (const auto& e : r.decisions.Snapshot())
		{
			DH::EventRecord ev = e.record;
			ev.qpc = 0;
			out.push_back(ev);
		}
		return out;
	}
	void AssertSameFrames(const DH::Recorder& a, const DH::Recorder& b, bool ignoreStampedQpc)
	{
		auto fa = Frames(a), fb = Frames(b);
		Assert::AreEqual(fa.size(), fb.size(), L"frame row count");
		for (size_t i = 0; i < fa.size(); ++i)
		{
			if (ignoreStampedQpc)
				fa[i].qpc = fb[i].qpc = 0;
			Assert::IsTrue(std::memcmp(&fa[i], &fb[i], sizeof(DH::FrameRecord)) == 0, L"frame row differs");
		}
	}
	void AssertSameDecisions(const DH::Recorder& a, const DH::Recorder& b)
	{
		const auto da = Decisions(a), db = Decisions(b);
		Assert::AreEqual(da.size(), db.size(), L"decision count");
		for (size_t i = 0; i < da.size(); ++i)
			Assert::IsTrue(std::memcmp(&da[i], &db[i], sizeof(DH::EventRecord)) == 0, L"decision differs");
	}

	// ---- old host code (verbatim copies) ----

	// LibplaceboVideoRenderer.cpp, CaptureRenderedOutput: the "dolby_vision" object's tail.
	void OldDvTail(std::ostringstream& metadata, bool captureDolbyVision, bool sceneMinAvailable,
		float sceneMin, float avgPq)
	{
		metadata << ", \"scene_average_pq\": " << avgPq <<
			", \"scene_min_present\": " <<
			(captureDolbyVision && sceneMinAvailable ? "true" : "false") <<
			", \"scene_min_nits\": " <<
			(captureDolbyVision && sceneMinAvailable ? sceneMin : 0.0f) << "},\n";
	}

	// RecordRendererFrame (anonymous namespace in the host).
	void OldRecordRendererFrame(DH::Recorder& to, uint32_t stage, uint64_t counter, int64_t sourceTimestamp,
		size_t queueDepth, uint32_t reason, uint64_t droppedTotal, uint32_t rendererGeneration,
		uint32_t flags = 0, int64_t qpc = 0) noexcept
	{
		DiagnosticHistory::FrameRecord record;
		record.qpc = qpc;
		record.stage = stage;
		record.counter = counter;
		record.sourceTimestamp = sourceTimestamp;
		record.queueDepth = static_cast<uint32_t>(queueDepth);
		record.holdReason = reason;
		record.droppedTotal = droppedTotal;
		record.rendererGeneration = rendererGeneration;
		record.flags = flags;
		to.RecordFrame(record);
	}

	// RenderLocked: the presented row.
	struct OldPresentedInputs
	{
		int64_t swapEndQpc; uint64_t sourceFrameNumber; int64_t captureTimestampRecord; int64_t captureTimingTimestamp;
		int64_t qpcFrequency; bool presentationTargetTimingKnown; double presentationTargetLeadMs;
		size_t queueDepthAfterDequeue; double renderMs, sourceUploadCpuLastMs, swapBlockMs, submitGapMs;
		int64_t callbackQpc; bool temporalActive, renderAheadActive, openLoop; int renderAheadPendingMainInterval;
		bool vectorsValid, hdr, discontinuity, cadenceRepeat; int width, height; uint64_t holds; uint32_t generation;
	};
	void OldPresented(DH::Recorder& to, const OldPresentedInputs& in)
	{
		DiagnosticHistory::FrameRecord diag;
		diag.qpc = in.swapEndQpc;
		diag.stage = DiagnosticHistory::StagePresented;
		diag.counter = in.sourceFrameNumber;
		diag.sourceTimestamp = in.captureTimestampRecord;
		diag.captureTimestamp = in.captureTimingTimestamp;
		const double qpcPerMs = static_cast<double>(in.qpcFrequency) / 1000.0;
		if (in.presentationTargetTimingKnown && qpcPerMs > 0.0)
		{
			diag.presentQpc = in.swapEndQpc + static_cast<int64_t>(in.presentationTargetLeadMs * qpcPerMs);
			diag.leadMs = static_cast<float>(in.presentationTargetLeadMs);
		}
		diag.queueDepth = static_cast<uint32_t>(in.queueDepthAfterDequeue);
		diag.renderMs = static_cast<float>(in.renderMs);
		diag.uploadMs = static_cast<float>(in.sourceUploadCpuLastMs);
		diag.waitMs = static_cast<float>(in.swapBlockMs);
		diag.intervalMs = static_cast<float>(in.submitGapMs);
		if (qpcPerMs > 0.0 && in.callbackQpc > 0 && in.swapEndQpc > in.callbackQpc)
			diag.latencyMs = static_cast<float>(static_cast<double>(in.swapEndQpc - in.callbackQpc) / qpcPerMs);
		uint32_t flags = 0;
		if (in.temporalActive) flags |= DiagnosticHistory::FlagInterpolated;
		if (in.renderAheadActive)
		{
			flags |= DiagnosticHistory::FlagRenderAhead;
			if (in.openLoop) flags |= DiagnosticHistory::FlagOpenLoop;
			diag.phase = in.renderAheadPendingMainInterval;
		}
		if (in.vectorsValid)
			flags |= DiagnosticHistory::FlagVectorsValid;
		if (in.hdr) flags |= DiagnosticHistory::FlagHdr;
		if (in.discontinuity) flags |= DiagnosticHistory::FlagDiscontinuity;
		diag.flags = flags;
		diag.repeatCount = in.cadenceRepeat ? 1 : 0;
		diag.width = in.width > 0 ? static_cast<uint32_t>(in.width) : 0u;
		diag.height = in.height > 0 ? static_cast<uint32_t>(in.height) : 0u;
		diag.repeatedTotal = in.holds;
		diag.rendererGeneration = in.generation;
		to.RecordFrame(diag);
	}

	VpDiagnostics::PresentedFacts NewFacts(const OldPresentedInputs& in)
	{
		VpDiagnostics::PresentedFacts f;
		f.swapEndQpc = in.swapEndQpc;
		f.counter = in.sourceFrameNumber;
		f.sourceTimestamp = in.captureTimestampRecord;
		f.captureTimestamp = in.captureTimingTimestamp;
		f.qpcFrequency = in.qpcFrequency;
		f.targetTimingKnown = in.presentationTargetTimingKnown;
		f.targetLeadMs = in.presentationTargetLeadMs;
		f.queueDepth = in.queueDepthAfterDequeue;
		f.renderMs = in.renderMs;
		f.uploadMs = in.sourceUploadCpuLastMs;
		f.waitMs = in.swapBlockMs;
		f.intervalMs = in.submitGapMs;
		f.callbackQpc = in.callbackQpc;
		f.interpolated = in.temporalActive;
		f.renderAhead = in.renderAheadActive;
		f.openLoop = in.openLoop;
		f.mainInterval = in.renderAheadPendingMainInterval;
		f.vectorsValid = in.vectorsValid;
		f.hdr = in.hdr;
		f.discontinuity = in.discontinuity;
		f.cadenceRepeat = in.cadenceRepeat;
		f.width = in.width;
		f.height = in.height;
		f.holds = in.holds;
		f.rendererGeneration = in.generation;
		return f;
	}

	// DejudderHold: the row/decision part and the hold lines (FlushDejudderHolds).
	struct OldHoldState
	{
		uint32_t diagLastHoldCode = 0;
		unsigned long long holds = 0;
		ULONGLONG dejudderHoldLogTick = 0;
		unsigned long long dejudderHoldsNotLogged = 0;
		std::vector<std::pair<std::string, unsigned long long>> dejudderHoldsPending;

		void Flush()
		{
			for (const auto& pending : dejudderHoldsPending)
				DebugLog::Log("De-judder: hold (%s; %llu holds since the last line, %llu in total)",
					pending.first.c_str(), pending.second, holds);
			dejudderHoldsPending.clear();
			dejudderHoldsNotLogged = 0;
		}

		void Hold(DH::Recorder& to, const char* why, uint64_t diagExtrasCounter, uint64_t diagExtrasSequence,
			bool renderAheadActive, bool openLoop, uint32_t diagRendererGeneration, ULONGLONG now)
		{
			{
				const uint32_t code = RendererDiagnostics::ClassifyDejudderHold(why);
				DiagnosticHistory::FrameRecord record;
				record.stage = DiagnosticHistory::StageRepeated;
				record.counter = diagExtrasCounter;
				record.holdReason = code;
				record.flags = renderAheadActive ? DiagnosticHistory::FlagRenderAhead : 0u;
				if (renderAheadActive && openLoop)
					record.flags |= DiagnosticHistory::FlagOpenLoop;
				record.rendererGeneration = diagRendererGeneration;
				record.repeatedTotal = holds + 1;
				to.RecordFrame(record);
				if (code != diagLastHoldCode)
				{
					diagLastHoldCode = code;
					to.RecordDecision("dejudder-hold", why, code, static_cast<int64_t>(diagExtrasSequence));
				}
			}
			++holds;
			++dejudderHoldsNotLogged;
			auto pending = std::find_if(dejudderHoldsPending.begin(), dejudderHoldsPending.end(),
				[why](const std::pair<std::string, unsigned long long>& p) { return p.first == why; });
			if (pending == dejudderHoldsPending.end())
				dejudderHoldsPending.emplace_back(why, 1);
			else
				++pending->second;
			if (dejudderHoldLogTick == 0 || now - dejudderHoldLogTick >= 5000)
			{
				Flush();
				dejudderHoldLogTick = now;
			}
		}
	};

	struct NewHoldState
	{
		uint32_t lastHoldCode = 0;
		unsigned long long holds = 0;
		VpDiagnostics::HoldLog log;

		void Hold(DH::Recorder& to, const char* why, uint64_t counter, uint64_t sequence,
			bool renderAhead, bool openLoop, uint32_t generation, ULONGLONG now)
		{
			VpDiagnostics::HoldFacts f;
			f.why = why;
			f.counter = counter;
			f.sequence = sequence;
			f.renderAhead = renderAhead;
			f.openLoop = openLoop;
			f.rendererGeneration = generation;
			f.holdsBefore = holds;
			VpDiagnostics::RecordDejudderHold(f, lastHoldCode, to);
			++holds;
			log.OnHold(why, holds, now);
		}
	};

	// LogEntranceCounters (with the interval and clock injected; text as the S1 entrance segment wrote it).
	void OldEntrance(GeneralRate::LogInterval& entranceLog, unsigned long long& entranceCostLoggedAt,
		const VpDiagnostics::EntranceCounters& c, GeneralRate::LogInterval::Clock::time_point now)
	{
		const bool batch = c.stageRuns >= entranceCostLoggedAt + 120;
		if (entranceLog.DueOr(c.runs == 1 || batch, now))
		{
			entranceCostLoggedAt = c.stageRuns;
			DebugLog::Log("Entrance cost: %llu input-side runs for %llu source frames", c.stageRuns, c.sourceFrames);
			if (c.once)
				DebugLog::Log("Entrance stages: %llu runs, %llu repeats reused (once per source frame); entrance once (B3): "
					"A %llu, B ahead %llu, ring reuses %llu, pictures from processed planes %llu, fallbacks %llu, history breaks %llu",
					c.runs, c.reuses, c.onceA, c.onceB, c.onceReused, c.oncePictures, c.onceFallbacks, c.statefulBreaks);
			else
				DebugLog::Log("Entrance stages: %llu runs, %llu repeats reused (once per source frame)", c.runs, c.reuses);
		}
	}

	// ScreenshotDirectory's / ReadUserShader's / ShaderCacheTestFolder's override reads.
	bool OldWide(const wchar_t* name, std::wstring& out)
	{
		wchar_t overrideDir[1024] = {};
		const DWORD overrideLength = GetEnvironmentVariableW(name, overrideDir, 1024);
		if (overrideLength > 0 && overrideLength < 1024)
		{
			out = std::wstring(overrideDir, overrideLength);
			return true;
		}
		return false;
	}
	bool OldNarrow(const char* name, std::string& out)
	{
		char overrideDir[1024] = {};
		const DWORD overrideLength = GetEnvironmentVariableA(name, overrideDir, 1024);
		if (overrideLength > 0 && overrideLength < 1024)
		{
			out = std::string(overrideDir, overrideLength);
			return true;
		}
		return false;
	}

	struct FakeCapture : VpDiagnostics::RenderedFrameCapture
	{
		explicit FakeCapture(const char* name) : name(name) {}
		const char* Command() const override { return name; }
		void Configure(const std::string& f) override { folder = f; enabled = !f.empty(); }
		bool Enabled() const override { return enabled; }
		void OnFrameRendered(const VpDiagnostics::RenderedFrameContext& frame) override { counters.push_back(frame.counter); }
		std::string Toggle() override { return std::string(name) + ": toggled"; }
		const char* name;
		std::string folder;
		bool enabled = false;
		std::vector<uint64_t> counters;
	};

	std::wstring TempFolder(const wchar_t* leaf)
	{
		wchar_t temp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, temp);
		std::wstring folder = std::wstring(temp) + leaf + L"-" + std::to_wstring(GetCurrentProcessId());
		CreateDirectoryW(folder.c_str(), nullptr);
		return folder;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(DiagnosticsAdapterTests)
	{
	public:
		// The rendered-output capture sidecar: VP's scene-minimum fields, byte for byte, in the
		// sidecar stream's own state (classic locale, precision 10, after a hex/fill section).
		TEST_METHOD(SidecarDvSceneMinFieldsAreByteIdentical)
		{
			const float values[] = { 0.0f, 0.0001f, 0.005f, 0.1234567f, 1.0f, 48.25f, 1000.0f, 1234.5678f };
			for (const bool dv : { false, true })
				for (const bool available : { false, true })
					for (const float nits : values)
					{
						std::ostringstream oldStream, newStream;
						for (std::ostringstream* s : { &oldStream, &newStream })
						{
							s->imbue(std::locale::classic());
							*s << std::setprecision(10) << "x" << std::hex << std::setw(16) << std::setfill('0')
								<< 0xabcull << std::dec;
						}
						OldDvTail(oldStream, dv, available, nits, 0.4321f);
						newStream << ", \"scene_average_pq\": " << 0.4321f <<
							VpDiagnostics::DvSceneMinFields{ dv, available, nits } << "},\n";
						Assert::AreEqual(oldStream.str(), newStream.str());
					}
		}

		TEST_METHOD(TestFolderOverridesReadTheSameVariables)
		{
			Assert::AreEqual("VP_TEST_SCREENSHOT_DIR", VpDiagnostics::TestFolderVariable(VpDiagnostics::TestFolder::Screenshots));
			Assert::AreEqual("VP_TEST_SHADER_DIR", VpDiagnostics::TestFolderVariable(VpDiagnostics::TestFolder::Shaders));
			Assert::AreEqual("VP_TEST_SHADER_CACHE_DIR", VpDiagnostics::TestFolderVariable(VpDiagnostics::TestFolder::ShaderCache));
			const EnvRestore restoreScreenshots("VP_TEST_SCREENSHOT_DIR");
			const EnvRestore restoreShaders("VP_TEST_SHADER_DIR");
			const EnvRestore restoreCache("VP_TEST_SHADER_CACHE_DIR");
			const std::wstring longValue(1100, L'x');
			for (const wchar_t* value : { L"", L"C:\\tmp\\vp-test", longValue.c_str() })
			{
				SetEnvironmentVariableW(L"VP_TEST_SCREENSHOT_DIR", value[0] ? value : nullptr);
				SetEnvironmentVariableA("VP_TEST_SHADER_DIR", value[0] ? std::string(CStringA(value).GetString()).c_str() : nullptr);
				SetEnvironmentVariableA("VP_TEST_SHADER_CACHE_DIR", value[0] ? std::string(CStringA(value).GetString()).c_str() : nullptr);
				std::wstring oldW, newW;
				Assert::AreEqual(OldWide(L"VP_TEST_SCREENSHOT_DIR", oldW),
					VpDiagnostics::TestFolderOverride(VpDiagnostics::TestFolder::Screenshots, newW));
				Assert::IsTrue(oldW == newW);
				for (const auto folder : { VpDiagnostics::TestFolder::Shaders, VpDiagnostics::TestFolder::ShaderCache })
				{
					std::string oldA, newA;
					Assert::AreEqual(OldNarrow(VpDiagnostics::TestFolderVariable(folder), oldA),
						VpDiagnostics::TestFolderOverride(folder, newA));
					Assert::AreEqual(oldA, newA);
				}
			}
		}

		TEST_METHOD(TestAdapterUnsetLeavesLibplacebosChoice)
		{
			const EnvRestore restore("VP_TEST_ADAPTER");
			SetEnvironmentVariableA("VP_TEST_ADAPTER", nullptr);
			_putenv_s("VP_TEST_ADAPTER", "");
			IDXGIAdapter1* adapter = reinterpret_cast<IDXGIAdapter1*>(1);
			VpDiagnostics::TestAdapterFromEnvironment(&adapter);
			Assert::IsTrue(adapter == nullptr);
			_putenv_s("VP_TEST_ADAPTER", "no GPU has this name 7f3a");
			VpDiagnostics::TestAdapterFromEnvironment(&adapter);
			Assert::IsTrue(adapter == nullptr);
		}

		// The match path (DXGI enumeration only, no device): the first adapter's own name selects
		// it, the choice is logged, and the caller owns one reference.
		TEST_METHOD(TestAdapterMatchSelectsAndLogs)
		{
			IDXGIFactory1* factory = nullptr;
			if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
				return;
			IDXGIAdapter1* first = nullptr;
			DXGI_ADAPTER_DESC1 desc{};
			const bool any = factory->EnumAdapters1(0, &first) != DXGI_ERROR_NOT_FOUND && first &&
				SUCCEEDED(first->GetDesc1(&desc));
			if (first)
				first->Release();
			factory->Release();
			if (!any || !desc.Description[0])
				return;   // no adapter on this machine: nothing to match
			const EnvRestore restore("VP_TEST_ADAPTER");
			_putenv_s("VP_TEST_ADAPTER", std::string(CStringA(desc.Description).GetString()).c_str());
			LogCapture capture;
			IDXGIAdapter1* adapter = nullptr;
			VpDiagnostics::TestAdapterFromEnvironment(&adapter);
			Assert::IsTrue(adapter != nullptr);
			DXGI_ADAPTER_DESC1 chosen{};
			Assert::IsTrue(SUCCEEDED(adapter->GetDesc1(&chosen)));
			Assert::IsTrue(std::wstring(chosen.Description).find(desc.Description) != std::wstring::npos);
			adapter->Release();   // the caller's reference
			const auto lines = LogCapture::Take("libplacebo: VP_TEST_ADAPTER selects ");
			Assert::AreEqual(size_t(1), lines.size());
		}

		TEST_METHOD(PresentedRowsAreByteIdentical)
		{
			auto oldRec = NewRecorder();
			auto newRec = NewRecorder();
			OldPresentedInputs cases[] = {
				{ 1000000, 42, 777, 888, 10000000, true, 12.5, 3, 4.25, 0.5, 1.75, 16.68, 900000,
					true, true, true, 2, true, true, false, true, 3840, 2160, 9, 3 },
				{ 2000000, 43, 778, 889, 10000000, false, 0.0, 0, 0.0, 0.0, 0.0, 0.0, 0,
					false, false, true, 3, false, false, true, false, 0, -1, 0, 4 },
				{ 3000000, 44, 0, 0, 0, true, 5.0, 7, 1.0, 2.0, 3.0, 4.0, 3500000,
					false, true, false, 1, true, false, false, false, 1920, 1080, 1, 5 },
			};
			for (const auto& c : cases)
			{
				OldPresented(*oldRec, c);
				VpDiagnostics::RecordPresented(NewFacts(c), *newRec);
			}
			AssertSameFrames(*oldRec, *newRec, false);
		}

		TEST_METHOD(RendererAndCadenceRowsAreByteIdentical)
		{
			auto oldRec = NewRecorder();
			auto newRec = NewRecorder();
			OldRecordRendererFrame(*oldRec, DH::StageDropped, 5, 50, 4, 7, 12, 2, 0, 1234);
			VpDiagnostics::RecordRendererFrame(DH::StageDropped, 5, 50, 4, 7, 12, 2, 0, 1234, *newRec);
			OldRecordRendererFrame(*oldRec, DH::StageRendererIngest, 6, 60, 1, 0, 0, 2, DH::FlagDiscontinuity, 5678);
			VpDiagnostics::RecordRendererFrame(DH::StageRendererIngest, 6, 60, 1, 0, 0, 2, DH::FlagDiscontinuity, 5678, *newRec);
			// RenderLoop: a drift-correction repeat queued.
			{
				DiagnosticHistory::FrameRecord diag;
				diag.stage = DiagnosticHistory::StageRepeated;
				diag.counter = 9;
				diag.sourceTimestamp = 99;
				diag.queueDepth = static_cast<uint32_t>(size_t(3));
				diag.holdReason = RendererDiagnostics::RepeatCadenceCorrection;
				diag.repeatedTotal = 11;
				diag.rendererGeneration = 4;
				diag.qpc = 4321;   // fixed for the comparison (both stamp NowQpc otherwise)
				oldRec->RecordFrame(diag);
			}
			{
				auto tmp = NewRecorder();
				VpDiagnostics::RecordCadenceRepeat(9, 99, 3, 11, 4, *tmp);
				DH::FrameRecord row = Frames(*tmp).at(0);
				row.qpc = 4321;
				newRec->RecordFrame(row);
			}
			AssertSameFrames(*oldRec, *newRec, false);
		}

		// De-judder holds: rows, decisions and the "De-judder: hold" lines, over reason changes,
		// repeats and the 5 s line interval, then the teardown flush.
		TEST_METHOD(HoldRowsDecisionsAndLinesAreByteIdentical)
		{
			LogCapture capture;
			auto oldRec = NewRecorder();
			auto newRec = NewRecorder();
			OldHoldState oldState;
			NewHoldState newState;
			struct Step { const char* why; ULONGLONG now; bool ra; bool open; };
			const Step steps[] = {
				{ "no motion estimate", 1000, false, false },
				{ "no motion estimate", 1500, false, false },
				{ "cadence repeat", 2000, true, false },
				{ "cadence repeat", 6100, true, true },
				{ "render-ahead gate at hold", 6200, true, true },
				{ "no motion estimate", 11200, false, false },
				{ "unclassified text", 11300, true, false },
			};
			uint64_t counter = 100;
			for (const Step& s : steps)
			{
				oldState.Hold(*oldRec, s.why, counter, counter + 7, s.ra, s.open, 3, s.now);
				newState.Hold(*newRec, s.why, counter, counter + 7, s.ra, s.open, 3, s.now);
				++counter;
			}
			AssertSameFrames(*oldRec, *newRec, true);
			AssertSameDecisions(*oldRec, *newRec);

			// The lines: run each implementation alone and compare what it logged.
			LogCapture::Take();
			OldHoldState oldOnly;
			NewHoldState newOnly;
			auto scratch = NewRecorder();
			for (const Step& s : steps)
				oldOnly.Hold(*scratch, s.why, 1, 1, s.ra, s.open, 1, s.now);
			oldOnly.Flush();
			const auto oldLines = LogCapture::Take("De-judder: hold");
			for (const Step& s : steps)
				newOnly.Hold(*scratch, s.why, 1, 1, s.ra, s.open, 1, s.now);
			newOnly.log.Flush(newOnly.holds);
			const auto newLines = LogCapture::Take("De-judder: hold");
			Assert::IsTrue(oldLines.size() >= 4, L"the scenario logs lines");
			Assert::IsTrue(oldLines == newLines, L"hold lines differ");
		}

		TEST_METHOD(EntranceCounterLinesAreByteIdentical)
		{
			LogCapture capture;
			using Clock = GeneralRate::LogInterval::Clock;
			const Clock::time_point t0 = Clock::now();
			GeneralRate::LogInterval oldInterval = GeneralRate::LogInterval::ForFrames(2400);
			GeneralRate::LogInterval newInterval = GeneralRate::LogInterval::ForFrames(2400);
			unsigned long long oldAt = 0, newAt = 0;
			struct Step { VpDiagnostics::EntranceCounters c; int ms; };
			const Step steps[] = {
				{ { 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, false }, 0 },
				{ { 60, 30, 30, 30, 0, 0, 0, 0, 0, 0, false }, 500 },
				{ { 121, 61, 61, 60, 0, 0, 0, 0, 0, 0, false }, 900 },
				{ { 200, 100, 100, 100, 50, 40, 30, 20, 1, 0, true }, 1200 },
				{ { 260, 130, 130, 130, 60, 50, 40, 30, 2, 1, true }, 200000 },
				{ { 400, 200, 200, 200, 90, 80, 70, 60, 3, 2, true }, 200100 },
			};
			LogCapture::Take();
			for (const Step& s : steps)
				OldEntrance(oldInterval, oldAt, s.c, t0 + std::chrono::milliseconds(s.ms));
			const auto oldLines = LogCapture::Take("Entrance ");
			for (const Step& s : steps)
				VpDiagnostics::LogEntranceCounters(s.c, newAt, newInterval, t0 + std::chrono::milliseconds(s.ms));
			const auto newLines = LogCapture::Take("Entrance ");
			Assert::IsTrue(oldLines.size() >= 6, L"the scenario logs lines");
			Assert::IsTrue(oldLines == newLines, L"entrance lines differ");
			Assert::AreEqual(oldAt, newAt);
		}

		TEST_METHOD(RenderedFrameCapturesRouteCommandsAndFrames)
		{
			VpDiagnostics::RenderedFrameCaptures captures;
			std::string reply;
			Assert::IsFalse(captures.Invoke("dv_compare_capture", reply));
			Assert::IsFalse(captures.AnyEnabled());
			auto owned = std::make_unique<FakeCapture>("dv_compare_capture");
			FakeCapture* fake = owned.get();
			captures.Register(std::move(owned));
			captures.Register(nullptr);
			Assert::AreEqual(size_t(1), captures.Count());

			VpDiagnostics::RenderedFrameContext frame;
			frame.counter = 1;
			captures.OnFrameRendered(frame);
			Assert::IsTrue(fake->counters.empty(), L"a disabled capture sees no frame");

			captures.Configure("dv_compare_capture", "D:\\clips");
			Assert::AreEqual(std::string("D:\\clips"), fake->folder);
			Assert::IsTrue(captures.AnyEnabled());
			frame.counter = 2;
			captures.OnFrameRendered(frame);
			Assert::AreEqual(size_t(1), fake->counters.size());
			Assert::AreEqual(uint64_t(2), fake->counters[0]);

			Assert::IsTrue(captures.Invoke("dv_compare_capture", reply));
			Assert::AreEqual(std::string("dv_compare_capture: toggled"), reply);
			Assert::IsFalse(captures.Invoke("raw_capture", reply));
			Assert::IsFalse(captures.Invoke(nullptr, reply));
		}

		// Report-a-problem run: the dialog's gates and result handling.
		TEST_METHOD(ReportRunGatesAndResults)
		{
			LogCapture capture;
			VpDiagnostics::ReportRun run;
			Assert::IsTrue(run.TryBegin(1000));
			Assert::IsFalse(run.TryBegin(2000), L"already saving");
			Assert::IsFalse(run.TryBegin(1000 + 120000), L"exactly 2 min: still busy");
			Assert::IsTrue(run.TryBegin(1000 + 120001), L"a lost completion frees the flag after 2 min");
			Assert::IsTrue(run.Busy());

			// Busy: an automatic report spends no slot.
			LogCapture::Take();
			Assert::IsFalse(run.AllowAutomatic(true, "liveness-recovery", 500000));
			Assert::AreEqual(0, run.AutomaticCount());
			auto lines = LogCapture::Take("Automatic problem report");
			Assert::AreEqual(size_t(1), lines.size());
			Assert::AreEqual(std::string("Automatic problem report skipped (a report is being saved): reason=liveness-recovery saved_this_run=0"), lines[0]);

			Assert::AreEqual(std::string(), run.Finish(0));
			Assert::IsFalse(run.Busy());
			Assert::IsFalse(run.AllowAutomatic(false, "reset-failed", 600000), L"auto_report off");
			Assert::IsTrue(run.AllowAutomatic(true, "reset-failed", 600000));
			Assert::AreEqual(1, run.AutomaticCount());
			LogCapture::Take();
			Assert::IsFalse(run.AllowAutomatic(true, "reset-failed", 600001));
			lines = LogCapture::Take("Automatic problem report");
			Assert::AreEqual(std::string("Automatic problem report skipped (rate limit): reason=reset-failed saved_this_run=1"), lines.at(0));

			Assert::IsTrue(run.TryBegin(700000));
			auto* ok = new StateReport::Result();
			ok->ok = true;
			ok->fileName = "vp-report-1.zip";
			ok->bytes = 1234;
			ok->entries = { "README.md", "manifest.json" };
			LogCapture::Take();
			Assert::AreEqual(std::string("saved vp-report-1.zip"), run.Finish(reinterpret_cast<LPARAM>(ok)));
			lines = LogCapture::Take("Problem report saved");
			Assert::AreEqual(std::string("Problem report saved: logs\\reports\\vp-report-1.zip bytes=1234 files=2 removed_old=0"), lines.at(0));
			Assert::IsFalse(run.Busy());

			auto* failed = new StateReport::Result();
			failed->error = "disk full";
			Assert::AreEqual(std::string("failed (disk full)"), run.Finish(reinterpret_cast<LPARAM>(failed)));
			Assert::IsTrue(run.WaitForExit(GetTickCount64(), 0), L"no worker: nothing to wait for");
		}

		// The shared report inputs that moved out of SaveStateReport (verbatim copy of the old
		// block, no renderer), then a whole run: worker, StateReport::Write, exit wait. The
		// report goes to %TEMP%, never beside the test host.
		TEST_METHOD(ReportRunInputsMatchTheOldDialogCodeAndARunWritesABundle)
		{
			// Old SaveStateReport (ac49a1de), the parts NewInputs took over, without a renderer.
			StateReport::Inputs old;
			old.trigger = "test";
			old.vpRoot = VpDiagnostics::ExecutableFolder();
			old.reportsDirectory = old.vpRoot + L"\\logs\\reports";
			old.binaries.push_back({ "VideoProcessor.exe", old.vpRoot + L"\\VideoProcessor.exe" });
			for (const bool beta : { false, true })
			{
				const std::wstring plugin = OptionalRendererLayout::PluginPath(old.vpRoot, beta);
				if (GetFileAttributesW(plugin.c_str()) == INVALID_FILE_ATTRIBUTES)
					continue;
				std::wstring relative = plugin.size() > old.vpRoot.size() + 1 ?
					plugin.substr(old.vpRoot.size() + 1) : plugin;
				std::replace(relative.begin(), relative.end(), L'\\', L'/');
				old.binaries.push_back({ std::string(CStringA(relative.c_str()).GetString()), plugin });
			}
			{
				std::string live = CrashState::Global().LivePath();
				const size_t separator = live.find_last_of("\\/");
				if (separator != std::string::npos)
					old.crashStateDirectory = VpDiagnostics::WidenAnsi(live.substr(0, separator));
			}
			old.configFiles = VpDiagnostics::ReportConfigFiles();

			VpDiagnostics::ReportRun run;
			Assert::IsTrue(run.TryBegin(GetTickCount64()));
			auto inputs = run.NewInputs("test", nullptr);
			Assert::AreEqual(old.trigger, inputs->trigger);
			Assert::IsTrue(old.vpRoot == inputs->vpRoot);
			Assert::IsTrue(old.reportsDirectory == inputs->reportsDirectory);
			Assert::AreEqual(old.binaries.size(), inputs->binaries.size());
			for (size_t i = 0; i < old.binaries.size(); ++i)
			{
				Assert::AreEqual(old.binaries[i].name, inputs->binaries[i].name);
				Assert::IsTrue(old.binaries[i].path == inputs->binaries[i].path);
			}
			Assert::IsFalse(inputs->rendererContributed);
			Assert::IsTrue(inputs->historyRenderer.empty());
			Assert::IsTrue(inputs->historyExe.find("\"src\":\"exe\"") != std::string::npos, L"exe rings serialised");
			Assert::IsTrue(inputs->historyExe.find("state-report") != std::string::npos, L"the request is a decision row");
			Assert::IsTrue(old.crashStateDirectory == inputs->crashStateDirectory);
			Assert::IsTrue(old.configFiles == inputs->configFiles);

			run.RequestThumbnail(*inputs, false, nullptr);
			Assert::IsFalse(inputs->thumbnailEnabled);
			Assert::IsTrue(inputs->thumbnailNote.empty());
			VpDiagnostics::ReportRun probe;
			StateReport::Inputs probeInputs;
			probe.RequestThumbnail(probeInputs, true, nullptr);
			Assert::AreEqual(std::string("no active VP Renderer"), probeInputs.thumbnailNote);

			const std::wstring folder = TempFolder(L"vp-wp17-reports");
			inputs->reportsDirectory = folder;
			const size_t before = StateReport::ListReports(folder).size();
			Assert::IsTrue(run.Start(inputs, nullptr, 0));   // no window: the result is dropped
			Assert::IsTrue(run.WaitForExit(GetTickCount64() + 60000, 60000), L"the worker finishes");
			const auto reports = StateReport::ListReports(folder);
			Assert::AreEqual(before + 1, reports.size(), L"one bundle written");
			for (const std::wstring& report : reports)
				DeleteFileW((folder + L"\\" + report).c_str());
			RemoveDirectoryW(folder.c_str());
			run.End();
		}

		// The DV comparison capture through the adapter (feat/dv-l8 wiring): registered under its
		// command, off without a folder, and its toggle reply is the recorder's own.
		TEST_METHOD(DvCompareCaptureIsTheRenderersRenderedFrameCapture)
		{
			const EnvRestore restore("VP_DV_COMPARE_PATH");
			SetEnvironmentVariableA("VP_DV_COMPARE_PATH", nullptr);
			_putenv_s("VP_DV_COMPARE_PATH", "");
			VpDiagnostics::RenderedFrameCaptures captures = DvCompareCapture::RendererCaptures();
			Assert::AreEqual(size_t(1), captures.Count());
			Assert::IsFalse(captures.AnyEnabled(), L"off without VP_DV_COMPARE_PATH or a Config folder");
			std::string reply;
			Assert::IsTrue(captures.Invoke(DvCompareCapture::kCommand, reply));
			DvCompareCapture::Recorder direct{ DvCompareCapture::Options::FromEnvironment() };
			Assert::AreEqual(direct.Toggle(), reply);
			VpDiagnostics::RenderedFrameContext frame;
			Assert::IsFalse(captures.OnFrameRendered(frame), L"a disabled capture takes no frame");
		}

		TEST_METHOD(NewestScreenshotSinceFindsTheThumbnailSource)
		{
			const std::wstring folder = TempFolder(L"vp-wp17-shots");
			FILETIME before = {};
			GetSystemTimeAsFileTime(&before);
			Assert::IsTrue(VpDiagnostics::NewestScreenshotSince(folder, before).empty());
			const std::wstring png = folder + L"\\a.png";
			const std::wstring txt = folder + L"\\b.txt";
			for (const std::wstring& path : { png, txt })
			{
				HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				Assert::IsTrue(h != INVALID_HANDLE_VALUE);
				FILETIME later = before;
				reinterpret_cast<ULARGE_INTEGER*>(&later)->QuadPart += 10000000ull;
				SetFileTime(h, nullptr, nullptr, &later);
				CloseHandle(h);
			}
			Assert::IsTrue(VpDiagnostics::NewestScreenshotSince(folder, before) == png);
			FILETIME after = before;
			reinterpret_cast<ULARGE_INTEGER*>(&after)->QuadPart += 20000000ull;
			Assert::IsTrue(VpDiagnostics::NewestScreenshotSince(folder, after).empty());
			DeleteFileW(png.c_str());
			DeleteFileW(txt.c_str());
			RemoveDirectoryW(folder.c_str());
		}
	};
}
