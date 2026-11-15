// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/FrameQuiet.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// fx/dv-frame-quiet, CPU only (no device): the two-call monitor frame (FrameBegin before the render,
// FrameSubmit after the submit), the skipped / late counters that keep a loss visible, the source time
// the renderer hands to the monitor's decisions, the status decision, and the pinned order of the host's
// call sites (the monitor's GPU work is after the frame's submit, the decisions use source time).
namespace VideoProcessorTest
{
	namespace
	{
		using SourceResolutionMonitor::Monitor;

		constexpr int kW = 1920, kH = 1080;
		constexpr double kBlack = 64.0 / 1023.0;

		std::vector<float> NoiseTiles(uint64_t sequence)
		{
			std::mt19937 rng(static_cast<unsigned>(sequence) * 2654435761u + 7u);
			std::normal_distribution<double> n(0.5, 0.12);
			std::vector<float> tiles(static_cast<size_t>(8) * Monitor::kTile * Monitor::kTile);
			for (float& v : tiles) v = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, n(rng))) * 1023.0) / 1023.0);
			return tiles;
		}

		void AnalyseNoise(Monitor& m, uint64_t sequence, double seconds)
		{
			m.AnalyseTilesForTest(NoiseTiles(sequence), 8, seconds, kW, kH, {}, {}, 8, Monitor::kTile, kBlack, sequence);
		}

		// A frame with no planes: FrameBegin needs no GPU and reports "no source".
		pl_frame NoPlanes() { pl_frame f{}; return f; }

		std::string ReadText(const std::string& path)
		{
			std::ifstream in(path, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		std::string RepositoryRoot()
		{
			std::string root = __FILE__;
			std::replace(root.begin(), root.end(), '/', '\\');
			root.resize(root.rfind("\\src\\VP-V\\tests\\"));
			return root;
		}

		size_t CountOf(const std::string& text, const std::string& part)
		{
			size_t n = 0;
			for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
			return n;
		}
	}

	TEST_CLASS(SourceResolutionFrameQuietTests)
	{
	public:
		TEST_METHOD(FrameBeginIsCpuOnlyAndFrameSubmitWithoutADeviceIsCountedNotSilent)
		{
			Monitor m;
			m.SetFrameBound(true, 4);
			const pl_frame f = NoPlanes();
			m.FrameBegin(f, 1, 0.0);
			Assert::AreEqual<unsigned long long>(0, m.SubmitSkipped(), L"nothing is lost by a Begin");
			m.FrameSubmit(nullptr, nullptr, f, 1);   // no GPU / dispatch: the frame had no GPU work
			Assert::AreEqual<unsigned long long>(1, m.SubmitSkipped());
			Assert::IsFalse(m.Reproducible(), L"a frame without its tile copy makes the run not reproducible");
			Assert::IsTrue(SourceResolutionMonitor::Describe(m.Read(), 1.0, true).find("copies skipped 1") != std::string::npos,
				L"the skipped copy is shown");
		}

		TEST_METHOD(ASubmitForAnotherSequenceOrWithoutABeginDoesNothing)
		{
			Monitor m;
			m.SetFrameBound(true, 4);
			const pl_frame f = NoPlanes();
			m.FrameSubmit(nullptr, nullptr, f, 5);   // no Begin
			Assert::AreEqual<unsigned long long>(0, m.SubmitSkipped());
			m.FrameBegin(f, 6, 0.1);
			m.FrameSubmit(nullptr, nullptr, f, 5);   // another frame's submit: ignored, the begin still waits
			Assert::AreEqual<unsigned long long>(0, m.SubmitSkipped());
			m.FrameSubmit(nullptr, nullptr, f, 6);
			Assert::AreEqual<unsigned long long>(1, m.SubmitSkipped());
			m.FrameSubmit(nullptr, nullptr, f, 6);   // already consumed
			Assert::AreEqual<unsigned long long>(1, m.SubmitSkipped());
		}

		TEST_METHOD(ABeginWhoseSubmitNeverCameIsCountedByTheNextBegin)
		{
			Monitor m;
			m.SetFrameBound(true, 4);
			const pl_frame f = NoPlanes();
			m.FrameBegin(f, 1, 0.0);
			m.FrameBegin(f, 2, 0.1);   // frame 1 failed before its end
			Assert::AreEqual<unsigned long long>(1, m.SubmitSkipped());
			Assert::IsFalse(m.Reproducible());
		}

		TEST_METHOD(AResultThatArrivesAfterItsReleasePointIsCountedAsAMissedWait)
		{
			Monitor m;
			m.SetFrameBound(true, 2);
			// The decision for frame 10 releases the analysis of frames <= 8.
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.FrameFrontForTest(10, 10.0);
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 10.0, 10);
			Assert::AreEqual<unsigned long long>(0, m.WaitsMissed());
			AnalyseNoise(m, 9, 9.0);   // frame 9 > 8: still ahead of the release point, in time
			Assert::AreEqual<unsigned long long>(0, m.WaitsMissed());
			Assert::IsTrue(m.Reproducible());
			AnalyseNoise(m, 5, 5.0);   // frame 5 <= 8: the decision for it is already made
			Assert::AreEqual<unsigned long long>(1, m.WaitsMissed(), L"a late result is a missed wait, counted");
			Assert::IsFalse(m.Reproducible());
			Assert::IsTrue(SourceResolutionMonitor::Describe(m.Read(), 1.0, true).find("waits missed 1") != std::string::npos);
		}

		TEST_METHOD(ADiscontinuityForgetsTheReleasePointAndANewEraStartsClean)
		{
			Monitor m;
			m.SetFrameBound(true, 2);
			std::string why;
			double fx = 1.0, fy = 1.0;
			m.FrameFrontForTest(100, 100.0);
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 100.0, 100);
			m.Discontinuity("test seek");
			m.FrameFrontForTest(3, 101.0);   // the sequence restarted (a new file): the old release point means nothing
			m.AutoFactors(kW, kH, 0.5, why, fx, fy, 101.0, 3);
			AnalyseNoise(m, 2, 2.0);   // frame 2 is of the new stretch
			Assert::AreEqual<unsigned long long>(0, m.WaitsMissed(), L"the earlier stretch's release point is forgotten");
			m.SetFrameBound(true, 6);   // a new lead: a fresh era
			Assert::AreEqual<unsigned long long>(0, m.WaitsMissed());
			Assert::IsTrue(m.Reproducible());
		}

		TEST_METHOD(SourceTimeIsFramesTimesThePeriodAndHoldsWithoutARateOrOnASeek)
		{
			FrameQuiet::SourceTime t;
			Assert::AreEqual(0.0, t.Advance(100, 25.0), 0.0, L"the first frame is time zero");
			Assert::AreEqual(0.04, t.Advance(101, 25.0), 1e-12);
			Assert::AreEqual(0.16, t.Advance(104, 25.0), 1e-12, L"skipped frames count: 3 more periods");
			Assert::AreEqual(0.16, t.Advance(104, 25.0), 1e-12, L"the same frame again: no time passes");
			Assert::AreEqual(0.16, t.Advance(105, 0.0), 1e-12, L"an unknown rate holds the time still");
			Assert::AreEqual(0.20, t.Advance(106, 25.0), 1e-12);
			Assert::AreEqual(0.20, t.Advance(50, 25.0), 1e-12, L"a seek back holds the time");
			Assert::AreEqual(0.24, t.Advance(51, 25.0), 1e-12, L"and goes on from the new place");
			Assert::AreEqual(0.24, t.Seconds(), 1e-12);
			t.Reset();
			Assert::AreEqual(0.0, t.Advance(7, 25.0), 0.0);
		}

		TEST_METHOD(SourceTimeDoesNotDependOnTheWallClockOrTheCallPattern)
		{
			FrameQuiet::SourceTime a, b;
			double ta = 0.0, tb = 0.0;
			for (uint64_t s = 1; s <= 200; ++s) ta = a.Advance(s, 24000.0 / 1001.0);
			b.Advance(1, 24000.0 / 1001.0);
			for (uint64_t s = 3; s <= 198; s += 3) b.Advance(s, 24000.0 / 1001.0);
			tb = b.Advance(200, 24000.0 / 1001.0);
			Assert::AreEqual(ta, tb, 1e-9, L"the same frames give the same time, however often it is asked");
		}

		TEST_METHOD(TheLeadIsFramesAndTheStatusDecisionNamesBothCauses)
		{
			Assert::IsTrue(FrameQuiet::LeadFrames() >= 2, L"a lead of at least a couple of frames absorbs the readback and the analysis");
			Assert::IsFalse(FallbackCatalog::NativeReadback(0, 0).engaged);
			Assert::IsTrue(FallbackCatalog::NativeReadback(1, 0).engaged);
			Assert::IsTrue(FallbackCatalog::NativeReadback(0, 1).engaged);
		}

		// Re-placed (refs/121): the renderer host (Bill's file) has NO frame-quiet call site; Arm + Begin are in the planner
		// segment (VP-V/segments/EntrancePlanner.inl, reached only outside the Original pathway) and the end of the frame is
		// the R12 AfterPresent seam handler (SkeletonModule::OnAfterPresent). The monitor's GPU work is never issued by the
		// planner, the decisions take source time, and the end comes before the handler's early return.
		TEST_METHOD(TheFrameQuietCallsSitInThePlannerAndTheAfterPresentSeamNotInTheRenderer)
		{
			const std::string renderer = ReadText(RepositoryRoot() + "\\src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			const std::string planner = ReadText(RepositoryRoot() + "\\src\\VP-V\\segments\\EntrancePlanner.inl");
			const std::string skeleton = ReadText(RepositoryRoot() + "\\src\\VP-V\\modules\\control\\skeleton\\SkeletonModule.cpp");
			Assert::IsFalse(renderer.empty() || planner.empty() || skeleton.empty(), L"the sources");
			Assert::AreEqual<size_t>(0, CountOf(renderer, "frameQuiet"), L"no frame-quiet line in the Bill-owned renderer");
			Assert::AreEqual<size_t>(0, CountOf(planner, "sourceMonitor.Frame("), L"the monitor's GPU work is not issued before the render");
			Assert::AreEqual<size_t>(1, CountOf(planner, "frameQuiet.Arm("));
			Assert::AreEqual<size_t>(1, CountOf(planner, "frameQuiet.Begin("));
			Assert::AreEqual<size_t>(0, CountOf(planner, "-1.0, sourceSequence);"), L"AutoFactors takes source time, not the steady clock");
			Assert::AreEqual<size_t>(0, CountOf(planner, "AutoTransition();"), L"AutoTransition takes source time");
			Assert::AreEqual<size_t>(1, CountOf(skeleton, "FrameQuiet::Deferred::EndThisFrame();"));
			const size_t handler = skeleton.find("void SkeletonModule::OnAfterPresent(");
			const size_t end = skeleton.find("FrameQuiet::Deferred::EndThisFrame();");
			const size_t early = skeleton.find("return;", handler);
			Assert::IsTrue(handler != std::string::npos && handler < end && end < early, L"End runs before the handler's early return");
			const size_t arm = planner.find("frameQuiet.Arm(");
			const size_t begin = planner.find("frameQuiet.Begin(");
			Assert::IsTrue(arm < begin, L"Arm before Begin");
		}

		TEST_METHOD(DeferredEndWithoutABeginFlushesNothingWhenNoVpWorkRan)
		{
			FrameQuiet::Deferred deferred;
			deferred.Arm(nullptr);
			Assert::IsTrue(LedDrive::DeferredCopies::ForThisThread().Armed());
			deferred.EndFrame();
			Assert::IsFalse(LedDrive::DeferredCopies::ForThisThread().Armed(), L"End disarms for the next frame");
			Assert::AreEqual<unsigned long long>(0, deferred.Flushes(), L"no VP GPU work ran, so no pl_gpu_flush");
			const FallbackCatalog::Decision d = deferred.TakeDecision();
			Assert::AreEqual(std::string("native readback"), d.id);
			deferred.EndFrame();   // a second End is a no-op
			Assert::AreEqual<unsigned long long>(0, deferred.MissedEnds());
		}

		TEST_METHOD(AnArmWithoutAnEndIsMadeUpAndCounted)
		{
			FrameQuiet::Deferred deferred;
			deferred.Arm(nullptr);
			deferred.Arm(nullptr);   // the seam was not reached: the stale frame is ended now, visibly
			Assert::AreEqual<unsigned long long>(1, deferred.MissedEnds());
			deferred.EndFrame();
		}

		// Audit 1: a refused fence signal never leaves a slot waiting for a value that was not signalled; it is counted and named.
		TEST_METHOD(ARefusedFenceSignalZeroesTheValueIsCountedAndEngagesTheStatus)
		{
			const unsigned long long before = LedDrive::SignalFailures().load();
			uint64_t value = 7;
			Assert::IsTrue(LedDrive::AcceptSignal(0, value));
			Assert::AreEqual<uint64_t>(7, value, L"an accepted signal keeps its value");
			Assert::IsFalse(LedDrive::AcceptSignal(static_cast<long>(0x80004005), value));
			Assert::AreEqual<uint64_t>(0, value, L"a refused signal is never waited on");
			Assert::AreEqual<unsigned long long>(before + 1, LedDrive::SignalFailures().load());
			const FallbackCatalog::Decision d = FallbackCatalog::NativeReadback(0, 0, 1);
			Assert::AreEqual(std::string("native readback"), d.id);
			Assert::IsTrue(d.engaged, L"engaged");
			Assert::IsTrue(d.reason.find("refused 1 fence signal") != std::string::npos, L"named");
		}

		// Audit 8: a transient (a refused signal) shows for a window of frames, then the status clears on its own.
		TEST_METHOD(ATransientEngagesTheStatusForAWindowOfFramesThenClears)
		{
			FrameQuiet::Deferred deferred;
			deferred.Arm(nullptr);
			deferred.EndFrame();
			Assert::IsFalse(deferred.TakeDecision().engaged, L"quiet at the start");
			uint64_t v = 1;
			LedDrive::AcceptSignal(static_cast<long>(0x80004005), v);   // one refusal
			deferred.Arm(nullptr);
			deferred.EndFrame();
			Assert::IsTrue(deferred.TakeDecision().engaged, L"engaged when it happens");
			for (int i = 0; i < 598; ++i) { deferred.Arm(nullptr); deferred.EndFrame(); }
			Assert::IsTrue(deferred.TakeDecision().engaged, L"still shown inside the window");
			for (int i = 0; i < 3; ++i) { deferred.Arm(nullptr); deferred.EndFrame(); }
			Assert::IsFalse(deferred.TakeDecision().engaged, L"clear after the window, nothing new");
		}

		// A frame nothing armed (the seam handler on an unarmed frame): nothing flushes, no End is counted as missed.
		TEST_METHOD(AFrameThatWasNeverArmedFlushesNothingAndAddsNoDecision)
		{
			FrameQuiet::Deferred deferred;
			deferred.EndFrame();
			Assert::AreEqual<unsigned long long>(0, deferred.Flushes());
			Assert::AreEqual<unsigned long long>(0, deferred.MissedEnds());
		}

		// ---- S3 (refs/121 decision (i)): the frame-bound engage lands on the next detected cut, read ahead ----

		struct BoundRun
		{
			uint64_t engagedAt = 0;
			unsigned long long cancelled = 0;
			std::string why;
			unsigned long long reactive = 0;
			unsigned long long verified = 0;
			std::string describe;
			std::string trace;
		};

		// A frame-bound run (D = 8) with a clear lower reading every 10 frames. cutAt != 0: the look-ahead reports that cut from
		// 6 frames before it (its worker finished). cutAt == 0: no look-ahead at all (the reactive fallback).
		BoundRun RunBoundEngage(uint64_t cutAt, unsigned lagSeed, double aheadRatio = 0.5, uint64_t lastFrame = 600)
		{
			Monitor m;
			m.SetFrameBound(true, 8);
			std::mt19937 rng(lagSeed);
			BoundRun out;
			std::deque<uint64_t> pending;
			bool aheadInjected = false;
			auto lower = [](unsigned long long frames) {
				SourceResolutionMonitor::Snapshot s;
				s.reading = true; s.nativeWidth = kW; s.nativeHeight = kH;
				s.declaredRatio = s.liveRatio = s.frameRatio = 0.5;
				s.confidence = 0.9; s.frames = frames;
				return s;
			};
			for (uint64_t seq = 1; seq <= lastFrame && out.engagedAt == 0; ++seq)
			{
				const double seconds = static_cast<double>(seq) * 0.0625;
				if (seq % 10 == 0) { m.NoteSubmittedForTest(seq); pending.push_back(seq); }
				for (unsigned k = static_cast<unsigned>(rng() % 4); k > 0 && !pending.empty(); --k)
				{
					const uint64_t q = pending.front(); pending.pop_front();
					m.InjectResultForTest(lower(q / 10), q);
				}
				while (!m.ReadyFor(seq) && !pending.empty())
				{
					const uint64_t q = pending.front(); pending.pop_front();
					m.InjectResultForTest(lower(q / 10), q);
				}
				if (cutAt != 0 && seq + 6 >= cutAt)
				{
					if (!aheadInjected)   // the event is injected once; later frames only advance the read position and keep the counters (the real worker carries them)
					{
						Monitor::AheadState a;
						a.eventSequence = cutAt; a.targetSequence = cutAt + 1; a.ready = true; a.informative = true; a.ratio = aheadRatio;
						m.SetAheadForTest(a, seq + 6);
						aheadInjected = true;
					}
					else m.SetAheadForTest(m.Ahead(), seq + 6);
				}
				std::string why;
				double fx = 1.0, fy = 1.0;
				m.AutoFactors(kW, kH, 0.5, why, fx, fy, seconds, seq);
				if (fx < 1.0 || fy < 1.0) { out.engagedAt = seq; out.why = why; }
				char line[64]; snprintf(line, sizeof(line), "%llu:%.3f;", static_cast<unsigned long long>(seq), fx);
				out.trace += line;
			}
			out.reactive = m.EngagedReactive();
			out.verified = m.Ahead().verified;
			out.cancelled = m.Ahead().cancelled;
			out.describe = SourceResolutionMonitor::Describe(m.Read(), 0.5, true);
			return out;
		}

		TEST_METHOD(AFrameBoundEngageLandsOnTheNextDetectedCutForThreeSeeds)
		{
			const uint64_t cuts[3] = { 131, 187, 242 };
			for (unsigned seed = 1; seed <= 3; ++seed)
			{
				const uint64_t cut = cuts[seed - 1];
				const BoundRun a = RunBoundEngage(cut, seed);
				const BoundRun b = RunBoundEngage(cut, seed + 100);   // the worker's timing differs, the decisions do not
				Logger::WriteMessage(("seed " + std::to_string(seed) + ": cut " + std::to_string(cut) + " engaged at " + std::to_string(a.engagedAt) + " (" + a.why + ")\n").c_str());
				Assert::AreEqual<unsigned long long>(cut, a.engagedAt, L"engaged on the cut frame itself");
				Assert::IsTrue(a.why.find("look-ahead verified") != std::string::npos, L"through the look-ahead, not the reactive trigger");
				Assert::AreEqual<unsigned long long>(0, a.reactive, L"no reactive fallback was used");
				Assert::AreEqual<unsigned long long>(1, a.verified);
				Assert::AreEqual<unsigned long long>(cut, b.engagedAt, L"the same frame whatever the worker's timing");
				Assert::AreEqual(a.trace, b.trace, L"the same decisions run to run");
			}
		}

		TEST_METHOD(WithoutALookAheadTheReactiveEngageIsAVisibleCountedFallback)
		{
			const BoundRun r = RunBoundEngage(0, 5);
			Assert::IsTrue(r.engagedAt != 0, L"the reactive trigger still engages (a crossfade after the wait)");
			Assert::AreEqual<unsigned long long>(1, r.reactive);
			Assert::IsTrue(r.describe.find("reactive fallback") != std::string::npos, L"shown in the Ctrl+I / Config status line, never log-only");
		}

		// Original (owner REVERT 2026-11-12, bda439bd9): PlanEntrance runs in Original on purpose (it feeds the source-resolution
		// monitor as before P4, 77fe0397e) and its body returns before any VP-V stage. source_resolution_frame_bound is off there,
		// so Begin runs the monitor inline and leaves nothing pending. C1 gate SHOULD-1: the REAL Original pattern is
		// OriginalFrame() with no End (the AfterPresent seam is skipped in Original): no flush, no decision, no missed End.
		// The Arm + End pairs this test used to call "Original" are a VP-pathway frame with no VP native work; kept, so named.
		// Category Cpu: the name says GPU (pl_gpu_flush), the body is a source-text check plus FrameQuiet counters with null
		// GPUs (no device).
		BEGIN_TEST_METHOD_ATTRIBUTE(OriginalMakesNoPlGpuFlushCallsAtTheHostLevel)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(OriginalMakesNoPlGpuFlushCallsAtTheHostLevel)
		{
			const std::string renderer = ReadText(RepositoryRoot() + "\\src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			Assert::IsTrue(renderer.find("\t\tPlanEntrance(renderImage, target, image, sourceSequence,") != std::string::npos &&
				renderer.find("if (!pipelineSlots.OriginalPathway()) { PlanEntrance(") == std::string::npos,
				L"the planner call is unguarded: the Original pathway still feeds the source-resolution monitor (owner REVERT 2026-11-12)");
			FrameQuiet::Deferred& frameQuiet = FrameQuiet::Deferred::ForThisThread();
			const unsigned long long before = frameQuiet.Flushes();
			for (int i = 0; i < 50; ++i) FrameQuiet::Deferred::EndThisFrame();   // the seam handler on frames nothing armed
			Assert::AreEqual(before, frameQuiet.Flushes(), L"no flush on an unarmed frame");
			for (int i = 0; i < 50; ++i)   // Original: OriginalFrame only, never an End (the seam is skipped there)
			{
				frameQuiet.OriginalFrame(nullptr);
				Assert::IsTrue(frameQuiet.TakeDecision().id.empty(), L"no frame-quiet decision on an Original frame");
			}
			Assert::AreEqual(before, frameQuiet.Flushes(), L"no flush on an Original frame (never armed, never ended)");
			Assert::AreEqual<unsigned long long>(0, frameQuiet.MissedEnds(), L"no missed End is counted in Original");
			for (int i = 0; i < 50; ++i) { frameQuiet.Arm(nullptr); FrameQuiet::Deferred::EndThisFrame(); }   // a VP-pathway frame: armed, nothing pending, no copies
			Assert::AreEqual(before, frameQuiet.Flushes(), L"no flush on an armed frame with no VP native work (VP pathway)");
			Assert::AreEqual<unsigned long long>(0, frameQuiet.MissedEnds());
		}

		// C1 landing gate MUST-1: the REAL Original call pattern. PlanEntrance runs on every Original frame, but the R12
		// AfterPresent seam (the only End) is skipped there, so an Arm on an Original frame is never ended: from frame 2 on
		// each one counted a missed End and engaged "native readback" with a reason that changed every frame (a fallbacks
		// log line and a VpEvent per frame). Bill's pathway has no such entry. Pinned two ways: the planner's frame-quiet
		// prologue keeps the Arm and the decision out of Original and releases the entry there; and over 50 Original
		// frames (Begin with frame_bound off, the release, never an End) nothing is missed, flushed or re-engaged.
		TEST_METHOD(OriginalFramesNeverArmSoNoMissedEndAndNoReadbackStatusChurn)
		{
			const std::string planner = ReadText(RepositoryRoot() + "\\src\\VP-V\\segments\\EntrancePlanner.inl");
			Assert::IsFalse(planner.empty(), L"the planner source");
			const size_t from = planner.find("FrameQuiet::Deferred::ForThisThread();");
			const size_t to = planner.find("// Stage 12 step A: the re-base factor for this frame");
			Assert::IsTrue(from != std::string::npos && to != std::string::npos && from < to, L"the frame-quiet prologue");
			const std::string prologue = planner.substr(from, to - from);
			const size_t original = prologue.find("if (pipelineSlots.OriginalPathway())");
			const size_t vpBranch = original == std::string::npos ? std::string::npos : prologue.find("\n\telse", original);
			const size_t arm = prologue.find("frameQuiet.Arm(");
			const size_t take = prologue.find("frameQuiet.TakeDecision()");
			Assert::IsTrue(original != std::string::npos && vpBranch != std::string::npos && arm != std::string::npos &&
				take != std::string::npos && vpBranch < arm && vpBranch < take,
				L"Arm and TakeDecision run only outside the Original pathway (the seam that ends an armed frame is skipped there)");
			const std::string originalBranch = prologue.substr(original, vpBranch - original);
			Assert::IsTrue(originalBranch.find("SyncFallback(FallbackCatalog::NativeReadback(0, 0))") != std::string::npos,
				L"Original releases the native readback entry (Bill has none)");
			Assert::AreEqual<size_t>(0, CountOf(originalBranch, "TakeDecision"), L"no frame-quiet decision is taken in Original");

			FallbackStatus::Registry fallbacks;
			Assert::IsTrue(FallbackCatalog::Sync(fallbacks, FallbackCatalog::NativeReadback(0, 1)), L"a VP frame before the switch left it engaged");
			FrameQuiet::Deferred quiet;
			Monitor m;
			const pl_frame f = NoPlanes();
			size_t headingChanges = 0;
			for (uint64_t s = 1; s <= 50; ++s)
			{
				quiet.Begin(m, nullptr, f, s, 24.0, false, static_cast<double>(s) / 24.0);   // frame_bound off in Original: inline, nothing pending
				if (FallbackCatalog::Sync(fallbacks, FallbackCatalog::NativeReadback(0, 0))) ++headingChanges;   // the Original release
			}
			Assert::AreEqual<size_t>(1, headingChanges, L"released once at the switch, then no event and no log line per frame");
			Assert::IsFalse(fallbacks.Engaged("native readback"));
			Assert::AreEqual<unsigned long long>(0, quiet.MissedEnds(), L"no missed End is counted in Original");
			Assert::AreEqual<unsigned long long>(0, quiet.Flushes());
			Assert::IsTrue(quiet.TakeDecision().id.empty(), L"no End ran, so no decision exists");
			Assert::AreEqual(-1.0, quiet.Seconds(), L"wall-clock monitor mode, as before C1");
		}

		// C1 gate MUST-1, the entry the Original branch calls: 50 Original frames (OriginalFrame + Begin, never an End) count no
		// missed End and produce no decision; a VP frame still armed at a live switch is ended once and not counted; the next VP
		// frame arms afresh (no missed End owed from Original).
		TEST_METHOD(OriginalFrameOpensNoWindowAndALiveSwitchEndsTheLastVpFrameOnce)
		{
			FrameQuiet::Deferred quiet;
			Monitor m;
			const pl_frame f = NoPlanes();
			for (uint64_t s = 1; s <= 50; ++s)
			{
				quiet.OriginalFrame(nullptr);
				quiet.Begin(m, nullptr, f, s, 24.0, false, static_cast<double>(s) / 24.0);
				Assert::IsTrue(quiet.TakeDecision().id.empty(), L"no decision in Original");
			}
			Assert::AreEqual<unsigned long long>(0, quiet.MissedEnds());
			Assert::AreEqual<unsigned long long>(0, quiet.Flushes());
			quiet.Arm(nullptr);   // a VP frame, then the switch to Original before its seam ran
			quiet.OriginalFrame(nullptr);
			Assert::AreEqual<unsigned long long>(0, quiet.MissedEnds(), L"the switch is not a missed End");
			Assert::IsFalse(LedDrive::DeferredCopies::ForThisThread().Armed(), L"the last VP frame was ended (its copies issued)");
			Assert::IsTrue(quiet.TakeDecision().id.empty(), L"its decision is dropped: Original shows no native readback entry");
			quiet.Arm(nullptr);   // back to VP
			Assert::AreEqual<unsigned long long>(0, quiet.MissedEnds(), L"no End is owed from an Original frame");
			quiet.EndFrame();
		}

		// C1 gate SHOULD-5: counted through Poll's flush rule (ReadyGate::PollFlushes, the exact condition Poll uses before
		// immediate->Flush()): over 50 Original frames, each with one waiting monitor tile copy, VP flushes nothing (as integ
		// 2e77256c7 and 4aac4f63c). Without the hold (unarmed, the state MUST-1 alone left) it would be one flush per copy.
		// A VP frame (Arm) ends the hold; armed frames never flush in Poll.
		TEST_METHOD(OriginalFramesIssueNoPollFlushAndAVpFrameEndsTheHold)
		{
			using LedDrive::ReadyGate;
			LedDrive::DeferredCopies& deferral = LedDrive::DeferredCopies::ForThisThread();
			FrameQuiet::Deferred quiet;
			struct EndHold { FrameQuiet::Deferred& q; ~EndHold() { q.Arm(nullptr); q.EndFrame(); } } endHold{ quiet };   // never leak the hold into later tests on this thread
			Assert::IsTrue(ReadyGate::PollFlushes(ReadyGate::Step::FenceWaiting, false, false, false),
				L"unarmed and not held: one flush per waiting copy (tests, other hosts; the state MUST-1 alone left in Original)");
			int originalFlushes = 0;
			for (int frame = 0; frame < 50; ++frame)
			{
				quiet.OriginalFrame(nullptr);
				if (ReadyGate::PollFlushes(ReadyGate::Step::FenceWaiting, false, deferral.Armed(), deferral.PollFlushHeld())) ++originalFlushes;
			}
			Assert::AreEqual(0, originalFlushes, L"no VP flush in Original");
			quiet.Arm(nullptr);
			Assert::IsFalse(deferral.PollFlushHeld(), L"a VP frame ends the hold");
			Assert::IsFalse(ReadyGate::PollFlushes(ReadyGate::Step::FenceWaiting, false, deferral.Armed(), deferral.PollFlushHeld()),
				L"armed: the host flushes after the submit, never Poll");
			quiet.EndFrame();
			Assert::IsTrue(ReadyGate::PollFlushes(ReadyGate::Step::FenceWaiting, false, deferral.Armed(), deferral.PollFlushHeld()),
				L"after the VP End (unarmed, not held): Poll's one-shot flush is back, as at 4aac4f63c");
			Assert::IsFalse(ReadyGate::PollFlushes(ReadyGate::Step::FenceWaiting, true, false, false), L"once per copy");
			Assert::IsFalse(ReadyGate::PollFlushes(ReadyGate::Step::MapProbe, false, false, false), L"only a waiting copy");
			Assert::IsFalse(ReadyGate::PollFlushes(ReadyGate::Step::NotIssued, false, false, false));
		}

		// C1 gate SHOULD-4: a frame whose post-frame end never ran is named as such, not as copies issued late.
		TEST_METHOD(AMissedEndIsNamedApartFromLateCopies)
		{
			const FallbackCatalog::Decision missed = FallbackCatalog::NativeReadback(0, 0, 0, 2);
			Assert::IsTrue(missed.engaged);
			Assert::IsTrue(missed.reason.find("2 frame(s) skipped the post-frame end") != std::string::npos, L"named");
			Assert::IsTrue(missed.reason.find("readback copy") == std::string::npos, L"no copy is claimed");
			const FallbackCatalog::Decision both = FallbackCatalog::NativeReadback(0, 1, 0, 1);
			Assert::IsTrue(both.reason.find("1 readback copy") != std::string::npos && both.reason.find(" and 1 frame(s) skipped") != std::string::npos);
			Assert::IsTrue(both.reason.find("; ") == std::string::npos, L"the catalog splits on a top-level semicolon");
			FrameQuiet::Deferred quiet;
			quiet.Arm(nullptr);
			quiet.Arm(nullptr);   // the seam was skipped once (a VP pathway)
			quiet.EndFrame();
			const FallbackCatalog::Decision d = quiet.TakeDecision();
			Assert::IsTrue(d.engaged, L"still visible in VP pathways");
			Assert::IsTrue(d.reason.find("1 frame(s) skipped the post-frame end") != std::string::npos, L"worded as a missed End");
		}

		// Negative look-ahead: the shot after the cut is native (ratio 1.0), so the cut does NOT engage; it is cancelled and counted.
		TEST_METHOD(ALookAheadThatSeesANativeNextShotCancelsTheEngageAtTheCut)
		{
			const BoundRun r = RunBoundEngage(131, 7, 1.0, 140);   // stops 9 frames past the cut
			Assert::AreEqual<unsigned long long>(0, r.engagedAt, L"no engage on or after the cut");
			Assert::AreEqual<unsigned long long>(1, r.cancelled, L"the cancel is counted");
			Assert::AreEqual<unsigned long long>(0, r.verified);
			Assert::AreEqual<unsigned long long>(0, r.reactive, L"and no reactive engage replaced it");
		}

		// End once per Begin: the second End of a frame is a no-op, and a frame whose End never came (a throw / cancel before the
		// AfterPresent seam) is ended exactly once by the next Arm and counted, never submitted twice.
		TEST_METHOD(EachBeginIsSubmittedExactlyOnceIncludingAnAbandonedFrame)
		{
			Monitor m;
			FrameQuiet::Deferred d;
			const pl_frame f = NoPlanes();
			d.Arm(nullptr);
			d.Begin(m, nullptr, f, 1, 24.0, true, 0.0);
			d.EndFrame();
			d.EndFrame();   // a second End
			Assert::AreEqual<unsigned long long>(1, m.SubmitSkipped(), L"one submit for the one Begin");
			d.Arm(nullptr);
			d.Begin(m, nullptr, f, 2, 24.0, true, 0.0);
			d.Arm(nullptr);   // the frame was abandoned (a throw / cancel before the seam)
			Assert::AreEqual<unsigned long long>(1, d.MissedEnds(), L"the abandoned frame is counted");
			Assert::AreEqual<unsigned long long>(2, m.SubmitSkipped(), L"made up once, outside the new frame");
			d.EndFrame();
			d.EndFrame();
			Assert::AreEqual<unsigned long long>(2, m.SubmitSkipped(), L"nothing is submitted twice");
		}
	};
}
