// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// W1-C2a A6 (doc 88): the ONE end-to-end resync injection test. The real ResyncExtension seals the
// central Indicator; a real VectorEngine is registered under the same host, so the "motion" FrameSeal
// provider binds its validity and registers the motion readers. A FULL (resync), a seek (FULL from a
// file discontinuity) and a gap are injected the way the host does (the FULL on the Indicator, the
// request through VectorEngine::RequestInvalidationFor), with resync_event on AND off. Per kind:
// the pair spanning the event is refused (FlowOf false), FieldEra moves (at the seal when on, at once
// when off), the gap seed is refused for a full / seek and kept for a pure gap. CPU only.
// Stage 2 (MUST 1) adds, per consumer of the event: the skeleton lane's regulator at T0 (ConservativeFloor) and the ahead
// product refused by its stamp; the camera fit's P2 provenance refused at the claim (StaleP2) while the lane generation stays
// (no StaleLane on the resync path); the de-judder snapshot lease dropped at the apply point (A2); the motion record published
// absent for the spanning pair; the guidance adapter's OnDiscontinuity (A3). Plus the no-engine status (MUST 2) and a deferred
// request applied at unbind (SHOULD 5).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/control/skeleton/SkeletonModule.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/consumers/DejudderHistory.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/MotionRecordPublish.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraExtension.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLinkCore.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraSettings.h>
#include <VP-V/modules/motion/estimation/global_model/link/GuidanceAdapter.h>
#include <VP-V/modules/motion/estimation/tests/CameraLinkTestSupport.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace ResyncInjectionA6
	{
		namespace RE = RendererExtension;
		using Vpv::Resync::ResyncExtension;
		using MVCause = MotionValidity::Cause;

		struct FakeRenderer
		{
			VpResync::Shadow resync;
			VpResync::Drive resyncDrive;
			Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
		};

		enum class Kind { Full, Seek, Gap };

		MotionAhead::FlowPtr MakeFlow()
		{
			auto flow = std::make_shared<MotionAhead::Flow>();
			flow->halfW = 16; flow->halfH = 8; flow->gridW = 4; flow->gridH = 2;
			for (int i = 0; i < 8; ++i) { flow->forward.push_back(64); flow->forward.push_back(-32); }
			return flow;
		}

		// The renderer in miniature: the extension, the renderer's Shadow / Drive and the engine of the same host.
		struct Rig
		{
			ResyncExtension ext;
			FakeRenderer r;
			VpMotion::VectorEngine engine{ &r };

			explicit Rig(const char* mode)
			{
				engine.PublishProducer(std::make_shared<MotionAhead::Producer>());
				engine.NoteFlowSourceWidth(64);
				Apply(mode);
			}
			void Apply(const char* mode)
			{
				RE::LifecycleFacts f;
				f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0.0;
				ext.OnLifecycle(f);
			}
			void Ingress(int64_t seq)
			{
				RE::IngressContext c;
				c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
				c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
				ext.OnIngress(c);
			}
			// The render loop's dequeue of seq: the frame's seal (resync on only) and the kept-run note.
			void Render(int64_t seq)
			{
				RE::FrameContext c; c.host = &r; c.phase = RE::FramePhase::Begin; c.seq = seq;
				Assert::IsFalse(ext.OnFrame(c));
				engine.motionValidity.NoteKept(static_cast<uint64_t>(seq));
			}
			bool Flow(uint64_t to)
			{
				VpMotion::FlowView view;
				return engine.FlowOf(to, view);
			}
			// The host's injection of `kind` carried by frame `at` (queued, not yet rendered).
			void Inject(Kind kind, uint64_t at)
			{
				const std::shared_ptr<VpIndicator::Indicator> ind = r.resync.GetIndicator();
				if (kind != Kind::Gap && ind && r.resync.Driving())
					ind->Advance(VpIndicator::kFull, kind == Kind::Seek ? VpResync::Cause::FileDiscontinuity : VpResync::Cause::RendererReset, at);
				const MVCause cause = kind == Kind::Full ? MVCause::Resync : kind == Kind::Seek ? MVCause::Seek : MVCause::Gap;
				Assert::IsTrue(VpMotion::VectorEngine::RequestInvalidationFor(&r, cause, kind == Kind::Gap ? "missed frames" : nullptr));
			}
		};

		std::string GapSeedWhy(const VpMotion::VectorEngine& engine, uint64_t pairEpoch)
		{
			const float global[2][4] = { { 1.0f, 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f, 1.0f } };
			const VpMotion::GapSeed s = VpMotion::GapSeed::Make(global, 4, 4, 6, pairEpoch, engine.FieldEra(),
				engine.motionValidity.LastCause() == MVCause::Gap);
			return s.why;
		}

		const char* const kMoved = "the vectors-valid state moved for more than the gap";

		// One kind, one mode: frames 1..5 enqueued, 1..4 rendered (kept), the event carried by queued frame 5, then 5 rendered.
		void Run(Kind kind, bool on)
		{
			Rig rig(on ? "on" : "off");
			for (int64_t s = 1; s <= 5; ++s) rig.Ingress(s);
			for (int64_t s = 1; s <= 4; ++s) rig.Render(s);
			Assert::AreEqual(on, VpMotion::VectorEngine::ResyncBoundFor(&rig.r), L"the motion provider binds only while resync_event drives");
			Assert::AreEqual(on, rig.engine.motionValidity.Driven());
			// The producer keys the slot to the stream generation of its jobs (MotionAheadWalk: StreamGeneration()) before it
			// publishes (C2a-F 2: FlowOf refuses a pair of another stream generation; binding "on" has already moved it here).
			rig.engine.p2Slot->SetGeneration(rig.engine.motionValidity.StreamGeneration());
			rig.engine.p2Slot->Publish(3, 4, rig.engine.p2Slot->Generation(), MakeFlow());
			rig.engine.p2Slot->Publish(4, 5, rig.engine.p2Slot->Generation(), MakeFlow());
			if (!rig.Flow(5))
			{
				const VpMotion::VectorEngine::FlowReadCounters c = rig.engine.FlowReads();
				const std::wstring m = L"before the event the pair (4, 5) is served: noProducer " + std::to_wstring(c.noProducer) +
					L" noPair " + std::to_wstring(c.noPair) + L" invalidated " + std::to_wstring(c.invalidated) + L" unscaled " +
					std::to_wstring(c.unscaled) + L" epoch " + std::to_wstring(rig.engine.motionValidity.Epoch()) + L" keptEpoch " +
					std::to_wstring(rig.engine.motionValidity.EpochOfLastKept());
				Assert::Fail(m.c_str());
			}
			const uint64_t era = rig.engine.FieldEra();
			const unsigned acts = rig.engine.motionResyncReaders.HistoryActs();

			rig.Inject(kind, 5);
			const bool deferred = on && kind != Kind::Gap;
			Assert::AreEqual(deferred ? era : era + 1, rig.engine.FieldEra(), L"FieldEra: deferred to the seal when on (full / seek), at once otherwise");
			Assert::IsFalse(rig.Flow(5), L"the pair spanning the event is refused at once (stamp when on, epoch when off)");

			rig.Render(5);
			Assert::AreEqual(era + 1, rig.engine.FieldEra(), L"one epoch move per event");
			// After frame 5 is kept in the new epoch the epoch form alone (EpochOfLastKept) no longer refuses (4, 5): when on, the
			// FULL stamp still does; on AND off, the pair's stream generation does (C2a-F 2: FlowOf refuses a pair stored under an
			// older StreamGeneration before the producer's asynchronous SetGeneration re-keys the slot). A gap moves no stream
			// generation by design (the ahead pairs after it are kept; the producer's breakBefore makes no pair across it).
			if (kind != Kind::Gap)
				Assert::IsFalse(rig.Flow(5), L"the pair published before the event is refused after the next kept frame (generation)");
			if (deferred)
			{
				Assert::IsFalse(rig.Flow(5), L"the FULL stamp refuses the spanning pair after the seal too");
				Assert::AreEqual(acts + 1u, rig.engine.motionResyncReaders.HistoryActs(), L"the motion-history reader fired at the carrying frame");
				const VpMotionResync::HistoryPending pending = rig.engine.motionResyncReaders.TakeHistory();
				Assert::IsTrue(pending.changed);
				if (kind == Kind::Full)
					Assert::IsTrue(pending.release, L"a resync releases resources");
			}
			const std::string why = GapSeedWhy(rig.engine, era);
			if (kind == Kind::Gap)
				Assert::AreNotEqual(std::string(kMoved), why, L"a pure gap keeps the seed's epoch check");
			else
				Assert::AreEqual(std::string(kMoved), why, L"a full / seek refuses the gap seed");
		}
	}

	namespace ResyncInjectionA6
	{
		namespace Tap = VpFrameAnalysis::SourceFrameTap;
		namespace CLT = VideoProcessorTest::CameraLinkTest;
		using Vpv::Camera::CameraExtension;
		using Vpv::Skeleton::SkeletonModule;

		bool HasPart(const std::vector<std::string>& lines, const char* part)
		{
			for (const std::string& l : lines)
				if (l.find(part) != std::string::npos)
					return true;
			return false;
		}
		bool HasId(const std::vector<std::string>& lines, const char* id)
		{
			const std::string prefix = std::string(id) + ": ";
			for (const std::string& l : lines)
				if (l.compare(0, prefix.size(), prefix) == 0)
					return true;
			return false;
		}

		struct TempCfg
		{
			std::string path;
			explicit TempCfg(const std::string& text)
			{
				static std::atomic<int> counter{ 0 };
				char dir[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, dir);
				path = std::string(dir) + "vp-resync-inject-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++) + ".cfg";
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

		// The host's FULL / seek on the Indicator (gap: none) and its request to the engine of `host`.
		void InjectOn(const void* host, VpResync::Shadow& shadow, Kind kind, uint64_t at)
		{
			const std::shared_ptr<VpIndicator::Indicator> ind = shadow.GetIndicator();
			if (kind != Kind::Gap && ind && shadow.Driving())
				ind->Advance(VpIndicator::kFull, kind == Kind::Seek ? VpResync::Cause::FileDiscontinuity : VpResync::Cause::RendererReset, at);
			const MVCause cause = kind == Kind::Full ? MVCause::Resync : kind == Kind::Seek ? MVCause::Seek : MVCause::Gap;
			Assert::IsTrue(VpMotion::VectorEngine::RequestInvalidationFor(host, cause, kind == Kind::Gap ? "missed frames" : nullptr));
		}

		// ---- (a) the skeleton lane: the regulator back at T0 and the ahead product refused by its stamp -----------------------
		constexpr uint32_t kW = 64;
		constexpr uint32_t kH = 48;

		struct LaneRig
		{
			TempCfg cfg;
			std::atomic<uint64_t> base{ 1000 };
			std::atomic<uint64_t> calls{ 0 };
			SkeletonModule sk;
			ResyncExtension resync;
			FakeRenderer r;
			VpMotion::VectorEngine engine{ &r };
			std::vector<uint16_t> store;

			explicit LaneRig(const char* mode)
				: cfg("[vpv.skeleton]\nanalysis_lane=on\n"), sk(cfg.Loader(), [this]() { return base.load() + calls.fetch_add(1); }),
				  store(size_t(16) * kW * kH)
			{
				uint32_t x = 777u;
				for (size_t i = 0; i < store.size(); ++i)
				{
					x = x * 1664525u + 1013904223u;
					store[i] = uint16_t((x >> 12) & 1023u);
				}
				VpSkeleton::CardProfile card;
				card.vramBudgetBytes = 1ull << 30;
				card.systemRamFreeBytes = 1ull << 30;
				card.uploadGBps = 10.0;
				sk.SetCardForTesting(card);
				sk.SetWorkerThreadsForTesting(false);
				RE::LifecycleFacts f;
				f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0;
				resync.OnLifecycle(f);
				Life(RE::Lifecycle::GpuCreated);
			}
			~LaneRig() { Life(RE::Lifecycle::GpuReleased); }
			void Life(RE::Lifecycle kind)
			{
				RE::LifecycleFacts f;
				f.host = &r; f.kind = kind; f.code = 0; f.value = 0;
				sk.OnLifecycle(f);
			}
			VpSkeleton::SourceFrameView View(uint64_t seq) const
			{
				VpSkeleton::SourceFrameView v;
				v.seq = seq;
				v.luma = store.data() + size_t(seq % 16) * kW * kH;
				v.width = kW;
				v.height = kH;
				v.strideSamples = kW;
				v.msbShift = 6;
				return v;
			}
			VpSkeleton::AnalysisLane* Lane() { return sk.LaneOf(&r); }
			// One rendered frame; `inject`: the event is carried by this frame (injected after its ingress, before its seal).
			void Frame(int64_t seq, bool inject = false, Kind kind = Kind::Full)
			{
				RE::IngressContext ic;
				ic.host = &r; ic.seq = seq; ic.queueGeneration = 1; ic.stateKey = 0x1000; ic.formatKey = 7;
				ic.frameCounter = seq; ic.timestamp = seq * 1000; ic.ticksPerSecond = 24000; ic.measuredRateHz = 24.0;
				resync.OnIngress(ic);
				if (seq == 1)
					Tap::Deliver(&r, View(1));
				Tap::Deliver(&r, View(uint64_t(seq) + 1));
				if (inject)
					InjectOn(&r, r.resync, kind, uint64_t(seq));
				base += 40000;
				RE::FrameContext c;
				c.host = &r; c.phase = RE::FramePhase::Begin; c.seq = seq;
				resync.OnFrame(c);
				Assert::IsFalse(sk.OnFrame(c));
				c.phase = RE::FramePhase::AfterRender;
				Assert::IsFalse(sk.OnFrame(c));
				engine.motionValidity.NoteKept(static_cast<uint64_t>(seq));
				if (VpSkeleton::AnalysisLane* lane = Lane())
				{
					int steps = 0;
					while (lane->StepWorker(VpSkeleton::Backend::Cpu, 0) && ++steps < 1000)
					{
					}
				}
			}
			size_t Floors()
			{
				std::vector<VpSkeleton::RegTrace> t(VpSkeleton::Regulator::kTraceCapacity);
				const size_t n = Lane()->RegulatorTraces(t.data(), t.size());
				size_t floors = 0;
				for (size_t i = 0; i < n; ++i)
					if (t[i].action == VpSkeleton::RegAction::ConservativeFloor)
						++floors;
				return floors;
			}
			uint64_t P1Invalid()
			{
				const std::shared_ptr<VpSkeleton::LaneState> st = Lane()->State();
				Assert::IsTrue(st != nullptr);
				const int idx = st->IndexOf(VpSkeleton::kP1HalfLuma);
				Assert::IsTrue(idx >= 0);
				return st->table->At(size_t(idx)).Counters().invalid.load();
			}
		};

		void LaneRun(Kind kind)
		{
			LaneRig rig("on");
			for (int64_t s = 1; s <= 5; ++s)
				rig.Frame(s);
			Assert::IsNotNull(rig.Lane(), L"the lane exists while resync_event drives");
			Assert::IsTrue(rig.Lane()->TotalAhead() > 0, L"the lane makes products ahead of their seal");
			const size_t floors = rig.Floors();
			const uint64_t invalid = rig.P1Invalid();
			const uint64_t generation = rig.Lane()->Generation();
			rig.Frame(6, true, kind);
			if (kind == Kind::Gap)
			{
				Assert::AreEqual(floors, rig.Floors(), L"a pure gap is no FULL: the regulator keeps its level");
				Assert::AreEqual(invalid, rig.P1Invalid(), L"... and the ahead product keeps its stamp");
			}
			else
			{
				Assert::IsTrue(rig.Floors() > floors, L"the FULL puts the regulator back at T0 (ConservativeFloor)");
				Assert::IsTrue(rig.P1Invalid() > invalid, L"the product made ahead of the FULL is refused by its stamp at the seal");
			}
			Assert::AreEqual(generation, rig.Lane()->Generation(), L"no lane rebuild: StaleLane is not the resync path");
			rig.Frame(7);
			Assert::IsTrue(rig.Lane()->Active(), L"the lane carries on after the event");
		}

		// ---- (b) the camera fit: the P2 provenance served before the event is refused at the claim (StaleP2) -----------------
		VpSkeleton::LaneLeaseSource SourceOf(CLT::Rig& r)
		{
			VpSkeleton::SlotTable* t = r.table.get();
			VpSkeleton::CpuBuffers* b = r.bytes.get();
			VpIndicator::Indicator* i = r.indicator.get();
			return [t, b, i]() { return VpSkeleton::LaneLease::Borrow(t, b, i); };
		}
		std::shared_ptr<CameraLink::FieldCopy> AheadField(uint64_t seq, uint64_t p2Epoch)
		{
			auto f = CLT::PanJob(seq, 4.0).field;
			auto c = std::make_shared<CameraLink::FieldCopy>(*f);
			c->origin = CameraLink::FieldOrigin::Ahead;
			c->haveP2 = true;
			c->p2Epoch = p2Epoch;
			return c;
		}

		// Frames 1..5 enqueued, 1..4 rendered, the event carried by 5, then 5 rendered: the epoch before and after the event.
		uint64_t EventAt5(Rig& rig, Kind kind, uint64_t& era)
		{
			for (int64_t s = 1; s <= 5; ++s) rig.Ingress(s);
			for (int64_t s = 1; s <= 4; ++s) rig.Render(s);
			era = rig.engine.FieldEra();
			rig.Inject(kind, 5);
			rig.Render(5);
			Assert::AreEqual(era + 1, rig.engine.FieldEra());
			return rig.engine.FieldEra();
		}

		void CameraRun(Kind kind, bool on)
		{
			Rig rig(on ? "on" : "off");
			uint64_t era = 0;
			const uint64_t now = EventAt5(rig, kind, era);
			CLT::Rig bridge, onTime;
			FrameAnalysis::Store store;
			for (uint64_t seq = 1; seq <= 4; ++seq)
			{
				FrameAnalysis::Identity id;
				id.seq = seq; id.width = 64; id.height = 36;
				store.Begin(id, FrameAnalysis::kCamera);
			}
			CameraLink::CameraLinkCore core(store);
			CameraLink::CameraLinkSettings s;
			s.p7Wanted = true;
			s.aheadFit = true;
			core.OnSettings(s);
			core.SetAheadSource([](uint64_t, CameraLink::AheadCopy&) { return CameraLink::AheadRefusal::NoPair; }, CameraLink::PairFactsProvider());
			VpMotion::VectorEngine* engine = &rig.engine;
			core.SetP2EpochSource([engine]() { return engine->FieldEra(); });   // as CameraExtension wires it (IVectorEngine::FieldEra)
			core.AttachOnTime(SourceOf(onTime), "test lane");
			core.AttachGraph(bridge.table.get(), bridge.bytes.get(), bridge.indicator.get());
			CameraLink::FramePhaseFacts f;
			f.seq = f.renderSeq = 1;
			core.OnFrameStart(f);
			onTime.PublishInputs(1);
			onTime.Retire(1);
			onTime.PublishInputs(2);
			onTime.PublishInputs(3);
			Assert::IsTrue(core.SubmitAheadField(AheadField(2, era)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().p2RefusedClaim.load(), L"P2 served before the event: StaleP2 at the claim");
			Assert::AreEqual(uint64_t(0), core.Counters().aheadOnTime.load());
			Assert::IsTrue(core.SubmitAheadField(AheadField(3, now)));
			core.Worker()->WaitIdle();
			Assert::AreEqual(uint64_t(1), core.Counters().p2RefusedClaim.load(), L"control: the new epoch's P2 is not refused");
			Assert::AreEqual(uint64_t(1), core.Counters().aheadOnTime.load());
		}

		// ---- (c) A2: the de-judder snapshot lease is dropped at the apply point (the renderer's ApplyMotionInvalidate) --------
		// The renderer's hook (LVR ApplyMotionInvalidate): when TakeDejudderHistoryAction is true, dejudder->InvalidateSnapshot() and
		// vectorEstimate.ResetSnapshot(). DejudderWarp::InvalidateSnapshot needs a GPU (FieldSourceShimTests); here the CPU half.
		bool ApplyPoint(Rig& rig, VpMotion::EstimateState<int>& state)
		{
			MotionValidity::Pending pending;
			const bool moved = rig.engine.TakeInvalidation(pending);
			const bool act = VpMotion::TakeDejudderHistoryAction(rig.r.resync.Driving(), moved, pending, rig.engine.motionResyncReaders);
			if (act)
				state.ResetSnapshot();
			return act;
		}

		void SnapshotRun(Kind kind, bool on)
		{
			Rig rig(on ? "on" : "off");
			VpMotion::EstimateState<int> state(rig.engine);
			for (int64_t s = 1; s <= 5; ++s) rig.Ingress(s);
			for (int64_t s = 1; s <= 4; ++s) { rig.Render(s); ApplyPoint(rig, state); }
			Dejudder::IFieldSource::Set copies;
			state.CaptureSnapshot(copies, 7);
			Dejudder::IFieldSource::Set got;
			Assert::IsTrue(state.SnapshotFieldSet(7, got) == Dejudder::IFieldSource::SnapshotState::Valid, L"a snapshot lease before the event");
			Assert::IsFalse(ApplyPoint(rig, state), L"nothing moved: the lease stays");
			rig.Inject(kind, 5);
			rig.Render(5);
			Assert::IsTrue(ApplyPoint(rig, state), L"the epoch move acts at the apply point (every kind, every mode)");
			Assert::IsTrue(state.SnapshotFieldSet(7, got) == Dejudder::IFieldSource::SnapshotState::None, L"the lease is invalidated");
			uint64_t from = 1, to = 1;
			state.SnapshotPair(from, to);
			Assert::AreEqual(uint64_t(0), from);
			Assert::AreEqual(uint64_t(0), to);
			Assert::IsFalse(state.SnapshotLeaseLost(), L"a reset, not a lost read");
		}

		// ---- (d) the motion record: the spanning pair is published ABSENT (ServeStatistics' gate, VectorEngine.cpp) -----------
		FrameAnalysis::Identity IdOf(uint64_t seq)
		{
			FrameAnalysis::Identity id;
			id.seq = seq; id.width = 1920; id.height = 1080;
			return id;
		}

		void RecordRun(Kind kind, bool on)
		{
			Rig rig(on ? "on" : "off");
			for (int64_t s = 1; s <= 5; ++s) rig.Ingress(s);
			for (int64_t s = 1; s <= 4; ++s) rig.Render(s);
			const MotionValidity::State& mv = rig.engine.motionValidity;
			auto gate = [&mv](uint64_t from, uint64_t to) {
				return mv.PairServes(from, to, mv.EpochOfLastKept(), mv.Epoch());   // C2a-F S1: the production gate itself, not a copy
			};
			const float global[4] = { 2.5f, -0.5f, 0.2f, 1.0f };
			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.Begin(IdOf(4), FrameAnalysis::kMotion);
			store.Begin(IdOf(5), FrameAnalysis::kMotion);
			Assert::IsTrue(gate(4, 5), L"before the event the pair (4, 5) passes the publish gate");
			Assert::IsTrue(VpMotion::MotionRecord::Publish(&store, 4, VpMotion::MotionRecord::SummaryOf(gate(3, 4), 3, 4, global, 1)));
			rig.Inject(kind, 5);
			Assert::IsFalse(gate(4, 5), L"the pair spanning the event fails the gate");
			Assert::IsTrue(VpMotion::MotionRecord::Publish(&store, 5, VpMotion::MotionRecord::SummaryOf(gate(4, 5), 4, 5, global, 1)));
			FrameAnalysis::Record rec;
			Assert::IsTrue(store.Read(4, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kMotion, rec) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(rec.motion.vectorsValid, L"control: the pair before the event carries the global");
			Assert::IsTrue(store.Read(5, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kMotion, rec) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsFalse(rec.motion.vectorsValid, L"the spanning pair is published absent");
			Assert::IsTrue(rec.motion.source == FrameAnalysis::VectorSource::None);
			Assert::AreEqual(uint64_t(0), rec.motion.pairFrom);
		}

		// ---- (e) A3: the guidance adapter forgets its pass at every FieldEra change (resync on only, S4) ------------------------
		struct CamRenderer
		{
			VpResync::Shadow resync;
			VpResync::Drive resyncDrive;
			Vpv::Resync::HostBinding binding{ this, resync, resyncDrive };
			FrameAnalysis::Store store{ FrameAnalysis::RingDepth(4, 4) };
		};

		// The camera tests' fake engine (CameraLiveWiringTests), with the validity epoch of a real VectorEngine of the same host.
		struct EraEngine : VpMotion::IVectorEngine
		{
			const VpMotion::IVectorEngine* real = nullptr;
			uint64_t latest = 8;
			VpMotion::IPreEstimateHook* hook = nullptr;

			uint64_t LatestVerifiedFieldSeq() const override { return latest; }
			uint64_t FieldEra() const override { return real->FieldEra(); }
			bool FieldOf(uint64_t seq, VpMotion::VerifiedField& out) const override
			{
				if (seq == 0 || seq > latest)
					return false;
				out = VpMotion::VerifiedField();
				out.seq = seq; out.pairFrom = seq - 1; out.pairTo = seq;
				out.gridW = 120; out.gridH = 68; out.blockSize = 16;
				return true;
			}
			float FieldScale() const override { return 1.0f; }
			bool SeedFieldOf(uint64_t seq, VpMotion::SeedField& out) const override
			{
				out = VpMotion::SeedField();
				if (seq == 0 || seq > latest)
					return false;
				out.seq = seq; out.pairFrom = seq - 1; out.pairTo = seq;
				out.gridW = 120; out.gridH = 68; out.blockSize = 16;
				out.scaleToSourcePx = 2.0f;
				out.sourceGridW = 120; out.sourceGridH = 68;
				out.sadTrustMax = 3.0f;
				out.consistencyPx = 1.5f;
				return true;
			}
			bool GlobalFromEngine() const override { return false; }
			bool EngineGlobal(uint64_t, VpMotion::GlobalVec&) const override { return false; }
			bool PairSpansSlip(uint64_t, uint64_t) const override { return false; }
			bool FlowIsCpuResident() const override { return false; }
			bool FlowOf(uint64_t, VpMotion::FlowView&) const override { return false; }
			void SetPreEstimateHook(VpMotion::IPreEstimateHook* h) override { hook = h; }
		};

		struct EraDirectory : VpMotion::IVectorEngineDirectory
		{
			VpMotion::IVectorEngine* engine = nullptr;
			const void* host = nullptr;
			VpMotion::IVectorEngine* EngineFor(const void* h) override { return h == host ? engine : nullptr; }
		};

		struct CamRig
		{
			TempCfg cfg;
			EraEngine engine;
			EraDirectory dir;
			CameraExtension ext;
			ResyncExtension resync;
			CamRenderer r;
			VpMotion::VectorEngine real{ &r };   // the host's real engine: the "motion" provider binds its validity

			CamRig() : cfg("[vpv.skeleton]\ncamera_link=report\n[vpv.camera]\ncamera_model=on\ncamera_meas_model_global=on\n"),
				ext(cfg.Loader(), [this]() -> VpMotion::IVectorEngineDirectory* { return &dir; })
			{
				ext.SetGuidanceLockForTesting(false);
				dir.host = &r;
				dir.engine = &engine;
				engine.real = &real;
				Tap::SetStore(&r, &r.store);
				RE::LifecycleFacts f;
				f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = "on"; f.code = 0; f.value = 0;
				resync.OnLifecycle(f);
				Life(RE::Lifecycle::GpuCreated);
			}
			~CamRig()
			{
				Life(RE::Lifecycle::GpuReleased);
				Tap::SetStore(&r, nullptr);
			}
			void Life(RE::Lifecycle kind)
			{
				RE::LifecycleFacts f;
				f.host = &r; f.kind = kind; f.code = 0;
				ext.OnLifecycle(f);
			}
			void Frame(int64_t seq, bool inject = false, Kind kind = Kind::Full)
			{
				RE::IngressContext c;
				c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
				c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
				resync.OnIngress(c);
				if (inject)
					InjectOn(&r, r.resync, kind, uint64_t(seq));
				RE::FrameContext f;
				f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
				resync.OnFrame(f);
				Assert::IsFalse(ext.OnFrame(f));
				f.phase = RE::FramePhase::AfterRender;
				Assert::IsFalse(ext.OnFrame(f));
				real.motionValidity.NoteKept(static_cast<uint64_t>(seq));
			}
		};

		// ---- MUST 2: a renderer with no vector engine is shown (Lines events + Fallbacks), and the binding returns ------------
		struct NoEngineRig
		{
			ResyncExtension ext;
			FakeRenderer r;
			std::unique_ptr<VpMotion::VectorEngine> engine;

			void Apply(const char* mode)
			{
				RE::LifecycleFacts f;
				f.host = &r; f.kind = RE::Lifecycle::SettingsApplied; f.subject = mode; f.code = 0; f.value = 0.0;
				ext.OnLifecycle(f);
			}
			void Frame(int64_t seq, bool full = false)
			{
				RE::IngressContext c;
				c.host = &r; c.seq = seq; c.queueGeneration = 1; c.stateKey = 0x1000; c.formatKey = 7;
				c.frameCounter = seq; c.timestamp = seq * 1000; c.ticksPerSecond = 24000; c.measuredRateHz = 24.0;
				ext.OnIngress(c);
				const std::shared_ptr<VpIndicator::Indicator> ind = r.resync.GetIndicator();
				if (full && ind)
					ind->Advance(VpIndicator::kFull, VpResync::Cause::RendererReset, uint64_t(seq));
				RE::FrameContext f; f.host = &r; f.phase = RE::FramePhase::Begin; f.seq = seq;
				Assert::IsFalse(ext.OnFrame(f));
			}
			std::vector<std::string> Lines(RE::StatusKind kind)
			{
				std::vector<std::string> lines;
				RE::Context c; c.host = &r;
				ext.OnStatus(c, kind, lines, nullptr, nullptr);
				return lines;
			}
		};
	}

	using namespace ResyncInjectionA6;

	TEST_CLASS(ResyncInjectionTests)
	{
	public:
		TEST_METHOD(FullOn) { Run(Kind::Full, true); }
		TEST_METHOD(FullOff) { Run(Kind::Full, false); }
		TEST_METHOD(SeekOn) { Run(Kind::Seek, true); }
		TEST_METHOD(SeekOff) { Run(Kind::Seek, false); }
		TEST_METHOD(GapOn) { Run(Kind::Gap, true); }
		TEST_METHOD(GapOff) { Run(Kind::Gap, false); }

		TEST_METHOD(OffThenOnRebindsTheSameEngine)
		{
			Rig rig("on");
			rig.Ingress(1); rig.Render(1);
			Assert::IsTrue(VpMotion::VectorEngine::ResyncBoundFor(&rig.r));
			rig.Apply("off");
			Assert::IsFalse(VpMotion::VectorEngine::ResyncBoundFor(&rig.r), L"resync_event off unbinds: immediate epochs again");
			const uint64_t era = rig.engine.FieldEra();
			Assert::IsTrue(VpMotion::VectorEngine::RequestInvalidationFor(&rig.r, MVCause::Resync, nullptr));
			Assert::AreEqual(era + 1, rig.engine.FieldEra());
			rig.Apply("on");
			rig.Ingress(2); rig.Render(2);
			Assert::IsTrue(VpMotion::VectorEngine::ResyncBoundFor(&rig.r), L"on again rebinds at the next frame");
		}

		// MUST 1 (a): the skeleton lane (resync on: the lane needs the shared seal; off has no lane).
		TEST_METHOD(LaneFullPutsTheRegulatorAtT0AndRefusesTheAheadProduct) { LaneRun(Kind::Full); }
		TEST_METHOD(LaneSeekPutsTheRegulatorAtT0AndRefusesTheAheadProduct) { LaneRun(Kind::Seek); }
		TEST_METHOD(LaneGapKeepsTheRegulatorAndTheProduct) { LaneRun(Kind::Gap); }
		TEST_METHOD(LaneOffHasNoLane)
		{
			LaneRig rig("off");
			rig.Frame(1);
			rig.Frame(2);
			Assert::IsNull(rig.Lane(), L"no lane without the shared seal");
		}

		// MUST 1 (b): the camera fit's P2 provenance (StaleP2).
		TEST_METHOD(CameraStaleP2FullOn) { CameraRun(Kind::Full, true); }
		TEST_METHOD(CameraStaleP2FullOff) { CameraRun(Kind::Full, false); }
		TEST_METHOD(CameraStaleP2SeekOn) { CameraRun(Kind::Seek, true); }
		TEST_METHOD(CameraStaleP2SeekOff) { CameraRun(Kind::Seek, false); }   // C2a-F S2
		TEST_METHOD(CameraStaleP2GapOn) { CameraRun(Kind::Gap, true); }
		TEST_METHOD(CameraStaleP2GapOff) { CameraRun(Kind::Gap, false); }

		// MUST 1 (c): A2, the snapshot lease, on every epoch move in every mode.
		TEST_METHOD(SnapshotLeaseFullOn) { SnapshotRun(Kind::Full, true); }
		TEST_METHOD(SnapshotLeaseFullOff) { SnapshotRun(Kind::Full, false); }
		TEST_METHOD(SnapshotLeaseSeekOn) { SnapshotRun(Kind::Seek, true); }
		TEST_METHOD(SnapshotLeaseSeekOff) { SnapshotRun(Kind::Seek, false); }   // C2a-F S2
		TEST_METHOD(SnapshotLeaseGapOn) { SnapshotRun(Kind::Gap, true); }
		TEST_METHOD(SnapshotLeaseGapOff) { SnapshotRun(Kind::Gap, false); }

		// MUST 1 (d): the motion record of the spanning pair is absent.
		TEST_METHOD(MotionRecordAbsentFullOn) { RecordRun(Kind::Full, true); }
		TEST_METHOD(MotionRecordAbsentFullOff) { RecordRun(Kind::Full, false); }
		TEST_METHOD(MotionRecordAbsentSeekOn) { RecordRun(Kind::Seek, true); }
		TEST_METHOD(MotionRecordAbsentGapOn) { RecordRun(Kind::Gap, true); }
		TEST_METHOD(MotionRecordAbsentSeekOff) { RecordRun(Kind::Seek, false); }   // C2a-F S2
		TEST_METHOD(MotionRecordAbsentGapOff) { RecordRun(Kind::Gap, false); }    // C2a-F S2

		// MUST 1 (e): A3, the guidance adapter's OnDiscontinuity at every FieldEra change (FULL at the seal, gap at once); resync on
		// only (S4): DriveFrame does not run the camera lane while resync is not Driving.
		TEST_METHOD(GuidanceAdapterForgetsAtEveryEraChange)
		{
			CamRig rig;
			for (int64_t s = 1; s <= 6; ++s)
				rig.Frame(s);
			Assert::IsTrue(VpMotion::VectorEngine::ResyncBoundFor(&rig.r), L"the host's engine is bound");
			CameraLink::CameraGuidanceAdapter* adapter = rig.ext.AdapterOf(&rig.r);
			Assert::IsNotNull(adapter, L"camera_link + camera_meas_model_global: the adapter exists");
			const uint64_t d0 = adapter->Discontinuities();
			const uint64_t era = rig.real.FieldEra();
			rig.Frame(7, true, Kind::Full);
			Assert::AreEqual(era + 1, rig.real.FieldEra());
			Assert::IsTrue(adapter == rig.ext.AdapterOf(&rig.r), L"the same adapter (a FULL is no new Indicator era)");
			Assert::AreEqual(d0 + 1, adapter->Discontinuities(), L"the FULL: OnDiscontinuity once");
			rig.Frame(8);
			Assert::AreEqual(d0 + 1, adapter->Discontinuities(), L"no epoch move: nothing");
			Assert::IsTrue(VpMotion::VectorEngine::RequestInvalidationFor(&rig.r, MVCause::Gap, "missed frames"));
			rig.Frame(9);
			Assert::AreEqual(d0 + 2, adapter->Discontinuities(), L"a gap moves FieldEra too: OnDiscontinuity again");
		}

		// MUST 2: an era with no vector engine is shown in the Lines events and the Fallbacks ("resync_motion: ").
		TEST_METHOD(AnEraWithoutAVectorEngineIsShown)
		{
			NoEngineRig rig;
			rig.Apply("on");
			rig.Frame(1);
			rig.Frame(2, true);
			rig.Frame(3);
			Assert::IsFalse(VpMotion::VectorEngine::ResyncBoundFor(&rig.r));
			Assert::IsTrue(HasId(rig.Lines(RE::StatusKind::Fallbacks), "resync_motion"), L"Fallbacks: resync_motion");
			Assert::IsTrue(HasPart(rig.Lines(RE::StatusKind::Lines), "era(s) with no vector engine"), L"Lines events: the count");
			// The engine comes; the next era binds it and the entry clears.
			rig.engine.reset(new VpMotion::VectorEngine(&rig.r));
			rig.Apply("off");
			rig.Apply("on");
			rig.Frame(4);
			Assert::IsTrue(VpMotion::VectorEngine::ResyncBoundFor(&rig.r), L"bound at the next era");
			Assert::IsFalse(HasId(rig.Lines(RE::StatusKind::Fallbacks), "resync_motion"), L"the entry clears once bound");
		}

		// SHOULD 5: a deferred request (bound, FULL / seek carried by a queued frame) and resync turned off before its seal: the
		// unbind applies it as the immediate move it would have been (never lost); counted in UnbindMoves.
		TEST_METHOD(ADeferredRequestIsAppliedAtUnbind)
		{
			const Kind kinds[] = { Kind::Full, Kind::Seek };
			for (Kind kind : kinds)
			{
				Rig rig("on");
				for (int64_t s = 1; s <= 5; ++s) rig.Ingress(s);
				for (int64_t s = 1; s <= 4; ++s) rig.Render(s);
				const uint64_t era = rig.engine.FieldEra();
				rig.Inject(kind, 5);
				Assert::AreEqual(era, rig.engine.FieldEra(), L"deferred to the seal while bound");
				rig.Apply("off");
				Assert::IsFalse(VpMotion::VectorEngine::ResyncBoundFor(&rig.r));
				Assert::AreEqual(era + 1, rig.engine.FieldEra(), L"applied at the unbind");
				Assert::AreEqual(uint64_t(1), rig.engine.motionValidity.UnbindMoves());
				rig.Render(5);
				Assert::AreEqual(era + 1, rig.engine.FieldEra(), L"once");
				Assert::IsFalse(rig.Flow(5), L"the pair spanning the event stays refused");
			}
		}

		// W1-C2a MUST 1: the gap-fill pair (no adjacency, DejudderPolicyRunner fillPair) never spans a FULL when on; a loss
		// (continuity) is what it bridges. Off: the stamps are not consulted (today's behaviour): SpanStatus is Unknown, which the
		// fill gate (SpanStatus != Invalid) does not refuse, and StampsAgree is false (C2a-F 1: never true unbound).
		TEST_METHOD(AGapFillPairNeverSpansAFull)
		{
			for (int on = 0; on < 2; ++on)
			{
				Rig rig(on != 0 ? "on" : "off");
				for (int64_t s = 1; s <= 8; ++s) rig.Ingress(s);
				for (int64_t s = 1; s <= 4; ++s) rig.Render(s);
				const std::shared_ptr<VpIndicator::Indicator> ind = rig.r.resync.GetIndicator();
				if (on != 0 && ind)
					ind->Advance(VpIndicator::kContinuity, VpResync::Cause::MissedFrames, 5, 1);
				using Span = MotionValidity::State::SpanState;
				const MotionValidity::State& mv = rig.engine.motionValidity;
				const Span known = on != 0 ? Span::Valid : Span::Unknown;
				Assert::IsTrue(mv.SpanStatus(4, 6, true) == known, L"a loss alone: the fill stands");
				Assert::AreEqual(on != 0, mv.StampsAgree(4, 6, true), L"StampsAgree: true only when the stamps vouch (on)");
				rig.Inject(Kind::Full, 7);
				Assert::IsTrue(mv.SpanStatus(6, 8, true) == (on != 0 ? Span::Invalid : Span::Unknown), L"across the queued FULL: refused when on");
				Assert::IsFalse(mv.StampsAgree(6, 8, true), L"across the queued FULL: never true (Invalid on, Unknown off)");
				Assert::IsTrue(mv.SpanStatus(4, 6, true) == known, L"before the FULL: unchanged");
			}
		}
	};
}
