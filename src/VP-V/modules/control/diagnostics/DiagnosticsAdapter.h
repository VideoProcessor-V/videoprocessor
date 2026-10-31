#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// VP diagnostics adapter (WP-17, integration batch 1): VP's diagnostic captures, frame rows,
// trace lines and test switches behind ONE adapter, so the host (LibplaceboVideoRenderer.cpp,
// VideoProcessorDlg.cpp: Bill's files) keeps one call per hook point. The GUI half (the
// report-a-problem run) is DiagnosticsReportRun.h; both are namespace VpDiagnostics.
//
// Hook points (docs/design/wp17-callsites.md lists every host line):
//   1. env-gated test/debug switches   TestFolderOverride, TestAdapterFromEnvironment, D3D11DebugFromEnvironment
//   2. capture requested               DvSceneMinFields (rendered-output capture sidecar),
//                                      RenderedFrameCaptures::Invoke (a capture command)
//   3. frame rendered                  RecordPresented, RecordDejudderHold, RecordCadenceRepeat,
//                                      RecordRendererFrame, RenderedFrameCaptures::OnFrameRendered,
//                                      HoldLog, LogEntranceCounters
//   4. state report                    WriteRendererHistory (renderer rings -> report sink)
//
// Byte identity: every row, log line and sidecar fragment is what the host wrote before the
// extraction (DiagnosticsAdapterTests compares them with verbatim copies of the old code).
// Header-only, C++14: the renderer DLL and the executable each compile it (DiagnosticHistory's
// Global() is one recorder per module, as before).
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/control/diagnostics/RendererDiagnosticCodes.h>
#include <DebugLog.h>
#include <GeneralRate.h>

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

class VideoState;
class VideoFrame;
struct DolbyVisionTunnelFrameMetadata;
namespace DvTrimSelection { struct Selection; }

namespace VpDiagnostics
{
	// ---- 1. Env-gated test switches (tests only; unset = the normal path) ---------------------

	enum class TestFolder
	{
		Screenshots,   // VP_TEST_SCREENSHOT_DIR: rendered-output captures (and the report's thumbnail)
		Shaders,       // VP_TEST_SHADER_DIR: the folder holding shaders\ (test runner elsewhere)
		ShaderCache,   // VP_TEST_SHADER_CACHE_DIR: a persistent shader cache for the capture harness
	};

	inline const char* TestFolderVariable(TestFolder folder)
	{
		switch (folder)
		{
		case TestFolder::Screenshots: return "VP_TEST_SCREENSHOT_DIR";
		case TestFolder::Shaders: return "VP_TEST_SHADER_DIR";
		case TestFolder::ShaderCache: return "VP_TEST_SHADER_CACHE_DIR";
		}
		return "";
	}

	// The process environment (a test sets it in-process; the CRT's copy may be the DLL's own).
	// False when unset, empty or 1024 characters or longer: the caller keeps its normal path.
	inline bool TestFolderOverride(TestFolder folder, std::wstring& path)
	{
		const wchar_t* name = folder == TestFolder::Screenshots ? L"VP_TEST_SCREENSHOT_DIR" :
			folder == TestFolder::Shaders ? L"VP_TEST_SHADER_DIR" : L"VP_TEST_SHADER_CACHE_DIR";
		wchar_t value[1024] = {};
		const DWORD length = GetEnvironmentVariableW(name, value, 1024);
		if (length == 0 || length >= 1024)
			return false;
		path.assign(value, length);
		return true;
	}

	inline bool TestFolderOverride(TestFolder folder, std::string& path)
	{
		char value[1024] = {};
		const DWORD length = GetEnvironmentVariableA(TestFolderVariable(folder), value, 1024);
		if (length == 0 || length >= 1024)
			return false;
		path.assign(value, length);
		return true;
	}

	// VP_D3D11_DEBUG=1 asks for the D3D11 debug layer on libplacebo's device (needs the Windows
	// Graphics Tools feature). Only the exact text "1" enables it; unset leaves device creation as is.
	inline bool D3D11DebugRequested(const char* value)
	{
		return value != nullptr && std::strcmp(value, "1") == 0;
	}

	inline bool D3D11DebugFromEnvironment()
	{
		char* v = nullptr;
		size_t n = 0;
		const bool on = _dupenv_s(&v, &n, "VP_D3D11_DEBUG") == 0 && D3D11DebugRequested(v);
		free(v);
		return on;
	}

	// VP_TEST_ADAPTER=<text> picks the GPU whose description contains it (e.g. Intel, the laptop
	// screen's own GPU, for real display timing). *adapter receives a referenced adapter, or stays
	// null (unset, or no match): libplacebo's choice, as always.
	inline void TestAdapterFromEnvironment(IDXGIAdapter1** adapter)
	{
		*adapter = nullptr;
		char* v = nullptr;
		size_t n = 0;
		if (_dupenv_s(&v, &n, "VP_TEST_ADAPTER") == 0 && v && v[0])
		{
			const std::wstring want(v, v + std::strlen(v));
			IDXGIFactory1* factory = nullptr;
			if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
			{
				for (UINT i = 0; !*adapter; ++i)
				{
					IDXGIAdapter1* candidate = nullptr;
					if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND)
						break;
					DXGI_ADAPTER_DESC1 desc{};
					if (SUCCEEDED(candidate->GetDesc1(&desc)) && std::wstring(desc.Description).find(want) != std::wstring::npos)
					{
						*adapter = candidate;
						candidate = nullptr;
						DebugLog::Log("libplacebo: VP_TEST_ADAPTER selects %ls", desc.Description);
					}
					if (candidate)
						candidate->Release();
				}
				factory->Release();
			}
		}
		std::free(v);
	}

	// ---- 2. Capture requested -------------------------------------------------------------------

	// VP's fields inside the rendered-output capture sidecar's "dolby_vision" object, after
	// "scene_average_pq": the DV L1 scene minimum (VP's DM status). Streamed into the host's
	// sidecar stream, so its locale and precision apply exactly as before.
	struct DvSceneMinFields
	{
		bool dolbyVision;
		bool sceneMinAvailable;
		float sceneMinNits;
	};

	inline std::ostream& operator<<(std::ostream& json, const DvSceneMinFields& f)
	{
		const bool present = f.dolbyVision && f.sceneMinAvailable;
		return json << ", \"scene_min_present\": " << (present ? "true" : "false") <<
			", \"scene_min_nits\": " << (present ? f.sceneMinNits : 0.0f);
	}

	// What the renderer knows about the picture it just rendered: the input of every capture
	// that rides on rendered frames. Pointers may be null (not in this render path).
	struct RenderedFrameContext
	{
		const VideoState* state = nullptr;
		const VideoFrame* rawFrame = nullptr;                        // the raw capture frame, if held
		uint64_t counter = 0;                                       // capture frame counter
		const DolbyVisionTunnelFrameMetadata* dvMetadata = nullptr;  // decoded DM (DV tunnel only)
		bool dvTrimsEnabled = false;
		const DvTrimSelection::Selection* dvTrims = nullptr;          // null: not evaluated
		double targetNits = 0.0;                                    // tone-mapping target
	};

	// A capture that records rendered frames (DV comparison capture after feat/dv-l8 merges:
	// a small wrapper around DvCompareCapture::Recorder in input/signal). Off unless enabled.
	class RenderedFrameCapture
	{
	public:
		virtual ~RenderedFrameCapture() = default;
		// The command name (shortcut / module command), e.g. "dv_compare_capture".
		virtual const char* Command() const = 0;
		// The folder from Config ("" = its environment variable, or off). Cheap when unchanged.
		virtual void Configure(const std::string& folder) { (void)folder; }
		virtual bool Enabled() const = 0;
		virtual void OnFrameRendered(const RenderedFrameContext& frame) = 0;
		// Start or stop a clip; returns the line to show on screen.
		virtual std::string Toggle() = 0;
	};

	// The renderer's captures: one member in the host, one hook per point.
	// Threading: Register only while the renderer is constructed (before the render thread
	// starts); afterwards the list is read-only, so the render thread (OnFrameRendered) and the
	// GUI/plugin thread (Invoke, Configure) need no lock here. Each capture locks its own state.
	class RenderedFrameCaptures
	{
	public:
		void Register(std::unique_ptr<RenderedFrameCapture> capture)
		{
			if (capture)
				captures.push_back(std::move(capture));
		}

		void Configure(const char* command, const std::string& folder)
		{
			if (RenderedFrameCapture* c = Find(command))
				c->Configure(folder);
		}

		bool AnyEnabled() const
		{
			for (const auto& c : captures)
				if (c->Enabled())
					return true;
			return false;
		}

		// Frame rendered (render thread): only enabled captures see the frame. Returns whether any
		// did. A host whose context has side effects (dv-l8 consumes its trim selection) builds it
		// only when AnyEnabled() is true.
		bool OnFrameRendered(const RenderedFrameContext& frame)
		{
			bool taken = false;
			for (const auto& c : captures)
				if (c->Enabled())
				{
					c->OnFrameRendered(frame);
					taken = true;
				}
			return taken;
		}

		// Capture requested: false when no capture has this command.
		bool Invoke(const char* command, std::string& reply)
		{
			RenderedFrameCapture* c = Find(command);
			if (!c)
				return false;
			reply = c->Toggle();
			return true;
		}

		size_t Count() const { return captures.size(); }

	private:
		RenderedFrameCapture* Find(const char* command) const
		{
			if (!command)
				return nullptr;
			for (const auto& c : captures)
				if (std::strcmp(c->Command(), command) == 0)
					return c.get();
			return nullptr;
		}

		std::vector<std::unique_ptr<RenderedFrameCapture>> captures;
	};

	// ---- 3. Frame rendered: report-a-problem frame rows (lock-free, no allocation) -------------

	// A renderer frame row outside RenderLocked (ingest, drops).
	inline void RecordRendererFrame(uint32_t stage, uint64_t counter, int64_t sourceTimestamp,
		size_t queueDepth, uint32_t reason, uint64_t droppedTotal, uint32_t rendererGeneration,
		uint32_t flags = 0, int64_t qpc = 0,
		DiagnosticHistory::Recorder& to = DiagnosticHistory::Global()) noexcept
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

	// The presented row of a frame, from values already at hand.
	struct PresentedFacts
	{
		int64_t swapEndQpc = 0;
		uint64_t counter = 0;               // the record's source frame number
		int64_t sourceTimestamp = 0;        // the record's capture timestamp
		int64_t captureTimestamp = 0;       // the frame's capture timing timestamp
		int64_t qpcFrequency = 0;
		bool targetTimingKnown = false;
		double targetLeadMs = 0.0;
		size_t queueDepth = 0;
		double renderMs = 0.0;
		double uploadMs = 0.0;
		double waitMs = 0.0;
		double intervalMs = 0.0;
		int64_t callbackQpc = 0;
		bool interpolated = false;
		bool renderAhead = false;
		bool openLoop = false;
		int mainInterval = 0;               // render-ahead: the main present's SyncInterval
		bool vectorsValid = false;
		bool hdr = false;
		bool discontinuity = false;
		bool cadenceRepeat = false;
		int width = 0;
		int height = 0;
		uint64_t holds = 0;                 // de-judder holds so far
		uint32_t rendererGeneration = 0;
	};

	inline void RecordPresented(const PresentedFacts& f,
		DiagnosticHistory::Recorder& to = DiagnosticHistory::Global()) noexcept
	{
		DiagnosticHistory::FrameRecord diag;
		diag.qpc = f.swapEndQpc;
		diag.stage = DiagnosticHistory::StagePresented;
		diag.counter = f.counter;
		diag.sourceTimestamp = f.sourceTimestamp;
		diag.captureTimestamp = f.captureTimestamp;
		const double qpcPerMs = static_cast<double>(f.qpcFrequency) / 1000.0;
		if (f.targetTimingKnown && qpcPerMs > 0.0)
		{
			diag.presentQpc = f.swapEndQpc + static_cast<int64_t>(f.targetLeadMs * qpcPerMs);
			diag.leadMs = static_cast<float>(f.targetLeadMs);
		}
		diag.queueDepth = static_cast<uint32_t>(f.queueDepth);
		diag.renderMs = static_cast<float>(f.renderMs);
		diag.uploadMs = static_cast<float>(f.uploadMs);
		diag.waitMs = static_cast<float>(f.waitMs);
		diag.intervalMs = static_cast<float>(f.intervalMs);
		if (qpcPerMs > 0.0 && f.callbackQpc > 0 && f.swapEndQpc > f.callbackQpc)
			diag.latencyMs = static_cast<float>(static_cast<double>(f.swapEndQpc - f.callbackQpc) / qpcPerMs);
		uint32_t flags = 0;
		if (f.interpolated) flags |= DiagnosticHistory::FlagInterpolated;
		if (f.renderAhead)
		{
			flags |= DiagnosticHistory::FlagRenderAhead;
			if (f.openLoop) flags |= DiagnosticHistory::FlagOpenLoop;
			diag.phase = f.mainInterval;
		}
		if (f.vectorsValid) flags |= DiagnosticHistory::FlagVectorsValid;
		if (f.hdr) flags |= DiagnosticHistory::FlagHdr;
		if (f.discontinuity) flags |= DiagnosticHistory::FlagDiscontinuity;
		diag.flags = flags;
		diag.repeatCount = f.cadenceRepeat ? 1 : 0;
		diag.width = f.width > 0 ? static_cast<uint32_t>(f.width) : 0u;
		diag.height = f.height > 0 ? static_cast<uint32_t>(f.height) : 0u;
		diag.repeatedTotal = f.holds;
		diag.rendererGeneration = f.rendererGeneration;
		to.RecordFrame(diag);
	}

	// One repeated-stage row per de-judder hold (code: RendererDiagnosticCodes.h) and a
	// decision when the hold reason changes. lastHoldCode is the host's per-renderer state.
	struct HoldFacts
	{
		const char* why = "";
		uint64_t counter = 0;               // the frame whose extras run
		uint64_t sequence = 0;              // its accepted source sequence
		bool renderAhead = false;
		bool openLoop = false;
		uint32_t rendererGeneration = 0;
		uint64_t holdsBefore = 0;           // holds before this one
	};

	inline void RecordDejudderHold(const HoldFacts& f, uint32_t& lastHoldCode,
		DiagnosticHistory::Recorder& to = DiagnosticHistory::Global()) noexcept
	{
		const uint32_t code = RendererDiagnostics::ClassifyDejudderHold(f.why);
		DiagnosticHistory::FrameRecord record;
		record.stage = DiagnosticHistory::StageRepeated;
		record.counter = f.counter;
		record.holdReason = code;
		record.flags = f.renderAhead ? DiagnosticHistory::FlagRenderAhead : 0u;
		if (f.renderAhead && f.openLoop)
			record.flags |= DiagnosticHistory::FlagOpenLoop;
		record.rendererGeneration = f.rendererGeneration;
		record.repeatedTotal = f.holdsBefore + 1;
		to.RecordFrame(record);
		if (code != lastHoldCode)
		{
			lastHoldCode = code;
			to.RecordDecision("dejudder-hold", f.why, code, static_cast<int64_t>(f.sequence));
		}
	}

	// A drift-correction repeat queued (the frame is shown twice).
	inline void RecordCadenceRepeat(uint64_t counter, int64_t sourceTimestamp, size_t queueDepth,
		uint64_t repeatCount, uint32_t rendererGeneration,
		DiagnosticHistory::Recorder& to = DiagnosticHistory::Global()) noexcept
	{
		DiagnosticHistory::FrameRecord diag;
		diag.stage = DiagnosticHistory::StageRepeated;
		diag.counter = counter;
		diag.sourceTimestamp = sourceTimestamp;
		diag.queueDepth = static_cast<uint32_t>(queueDepth);
		diag.holdReason = RendererDiagnostics::RepeatCadenceCorrection;
		diag.repeatedTotal = repeatCount;
		diag.rendererGeneration = rendererGeneration;
		to.RecordFrame(diag);
	}

	// ---- 3b. Frame rendered: trace lines ---------------------------------------------------------

	// "De-judder: hold" lines: one line per reason held since the last lines (render-ahead
	// follow-up 2026-10-17: a count is never put on a reason it does not belong to), at most
	// every 5 s, and at teardown (Flush) so the last holds are never lost.
	class HoldLog
	{
	public:
		// One hold of reason `why`; totalHolds already counts it.
		void OnHold(const char* why, unsigned long long totalHolds, ULONGLONG nowTick = GetTickCount64())
		{
			auto entry = std::find_if(pending.begin(), pending.end(),
				[why](const std::pair<std::string, unsigned long long>& p) { return p.first == why; });
			if (entry == pending.end())
				pending.emplace_back(why, 1);
			else
				++entry->second;
			if (lastTick == 0 || nowTick - lastTick >= 5000)
			{
				Flush(totalHolds);
				lastTick = nowTick;
			}
		}

		void Flush(unsigned long long totalHolds)
		{
			for (const auto& entry : pending)
				DebugLog::Log("De-judder: hold (%s; %llu holds since the last line, %llu in total)",
					entry.first.c_str(), entry.second, totalHolds);
			pending.clear();
		}

	private:
		ULONGLONG lastTick = 0;
		std::vector<std::pair<std::string, unsigned long long>> pending;
	};

	// The entrance (input side) counters the B3 cost criterion and the capture harness read.
	// Field order is the initialiser order the host uses.
	struct EntranceCounters
	{
		unsigned long long stageRuns;      // every call (any path): the input side's cost unit
		unsigned long long sourceFrames;   // distinct source frames drawn
		unsigned long long runs;
		unsigned long long reuses;
		unsigned long long onceA;
		unsigned long long onceB;
		unsigned long long onceReused;
		unsigned long long oncePictures;
		unsigned long long onceFallbacks;
		unsigned long long statefulBreaks; // stateful history breaks (once mode)
		bool once;                         // entrance once (B3) active
	};

	// "Entrance cost" / "Entrance stages" lines: the first run, every ~2400 legacy frames of
	// time, and every 120 input-side runs (the harness reads the counters in short runs).
	inline void LogEntranceCounters(const EntranceCounters& c, unsigned long long& costLoggedAt,
		GeneralRate::LogInterval& interval,
		GeneralRate::LogInterval::Clock::time_point now = GeneralRate::LogInterval::Clock::now())
	{
		const bool batch = c.stageRuns >= costLoggedAt + 120;
		if (!interval.DueOr(c.runs == 1 || batch, now))
			return;
		costLoggedAt = c.stageRuns;
		DebugLog::Log("Entrance cost: %llu input-side runs for %llu source frames", c.stageRuns, c.sourceFrames);
		if (c.once)
			DebugLog::Log("Entrance stages: %llu runs, %llu repeats reused (once per source frame); entrance once (B3): "
				"A %llu, B ahead %llu, ring reuses %llu, pictures from processed planes %llu, fallbacks %llu, history breaks %llu",
				c.runs, c.reuses, c.onceA, c.onceB, c.onceReused, c.oncePictures, c.onceFallbacks, c.statefulBreaks);
		else
			DebugLog::Log("Entrance stages: %llu runs, %llu repeats reused (once per source frame)", c.runs, c.reuses);
	}

	// The module's interval (one per module, as the host's function-local static was).
	inline void LogEntranceCounters(const EntranceCounters& c, unsigned long long& costLoggedAt)
	{
		static GeneralRate::LogInterval interval = GeneralRate::LogInterval::ForFrames(2400);
		LogEntranceCounters(c, costLoggedAt, interval);
	}

	// ---- 4. State report: the renderer's rings ---------------------------------------------------

	// This module's rings as JSONL rows tagged "renderer" into the report's sink. Nothing may
	// cross the plugin boundary as an exception.
	inline bool WriteRendererHistory(VpDiagnosticHistorySink sink, void* context,
		int64_t referenceQpc) noexcept
	{
		try
		{
			return DiagnosticHistory::WriteJsonlToSink(
				DiagnosticHistory::Global(), "renderer", referenceQpc, sink, context);
		}
		catch (...)
		{
			return false;
		}
	}
}
