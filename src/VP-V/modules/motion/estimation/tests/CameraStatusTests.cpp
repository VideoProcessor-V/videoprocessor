// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// CameraStatus (link/CameraStatus.h) and its use by CameraExtension: the camera model's visible state (K3). Off shows nothing; a
// switch that cannot act is shown; every fit reason has text; the Original pathway shows nothing; the dump's drops are visible.
// The extension tests drive it next to a real ResyncExtension, like CameraExtensionTests. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraDump.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraExtension.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraStatus.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraStatusTestsNs
{
	namespace RE = RendererExtension;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using namespace Vpv::Camera;
	using Vpv::Camera::CameraExtension;
	using Vpv::Resync::ResyncExtension;

	bool Has(const std::vector<std::string>& lines, const char* part)
	{
		for (const std::string& l : lines)
			if (l.find(part) != std::string::npos)
				return true;
		return false;
	}

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	// A running camera: link attached, engine feeding the fit, one record fitted.
	StatusInput Running()
	{
		StatusInput in;
		in.cam.model = true;
		in.linkReport = true;
		in.coreAlive = true;
		in.engineAvailable = true;
		in.fit.have = true;
		in.fit.type = CameraModel::ModelType::Similarity;
		in.fit.confidence = 0.92;
		in.fit.fitMs = 0.3;
		in.fit.reason = CameraModel::Reason::Ok;
		in.fit.published = true;
		in.fitted = 120;
		in.published = 118;
		in.late = 2;
		return in;
	}

	struct EnvGuard   // sets an environment variable for one test and restores it
	{
		std::string name, old;
		bool had = false;
		EnvGuard(const char* n, const char* value) : name(n)
		{
			char* v = nullptr;
			size_t sz = 0;
			if (_dupenv_s(&v, &sz, n) == 0 && v)
			{
				had = true;
				old = v;
				free(v);
			}
			_putenv_s(n, value);
		}
		~EnvGuard() { _putenv_s(name.c_str(), had ? old.c_str() : ""); }
	};

	struct TempCfg
	{
		std::string path;
		explicit TempCfg(const std::string& text)
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-camera-status-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << text;
		}
		~TempCfg() { std::remove(path.c_str()); }
		Vpv::Skeleton::ConfigLoader Loader() const
		{
			const std::string p = path;
			return [p](ConfigFile& c) { return c.Load(p, ConfigFile::ReadPolicy::Fresh); };
		}
	};

	struct FakeRenderer
	{
		VpResync::Shadow resync;
		VpResync::Drive resyncDrive;
		Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		FrameAnalysis::Store store{ FrameAnalysis::RingDepth(4, 4) };
	};

	struct Rig
	{
		explicit Rig(const std::string& text) : cfg(text), ext(cfg.Loader()) { Tap::SetStore(&r, &r.store); }
		~Rig()
		{
			Life(RE::Lifecycle::GpuReleased);
			Tap::SetStore(&r, nullptr);
		}
		TempCfg cfg;
		CameraExtension ext;
		ResyncExtension resync;
		FakeRenderer r;

		const void* Host() const { return &r; }
		void Life(RE::Lifecycle kind, int code = 0)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = kind; f.code = code;
			ext.OnLifecycle(f);
		}
		void ResyncApply(const char* mode)
		{
			RE::LifecycleFacts f;
			f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
			resync.OnLifecycle(f);
		}
		void Frame(int64_t seq)
		{
			RE::IngressContext c;
			c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
			c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
			resync.OnIngress(c);
			RE::FrameContext f;
			f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
			resync.OnFrame(f);
			ext.OnFrame(f);
			f.phase = RE::FramePhase::AfterRender;
			ext.OnFrame(f);
		}
		std::vector<std::string> Lines(RE::StatusKind kind)
		{
			std::vector<std::string> lines;
			RE::Context c; c.host = &r;
			ext.OnStatus(c, kind, lines, nullptr, nullptr);
			return lines;
		}
		std::string Command()
		{
			RE::Context c; c.host = &r;
			std::string reply;
			ext.OnCommand(c, "camera", "status", reply);
			return reply;
		}
	};

	TEST_CLASS(CameraStatusTests)
	{
	public:
		TEST_METHOD(OffAndOriginalShowNothing)
		{
			const StatusText off = BuildStatus(StatusInput());
			Assert::IsTrue(off.state == CameraState::Off);
			Assert::IsTrue(off.lines.empty() && off.fallbacks.empty() && off.detail.empty());
			StatusInput original = Running();
			original.cam.model = true;
			original.cam.overlay = OverlayMode::Vectors;
			original.cam.meas.staticGate = true;
			original.cam.use.grain = true;
			original.cam.originalPathway = true;
			original.cam.problems.push_back("camera_model 'x' is not valid (on|off); kept off");
			const StatusText none = BuildStatus(original);
			Assert::IsTrue(none.lines.empty() && none.fallbacks.empty() && none.detail.empty(), L"Original: no line, no fallback");
		}

		TEST_METHOD(ASwitchWhileTheModelIsOffIsShownAsInert)
		{
			StatusInput in;
			in.cam.use.grain = true;
			in.cam.meas.staticGate = true;
			const StatusText t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::InertSwitches);
			Assert::IsTrue(Has(t.lines, "camera: model off; camera_model is off, so camera_meas_static_gate, camera_use_grain have no effect"));
			Assert::IsTrue(Has(t.fallbacks, "camera_model: camera_model is off, so"));
			Assert::IsTrue(t.detail.empty());
		}

		TEST_METHOD(EachStateHasItsText)
		{
			StatusInput in;
			in.cam.model = true;
			StatusText t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::LinkOff);
			Assert::IsTrue(Has(t.lines, "camera: model on; the camera link is not attached (camera_link is off)"));
			Assert::IsTrue(Has(t.fallbacks, "camera_model: camera_link is off"));

			in.linkReport = true;
			in.linkProblem = "no frame analysis store for this renderer";
			t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::LinkUnavailable);
			Assert::IsTrue(Has(t.lines, "the camera link is unavailable (no frame analysis store for this renderer)"));

			in.coreAlive = true;
			t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::WaitingForEngine);
			Assert::IsTrue(Has(t.lines, "camera: model on; waiting for the vector engine (P5a pending): no fit runs"));
			Assert::IsTrue(Has(t.fallbacks, "camera_model: waiting for the vector engine (P5a pending)"));

			in.engineAvailable = true;
			t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::NoFitYet);
			Assert::IsTrue(Has(t.lines, "camera: model on; no fit published yet; users none; reason none yet"));
			Assert::IsTrue(t.fallbacks.empty(), L"waiting for the first fit is not a downgrade");

			t = BuildStatus(Running());
			Assert::IsTrue(t.state == CameraState::Fitting);
			Assert::AreEqual<size_t>(1, t.lines.size());
			Assert::AreEqual(std::string("camera: similarity 0.92, fit 0.3 ms; users none; reason ok"), t.lines[0]);
			Assert::IsTrue(t.fallbacks.empty());
		}

		TEST_METHOD(EveryFitReasonHasText)
		{
			for (int r = 0; r <= static_cast<int>(CameraModel::Reason::Disabled); ++r)
			{
				StatusInput in = Running();
				in.fit.reason = static_cast<CameraModel::Reason>(r);
				const StatusText t = BuildStatus(in);
				const std::string want = std::string("reason ") + CameraModel::ReasonTextOf(in.fit.reason, 0);
				Assert::IsTrue(Has(t.lines, want.c_str()), W(want).c_str());
			}
			StatusInput dyn = Running();
			dyn.fit.reason = CameraModel::Reason::Ambiguous;
			dyn.fit.verdictFlags = CameraModel::kVerdictDynamicTexture;
			Assert::IsTrue(Has(BuildStatus(dyn).lines, "reason ambiguous (dynamic texture, water-like)"));
			dyn.fit.verdictFlags = CameraModel::kVerdictStaticCamera;
			Assert::IsTrue(Has(BuildStatus(dyn).lines, "reason ambiguous (static background explains the motion better"));
			StatusInput late = Running();
			late.fit.published = false;
			Assert::IsTrue(Has(BuildStatus(late).lines, "(late: not published before the frame)"));
		}

		TEST_METHOD(TheUsersAreListed)
		{
			StatusInput in = Running();
			in.cam.use.grain = true;
			in.cam.use.dejudder = true;
			Assert::IsTrue(Has(BuildStatus(in).lines, "users grain+dejudder;"));
			in.cam.use.deband = true;
			Assert::IsTrue(Has(BuildStatus(in).lines, "users grain+deband+dejudder;"));
		}

		TEST_METHOD(InvalidAndUnknownKeysAreShownEvenWhenNothingElseIsSet)
		{
			StatusInput in;
			in.cam.problems.push_back("camera_model 'maybe' is not valid (on|off); kept off");
			in.cam.unknownKeys.push_back("camera_future");
			const StatusText t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::Off);
			Assert::AreEqual<size_t>(1, t.fallbacks.size());
			Assert::IsTrue(Has(t.fallbacks, "camera_settings: camera_model 'maybe' is not valid"));
			Assert::IsTrue(Has(t.fallbacks, "unknown key 'camera_future' in [vpv.camera] ignored"));
			Assert::IsTrue(t.lines.empty());
		}

		TEST_METHOD(TheDetailSectionHasWeightsDeadlinesAndGuidance)
		{
			StatusInput in = Running();
			in.cam.use.grain = true;
			in.cam.use.dejudder = true;
			in.cam.meas.guidedCandidate = true;
			in.consumers.push_back(ConsumerFact{ "dejudder", true, 0.75 });
			StatusText t = BuildStatus(in);
			Assert::IsTrue(Has(t.detail, "camera deadline misses: 2 late (fitted 120, published 118)"));
			Assert::IsTrue(Has(t.detail, "camera guidance: no pair guided yet"));
			Assert::IsTrue(Has(t.detail, "camera use dejudder: on, weight 0.75"));
			Assert::IsTrue(Has(t.detail, "camera use grain: on, weight n/a (no fit)"));
			Assert::IsTrue(Has(t.detail, "camera use filler: off, weight n/a (no fit)"));
			in.measurementLine = "camera guidance: narrow 71% (escalated 0.4%)";
			t = BuildStatus(in);
			Assert::IsTrue(Has(t.detail, "camera guidance: narrow 71% (escalated 0.4%)"));
			in.cam.meas = CameraModel::MeasurementSwitches();
			in.measurementLine.clear();
			Assert::IsTrue(Has(BuildStatus(in).detail, "camera guidance: off (no measurement switch is on)"));
		}

		TEST_METHOD(DumpDropsAreVisible)
		{
			StatusInput in = Running();
			in.dumpOn = true;
			in.dumpDirectory = "D:\\dump\\clip";
			in.dump.written = 5;
			StatusText t = BuildStatus(in);
			Assert::IsTrue(Has(t.detail, "camera dump: written 5, dropped 0, failed 0, queued 0 (D:\\dump\\clip)"));
			Assert::IsTrue(t.fallbacks.empty(), L"no drop, no fallback");
			in.dump.dropped = 3;
			in.dump.failed = 1;
			t = BuildStatus(in);
			Assert::IsTrue(Has(t.fallbacks, "camera_dump: 3 item(s) dropped (the writer is behind), 1 failed to write"));
		}

		TEST_METHOD(ADumpWithNoCameraKeyStillShowsItsDrops)
		{
			StatusInput in;   // nothing set in [vpv.camera]
			in.linkReport = true;
			in.coreAlive = true;
			in.dumpOn = true;
			in.dumpDirectory = "D:\\dump\\clip";
			in.dump.written = 2;
			in.dump.dropped = 4;
			const StatusText t = BuildStatus(in);
			Assert::IsTrue(t.state == CameraState::Off);
			Assert::IsTrue(t.lines.empty());
			Assert::IsTrue(Has(t.detail, "camera dump: written 2, dropped 4, failed 0, queued 0"));
			Assert::IsTrue(Has(t.fallbacks, "camera_dump: 4 item(s) dropped (the writer is behind), 0 failed to write"));
			// A switch set while the model is off does not hide it either.
			in.cam.use.grain = true;
			const StatusText inert = BuildStatus(in);
			Assert::IsTrue(inert.state == CameraState::InertSwitches);
			Assert::IsTrue(Has(inert.fallbacks, "camera_dump: 4 item(s) dropped"));
		}

		TEST_METHOD(TheOffReplyOfTheStatusCommandIsPinned)
		{
			Rig rig("[general]\nx=1\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::AreEqual(std::string("camera: off (camera_link = off and no [vpv.camera] key set, or the Original pathway)"), rig.Command());
		}

		TEST_METHOD(ALinkWithADumpAndNoKeyShowsTheDumpSection)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string target = std::string(dir) + "vp-camera-status-dump2-" + std::to_string(GetCurrentProcessId());
			EnvGuard set("VP_CAMERA_DUMP", target.c_str());
			Rig rig("[vpv.skeleton]\ncamera_link=report\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsNotNull(rig.ext.DumpOf(rig.Host()));
			Assert::IsTrue(rig.Command().find("camera dump: written 0, dropped 0, failed 0, queued 0") != std::string::npos);
		}

		// ---- the extension ----
		TEST_METHOD(AllOffInTheSectionIsInert)
		{
			Rig rig("[vpv.camera]\ncamera_model: off\ncamera_use_grain: off\ncamera_model_overlay: off\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			rig.Life(RE::Lifecycle::SettingsApplied);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
			Assert::IsFalse(rig.ext.CameraSettingsOf(rig.Host()).AnySet());
		}

		TEST_METHOD(ModelOnWithTheLinkOffShowsWhyNothingRuns)
		{
			Rig rig("[vpv.camera]\ncamera_model: on\ncamera_use_grain: on\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()), L"camera_link is off: no core");
			Assert::IsTrue(rig.ext.CameraSettingsOf(rig.Host()).ModelWanted());
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "camera=model on; the camera link is not attached (camera_link is off)"));
			Assert::IsFalse(Has(rig.Lines(RE::StatusKind::Lines), "report mode"), L"no report-mode line without camera_link");
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_model: camera_link is off"));
			Assert::IsNull(rig.ext.Query("vpv.camera.v1"), L"no consumer is fed");
			Assert::IsTrue(rig.Command().find("camera use grain: on, weight n/a (no fit)") != std::string::npos, L"Ctrl+Alt+D section");
		}

		TEST_METHOD(ModelOnWithTheLinkReportingWaitsForTheVectorEngine)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=report\n[vpv.camera]\ncamera_model: on\ncamera_use_dejudder: on\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::GpuCreated);
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			CameraLink::CameraLinkCore* core = rig.ext.CoreOf(rig.Host());
			Assert::IsNotNull(core);
			Assert::IsFalse(core->Allocated(), L"K3 allocates nothing: no vector engine");
			Assert::IsFalse(core->HasThread());
			const std::vector<std::string> lines = rig.Lines(RE::StatusKind::Lines);
			Assert::IsTrue(Has(lines, "camera=report mode; fit idle (no vector engine yet, P5a pending)"), L"the K1 line is unchanged");
			Assert::IsTrue(Has(lines, "camera=model on; waiting for the vector engine (P5a pending)"));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_model: waiting for the vector engine (P5a pending)"));
			Assert::IsNull(rig.ext.Query("vpv.camera.v1"));
		}

		TEST_METHOD(ASwitchSetWhileTheModelIsOffIsAFallback)
		{
			Rig rig("[vpv.camera]\ncamera_meas_static_gate: on\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_model: camera_model is off, so camera_meas_static_gate have no effect"));
			Assert::IsFalse(rig.ext.CameraSettingsOf(rig.Host()).Measurement().Any(), L"inert until the model is on");
		}

		TEST_METHOD(AnInvalidKeyIsShownWhileEverythingIsOff)
		{
			Rig rig("[vpv.camera]\ncamera_model: maybe\n");
			rig.Life(RE::Lifecycle::GpuCreated);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Fallbacks), "camera_settings: camera_model 'maybe' is not valid"));
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
		}

		TEST_METHOD(TheOriginalPathwayForcesTheModelOff)
		{
			Rig rig("[vpv.skeleton]\ncamera_link=report\n[vpv.camera]\ncamera_model: on\ncamera_model_overlay: vectors\n");
			rig.ResyncApply("on");
			rig.Life(RE::Lifecycle::SettingsApplied, 1);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::AreEqual<size_t>(0, rig.ext.HostStates());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
			Assert::IsTrue(rig.Lines(RE::StatusKind::Fallbacks).empty());
			rig.Life(RE::Lifecycle::SettingsApplied, 0);
			Assert::IsTrue(rig.ext.Active(rig.Host()));
			rig.Life(RE::Lifecycle::SettingsApplied, 1);
			Assert::IsFalse(rig.ext.Active(rig.Host()));
			Assert::IsNull(rig.ext.CoreOf(rig.Host()), L"torn down");
			Assert::IsTrue(rig.Lines(RE::StatusKind::Lines).empty());
		}

		TEST_METHOD(TheDumpWriterExistsOnlyWhenTheVariableNamesADirectory)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string target = std::string(dir) + "vp-camera-status-dump-" + std::to_string(GetCurrentProcessId());
			const std::string text = "[vpv.skeleton]\ncamera_link=report\n[vpv.camera]\ncamera_model: on\n";
			{
				EnvGuard unset("VP_CAMERA_DUMP", "");
				Rig rig(text);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				Assert::IsNotNull(rig.ext.CoreOf(rig.Host()));
				Assert::IsNull(rig.ext.DumpOf(rig.Host()), L"variable unset: no writer, no thread");
			}
			{
				EnvGuard status("VP_CAMERA_DUMP", "1");
				Rig rig(text);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				Assert::IsNull(rig.ext.DumpOf(rig.Host()), L"=1 keeps its older meaning (status log): no writer");
			}
			{
				EnvGuard set("VP_CAMERA_DUMP", target.c_str());
				Rig rig(text);
				rig.ResyncApply("on");
				rig.Life(RE::Lifecycle::GpuCreated);
				const CameraLink::CameraDump* dump = rig.ext.DumpOf(rig.Host());
				Assert::IsNotNull(dump);
				Assert::IsTrue(dump->HasThread());
				Assert::AreEqual(std::string("engine"), dump->Config().mode);
				Assert::IsTrue(Has(rig.Lines(RE::StatusKind::Lines), "camera=model on"));
				Assert::IsTrue(rig.Command().find("camera dump: written 0, dropped 0, failed 0, queued 0") != std::string::npos);
				rig.Life(RE::Lifecycle::GpuReleased);
				Assert::IsNull(rig.ext.DumpOf(rig.Host()), L"released with the host: the thread is joined");
			}
		}
	};
}
