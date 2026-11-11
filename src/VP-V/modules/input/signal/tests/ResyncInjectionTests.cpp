// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// W1-C2a A6 (doc 88): the ONE end-to-end resync injection test. The real ResyncExtension seals the
// central Indicator; a real VectorEngine is registered under the same host, so the "motion" FrameSeal
// provider binds its validity and registers the motion readers. A FULL (resync), a seek (FULL from a
// file discontinuity) and a gap are injected the way the host does (the FULL on the Indicator, the
// request through VectorEngine::RequestInvalidationFor), with resync_event on AND off. Per kind:
// the pair spanning the event is refused (FlowOf false), FieldEra moves (at the seal when on, at once
// when off), the gap seed is refused for a full / seek and kept for a pure gap. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

#include <cstring>
#include <memory>
#include <string>

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
			// FULL stamp still does; when off (and for a gap) only the producer's new generation (P2Slot::SetGeneration) drops it in the host: not in this rig (open item).
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
	};
}
